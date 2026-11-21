/*
  runtime.cpp - shared descriptor driven display owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file runtime.cpp
 * @brief Shared panel rendering and EPD scheduling over native display transports.
 * Descriptor and monochrome packing semantics adapted from Tasmota UDisplay,
 * Copyright (C) 2021 Gerhard Mutz and Theo Arends; GPL-3.0-or-later.
 */
#include "newmota/platform/display.h"
#include "newmota/platform/display/descriptor.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/display/native.h"
#include "newmota_features.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/i2c.h"
#include "newmota/platform/pwm.h"
#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "../esp-idf/spi_host.h"
#if defined(ESP_PLATFORM) && SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif
#if defined(ESP_PLATFORM) && SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#endif
#endif // ESP_PLATFORM
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>

namespace newmota::platform::display {
namespace {
using detail::Bus;
using detail::Config;
constexpr std::size_t kScratchSize = 4096;

/** @brief Owns all source, command, and transfer storage until restart. */
struct Owner {
    Config config;
    Info current;
    bool ready{}, faulted{}, dirty{}, enabled{true}, inverted{};
    bool epd_initialized{}, epd_has_refreshed{}, refresh_pending{}, pending_partial{},
        active_partial{}, frame_inverted{}, framebuffer_shared{};
    enum class EpdPhase { reset, init, idle, refresh, finish } epd_phase{EpdPhase::reset};
    const detail::Bytes* sequence_bytes{};
    std::size_t sequence_at{}, epd_frame_offset{};
    int epd_frame_opcode{-1}, epd_frame_stage{}, epd_refresh_stage{};
    bool epd_frame_blank{};
    std::uint32_t due{}, busy_begin{};
    bool busy_wait{}, due_wait{};
    unsigned reset_stage{}, reset_low{10}, reset_high{120};
    std::uint8_t orientation{}, level{255};
    std::uint8_t* mono{};
    std::uint8_t* snapshot{};
    unsigned frame_delay{};
    std::uint16_t* pixels{};
    std::uint8_t* scratch{};
#if defined(ESP_PLATFORM)
    spi_host::Lease spi_lease{};
    spi_device_handle_t spi{};
    esp_lcd_panel_io_handle_t io{};
    esp_lcd_panel_handle_t panel{};
#if defined(ESP_PLATFORM) && SOC_LCD_I80_SUPPORTED
    esp_lcd_i80_bus_handle_t i80{};
#endif
#if defined(ESP_PLATFORM) && SOC_MIPI_DSI_SUPPORTED
    esp_lcd_dsi_bus_handle_t dsi{};
    esp_ldo_channel_handle_t ldo{};
#endif
#else
    bool spi{};
#endif
    PwmHandle backlight{};
};

// The application retains native ownership and DMA buffers until restart.
Owner* owner{};
GeometryCallback geometry_callback{};

#if defined(ESP_PLATFORM)
Result native(esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return Result::success;
    case ESP_ERR_NO_MEM:
        return Result::no_memory;
    case ESP_ERR_NOT_SUPPORTED:
        return Result::unsupported;
    case ESP_ERR_TIMEOUT:
        return Result::timeout;
    default:
        return Result::failed;
    }
}

/** @brief Joins the shared bus, so an SD card or Ethernet chip may use the same wires. */
Result lease(spi_host_device_t host, const spi_bus_config_t& bus, spi_host::Lease& held) {
    static_assert(kScratchSize <= spi_host::kSharedTransferBytes);
    switch (spi_host::acquire(host, bus, spi_host::Use::shared, held)) {
    case spi_host::Result::success:
        return Result::success;
    case spi_host::Result::no_memory:
        return Result::no_memory;
    case spi_host::Result::unsupported:
        return Result::unsupported;
    case spi_host::Result::busy:
    case spi_host::Result::conflict:
        return Result::busy;
    case spi_host::Result::failed:
        break;
    }
    return Result::failed;
}

#endif

void delay_ms(unsigned value) {
    if (value)
        sleep_for_milliseconds(value);
}

bool output(int pin, bool high) {
    return pin < 0 || gpio_configure_output(PinId{static_cast<std::uint16_t>(pin)}, high) ==
                          GpioResult::success;
}

bool set(int pin, bool high) {
    return pin < 0 ||
           gpio_write(PinId{static_cast<std::uint16_t>(pin)}, high) == GpioResult::success;
}

Result reset(int pin, unsigned low = 10, unsigned high = 120) {
    if (pin < 0)
        return Result::success;
    if (!output(pin, true))
        return Result::failed;
    if (!set(pin, false))
        return Result::failed;
    delay_ms(low);
    if (!set(pin, true))
        return Result::failed;
    delay_ms(high);
    return Result::success;
}

/** @brief Transfers 8/9-bit software SPI with explicit per-word D/C. */
bool soft_word(int clock, int mosi, std::uint16_t word, int bits, bool slow) {
    for (int bit = bits - 1; bit >= 0; --bit) {
        if (!set(clock, false) || !set(mosi, (word >> bit) & 1U))
            return false;
        if (slow)
            delay_microseconds(1);
        if (!set(clock, true))
            return false;
        if (slow)
            delay_microseconds(1);
    }
    return true;
}

/** @brief Sends one synchronous SPI byte span while preserving the selected CS. */
Result spi_bytes(Owner& o, bool data, const std::uint8_t* bytes, std::size_t length) {
    auto& c = o.config;
    if (!set(c.dc, data))
        return Result::failed;
    if (!o.spi) {
        for (std::size_t i = 0; i < length; ++i)
            if (!soft_word(c.clock, c.mosi,
                    static_cast<std::uint16_t>(bytes[i]) | (c.dc < 0 && data ? 0x100U : 0U),
                    c.dc < 0 ? 9 : 8, c.spi_bus == 4))
                return Result::failed;
        return Result::success;
    }
#if defined(ESP_PLATFORM)
    if (c.dc < 0) {
        for (std::size_t i = 0; i < length; ++i) {
            spi_transaction_t transaction{};
            transaction.flags = SPI_TRANS_USE_TXDATA;
            transaction.length = 9;
            transaction.tx_data[0] = (data ? 0x80 : 0) | (bytes[i] >> 1);
            transaction.tx_data[1] = bytes[i] << 7;
            const auto result = native(spi_device_polling_transmit(o.spi, &transaction));
            if (result != Result::success)
                return result;
        }
        return Result::success;
    }
    while (length) {
        const auto count = std::min(length, kScratchSize);
        spi_transaction_t transaction{};
        transaction.length = count * 8;
        transaction.tx_buffer = bytes;
        const auto result = native(spi_device_polling_transmit(o.spi, &transaction));
        if (result != Result::success)
            return result;
        length -= count;
        bytes += count;
    }
    return Result::success;
#else
    return detail::native_spi_write(data, bytes, length);
#endif
}

#if !defined(ESP_PLATFORM)
/** @brief Writes I80 with at least 1 us setup, WR LOW and hold on claimed GPIOs. */
Result parallel_bytes(
    Owner& o, bool data, const std::uint8_t* bytes, std::size_t length, bool pixels = false) {
    const auto& c = o.config;
    if (!set(c.dc, data))
        return Result::failed;
    for (std::size_t i = 0; i < length; ++i) {
        unsigned word = bytes[i];
        if (pixels && c.data_width == 16) {
            if (i + 1 == length)
                return Result::invalid_descriptor;
            word |= static_cast<unsigned>(bytes[++i]) << 8U;
        }
        for (int bit = 0; bit < c.data_width; ++bit)
            if (!set(c.data_pins[bit], (word >> bit) & 1U))
                return Result::failed;
        delay_microseconds(1);
        if (!set(c.wr, false))
            return Result::failed;
        delay_microseconds(1);
        if (!set(c.wr, true))
            return Result::failed;
        delay_microseconds(1);
    }
    return Result::success;
}

#endif

Result i2c_bytes(int bus, int address, const std::uint8_t* bytes, std::size_t length) {
    const auto result = i2c::transfer(bus, address, bytes, length, nullptr, 0);
    return result == i2c::Result::success   ? Result::success
           : result == i2c::Result::timeout ? Result::timeout
                                            : Result::failed;
}

/** @brief Sends commands synchronously on the active control transport. */
Result command(Owner& o, int opcode, const std::uint8_t* data = nullptr, std::size_t length = 0) {
    auto& c = o.config;
#if USE_DISPLAY_QSPI
    if (c.bus == Bus::qspi)
        return opcode < 0 ? Result::invalid_descriptor
                          : detail::native_qspi_write(false, opcode, data, length);
#endif
    if (c.bus == Bus::i2c) {
        const std::uint8_t prefix[]{0, static_cast<std::uint8_t>(opcode)};
        auto result = i2c_bytes(c.i2c_bus, c.address, prefix, sizeof(prefix));
        for (std::size_t i = 0; result == Result::success && i < length; ++i) {
            const std::uint8_t parameter[]{0, data[i]};
            result = i2c_bytes(c.i2c_bus, c.address, parameter, sizeof(parameter));
        }
        return result;
    }
#if defined(ESP_PLATFORM)
    if (o.io)
        return native(esp_lcd_panel_io_tx_param(o.io, opcode, data, length));
#else
    if (c.bus == Bus::i80) {
        if (!set(c.cs, false))
            return Result::failed;
        auto result = Result::success;
        const auto byte = static_cast<std::uint8_t>(opcode);
        if (opcode >= 0)
            result = parallel_bytes(o, false, &byte, 1);
        if (result == Result::success && length)
            result = parallel_bytes(o, !c.all_commands, data, length);
        if (!set(c.cs, true))
            return Result::failed;
        return result;
    }
#endif
    if (c.bus != Bus::spi && c.bus != Bus::epd)
        return Result::unsupported;
#if defined(ESP_PLATFORM)
    // CS is a GPIO across several transactions; keep other devices on a shared bus out.
    if (o.spi) {
        const auto acquired = native(spi_device_acquire_bus(o.spi, portMAX_DELAY));
        if (acquired != Result::success)
            return acquired;
    }
#endif
    auto result = set(c.cs, false) ? Result::success : Result::failed;
    if (result == Result::success && opcode >= 0) {
        const auto byte = static_cast<std::uint8_t>(opcode);
        result = spi_bytes(o, false, &byte, 1);
    }
    if (result == Result::success && length)
        result = spi_bytes(o, opcode < 0 || !c.all_commands, data, length);
    if (!set(c.cs, true))
        result = Result::failed;
#if defined(ESP_PLATFORM)
    if (o.spi)
        spi_device_release_bus(o.spi);
#endif
    return result;
}

Result epd_area(Owner& o) {
    const auto& c = o.config;
    const std::uint8_t x[]{0, static_cast<std::uint8_t>((c.info.width - 1) / 8)};
    const auto last = c.info.height - 1;
    const std::uint8_t y[]{static_cast<std::uint8_t>(c.epd_mode == 3 ? last : 0),
        static_cast<std::uint8_t>(c.epd_mode == 3 ? last >> 8 : 0),
        static_cast<std::uint8_t>(c.epd_mode == 3 ? 0 : last),
        static_cast<std::uint8_t>(c.epd_mode == 3 ? 0 : last >> 8)};
    auto result = command(o, 0x44, x, sizeof(x));
    return result == Result::success ? command(o, 0x45, y, sizeof(y)) : result;
}

Result epd_pointer(Owner& o) {
    const auto y = o.config.epd_mode == 3 ? o.config.info.height - 1 : 0;
    const std::uint8_t x = 0, ys[]{static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8)};
    auto result = command(o, 0x4e, &x, 1);
    return result == Result::success ? command(o, 0x4f, ys, sizeof(ys)) : result;
}

unsigned encoded_delay(std::uint8_t argument) {
    switch (argument & 0xe0U) {
    case 0x80:
        return 150;
    case 0xa0:
        return 10;
    case 0xe0:
        return 500;
    default:
        return 0;
    }
}

/** @brief Executes previously validated command records and EPD pseudo opcodes. */
Result sequence(Owner& o, const detail::Bytes& bytes, bool special = false) {
    const auto& c = o.config;
    const bool dsi = c.bus == Bus::dsi;
    for (std::size_t at = 0; at < bytes.size();) {
        const auto opcode = bytes[at++], argument = bytes[at++];
        const std::size_t size = dsi ? argument : special ? argument & 0x7fU : argument & 0x1fU;
        const auto* params = bytes.data() + at;
        Result result = Result::success;
        result = command(o, opcode, params, size);
        if (result != Result::success)
            return result;
        at += size;
        delay_ms(dsi ? bytes[at++] : encoded_delay(argument));
    }
    return Result::success;
}

#if defined(ESP_PLATFORM) && SOC_LCD_RGB_SUPPORTED
/** @brief Initializes descriptor selected software serial commands before RGB scanout. */
Result rgb_initialize(Owner& o) {
    const auto& c = o.config;
    if (!c.init_kind)
        return Result::success;
    if (c.init_kind == 1) {
        if (!output(c.init_clock, true) || !output(c.init_mosi, false) || !output(c.init_cs, true))
            return Result::failed;
        auto result = reset(c.init_reset);
        if (result != Result::success)
            return result;
        for (const auto& bytes : c.init_lines) {
            for (std::size_t at = 0; at < bytes.size();) {
                const auto opcode = bytes[at++], argument = bytes[at++];
                if (!set(c.init_cs, false))
                    return Result::failed;
                bool transferred = soft_word(c.init_clock, c.init_mosi, opcode, 9, true);
                for (unsigned i = 0; transferred && i < (argument & 0x7fU); ++i)
                    transferred =
                        soft_word(c.init_clock, c.init_mosi, 0x100U | bytes[at++], 9, true);
                if (!set(c.init_cs, true) || !transferred)
                    return Result::failed;
                delay_ms(encoded_delay(argument));
            }
        }
    } else {
        for (const auto& bytes : c.init_lines) {
            if (bytes.size() == 1)
                delay_ms(bytes[0]);
            else {
                auto result = i2c_bytes(c.init_bus, c.init_address, bytes.data(), bytes.size());
                if (result != Result::success)
                    return result;
            }
        }
    }
    return Result::success;
}

#endif

/** @brief Creates native buses with resource ownership retained on failure. */
Result initialize(Owner& o) {
    auto& c = o.config;
#if defined(ESP_PLATFORM)
    o.scratch = static_cast<std::uint8_t*>(
        heap_caps_malloc(kScratchSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
#else
    o.scratch = static_cast<std::uint8_t*>(std::malloc(kScratchSize));
#endif
    if (!o.scratch)
        return Result::no_memory;
    if (c.info.bpp == 1) {
        const auto size = static_cast<std::size_t>(c.info.width) * ((c.info.height + 7U) / 8U);
        o.mono = static_cast<std::uint8_t*>(std::calloc(size, 1));
        if (!o.mono)
            return Result::no_memory;
        if (c.bus == Bus::epd) {
            std::memset(o.mono, 0xff, size);
            o.snapshot = static_cast<std::uint8_t*>(std::malloc(size));
            if (!o.snapshot)
                return Result::no_memory;
            std::memset(o.snapshot, 0xff, size);
        }
    }
    if (!output(c.backlight, (c.backlight_flags & 1) != 0))
        return Result::failed;
    auto result = Result::success;
    if (c.bus != Bus::epd)
        result = reset(c.reset);
    else if (!output(c.reset, true))
        result = Result::failed;
    if (result != Result::success)
        return result;
#if USE_DISPLAY_QSPI
    if (c.bus == Bus::qspi) {
        result = detail::native_qspi_open(c);
        if (result == Result::success)
            result = sequence(o, c.init);
    } else
#endif
        if (c.bus == Bus::spi || c.bus == Bus::epd) {
        if (!output(c.cs, true) || !output(c.dc, true))
            return Result::failed;
        if (c.busy >= 0 && gpio_configure_input(PinId{static_cast<std::uint16_t>(c.busy)},
                               GpioPull::none) != GpioResult::success)
            return Result::failed;
        if (c.spi_bus <= 2) {
#if defined(ESP_PLATFORM)
            spi_bus_config_t bus{};
            bus.sclk_io_num = c.clock;
            bus.mosi_io_num = c.mosi;
            bus.miso_io_num = c.miso;
            bus.quadwp_io_num = -1;
            bus.quadhd_io_num = -1;
            bus.max_transfer_sz = kScratchSize;
            const auto host = c.spi_bus == 1 ? SPI2_HOST : static_cast<spi_host_device_t>(2);
            result = lease(host, bus, o.spi_lease);
            if (result != Result::success)
                return result;
            spi_device_interface_config_t device{};
            device.clock_speed_hz = c.speed * 1000000;
            device.mode = 3;
            device.spics_io_num = -1;
            device.queue_size = 1;
            result = native(spi_bus_add_device(host, &device, &o.spi));
            if (result != Result::success)
                return result;
#else
            result = detail::native_spi_open(c);
            if (result != Result::success)
                return result;
            o.spi = true;
#endif
        } else if (!output(c.clock, true) || !output(c.mosi, false))
            return Result::failed;
        if (c.bus != Bus::epd)
            result = sequence(o, c.init);
        else
            o.dirty = true;
    } else if (c.bus == Bus::i2c) {
        if (c.scl >= 0 && i2c::configure(c.i2c_bus, PinId{static_cast<std::uint16_t>(c.scl)},
                              PinId{static_cast<std::uint16_t>(c.sda)}) != i2c::Result::success)
            return Result::failed;
        for (auto byte : c.init) {
            result = command(o, byte);
            if (result != Result::success)
                return result;
        }
    } else if (c.bus == Bus::i80) {
#if defined(ESP_PLATFORM) && SOC_LCD_I80_SUPPORTED
        if (!output(c.rd, true))
            return Result::failed;
        esp_lcd_i80_bus_config_t bus{};
        bus.dc_gpio_num = static_cast<gpio_num_t>(c.dc); // gpio_num_t from IDF 6.1
        bus.wr_gpio_num = static_cast<gpio_num_t>(c.wr);
        bus.clk_src = LCD_CLK_SRC_DEFAULT;
        bus.bus_width = c.data_width;
        bus.max_transfer_bytes = kScratchSize;
        bus.dma_burst_size = 32;
        for (int i = 0; i < 16; ++i)
            bus.data_gpio_nums[i] = static_cast<gpio_num_t>(c.data_pins[i]);
        result = native(esp_lcd_new_i80_bus(&bus, &o.i80));
        if (result != Result::success)
            return result;
        esp_lcd_panel_io_i80_config_t io{};
        io.cs_gpio_num = static_cast<gpio_num_t>(c.cs);
        io.pclk_hz = c.speed * 1000000;
        io.trans_queue_depth = 1;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        io.dc_levels.dc_idle_level = 1;
        io.dc_levels.dc_data_level = 1;
        result = native(esp_lcd_new_panel_io_i80(o.i80, &io, &o.io));
        if (result != Result::success)
            return result;
        result = sequence(o, c.init);
#elif !defined(ESP_PLATFORM)
        if (!output(c.cs, true) || !output(c.dc, true) || !output(c.wr, true) ||
            !output(c.rd, true))
            return Result::failed;
        for (int i = 0; i < c.data_width; ++i)
            if (!output(c.data_pins[i], false))
                return Result::failed;
        result = sequence(o, c.init);
#else
        return Result::unsupported;
#endif
    } else if (c.bus == Bus::rgb) {
#if defined(ESP_PLATFORM) && SOC_LCD_RGB_SUPPORTED
        result = rgb_initialize(o);
        if (result != Result::success)
            return result;
        esp_lcd_rgb_panel_config_t panel{};
        panel.clk_src = LCD_CLK_SRC_DEFAULT;
        panel.data_width = 16;
#if ESP_IDF_VERSION_MAJOR >= 6
        panel.in_color_format = LCD_COLOR_FMT_RGB565;
        panel.out_color_format = LCD_COLOR_FMT_RGB565;
#else
        panel.bits_per_pixel = 16;
#endif
        panel.num_fbs = 1;
        panel.dma_burst_size = 64;
        panel.flags.fb_in_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
        panel.de_gpio_num = static_cast<gpio_num_t>(c.de);
        panel.vsync_gpio_num = static_cast<gpio_num_t>(c.vsync);
        panel.hsync_gpio_num = static_cast<gpio_num_t>(c.hsync);
        panel.pclk_gpio_num = static_cast<gpio_num_t>(c.pclk);
        panel.disp_gpio_num = static_cast<gpio_num_t>(-1);
        for (int i = 0; i < 16; ++i)
            panel.data_gpio_nums[i] =
                static_cast<gpio_num_t>(c.data_pins[(c.info.flags & 2) ? (i ^ 8) : i]);
        auto& t = panel.timings;
        t.pclk_hz = c.speed * 1000000;
        t.h_res = c.info.width;
        t.v_res = c.info.height;
        t.flags.hsync_idle_low = !c.timing[0];
        t.hsync_front_porch = c.timing[1];
        t.hsync_pulse_width = c.timing[2];
        t.hsync_back_porch = c.timing[3];
        t.flags.vsync_idle_low = !c.timing[4];
        t.vsync_front_porch = c.timing[5];
        t.vsync_pulse_width = c.timing[6];
        t.vsync_back_porch = c.timing[7];
        t.flags.pclk_active_neg = c.timing[8];
        result = native(esp_lcd_new_rgb_panel(&panel, &o.panel));
        if (result == Result::success)
            result = native(esp_lcd_panel_reset(o.panel));
        if (result == Result::success)
            result = native(esp_lcd_panel_init(o.panel));
        void* framebuffer{};
        if (result == Result::success)
            result = native(esp_lcd_rgb_panel_get_frame_buffer(o.panel, 1, &framebuffer));
        o.pixels = static_cast<std::uint16_t*>(framebuffer);
#else
        return Result::unsupported;
#endif
    } else if (c.bus == Bus::dsi) {
#if defined(ESP_PLATFORM) && SOC_MIPI_DSI_SUPPORTED
        if (c.ldo >= 0) {
            esp_ldo_channel_config_t ldo{};
            ldo.chan_id = c.ldo;
            ldo.voltage_mv = c.voltage;
            result = native(esp_ldo_acquire_channel(&ldo, &o.ldo));
            if (result != Result::success)
                return result;
            delay_ms(10);
        }
        esp_lcd_dsi_bus_config_t bus{};
        bus.bus_id = 0;
        bus.num_data_lanes = c.lanes;
        bus.lane_bit_rate_mbps = c.lane_speed;
        result = native(esp_lcd_new_dsi_bus(&bus, &o.dsi));
        if (result != Result::success)
            return result;
        esp_lcd_dbi_io_config_t io{};
        io.virtual_channel = 0;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        result = native(esp_lcd_new_panel_io_dbi(o.dsi, &io, &o.io));
        if (result != Result::success)
            return result;
        esp_lcd_dpi_panel_config_t panel{};
        panel.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        panel.dpi_clock_freq_mhz = c.pixel_clock / 1000000.0f;
#if ESP_IDF_VERSION_MAJOR >= 6
        panel.in_color_format = LCD_COLOR_FMT_RGB565;
        panel.out_color_format = LCD_COLOR_FMT_RGB565;
#else
        panel.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
#endif
        panel.num_fbs = 1;
        auto& t = panel.video_timing;
        t.h_size = c.info.width;
        t.v_size = c.info.height;
        t.hsync_front_porch = c.timing[0];
        t.vsync_front_porch = c.timing[1];
        t.hsync_back_porch = c.timing[2];
        t.hsync_pulse_width = c.timing[3];
        t.vsync_pulse_width = c.timing[4];
        t.vsync_back_porch = c.timing[5];
        result = native(esp_lcd_new_panel_dpi(o.dsi, &panel, &o.panel));
        if (result == Result::success)
            result = native(esp_lcd_panel_init(o.panel));
        void* framebuffer{};
        if (result == Result::success)
            result = native(esp_lcd_dpi_panel_get_frame_buffer(o.panel, 1, &framebuffer));
        o.pixels = static_cast<std::uint16_t*>(framebuffer);
        if (result == Result::success)
            result = sequence(o, c.init);
#else
        return Result::unsupported;
#endif
    }
    if (result != Result::success)
        return result;
    if (c.info.bpp == 16 && c.info.buffered) {
        if (!o.pixels)
            return Result::failed;
        std::memset(o.pixels, 0, static_cast<std::size_t>(c.info.width) * c.info.height * 2U);
        o.dirty = true;
    }
    if (c.backlight >= 0 && !(c.backlight_flags & 2)) {
        PwmRequest request{};
        request.logical_output = {PwmOutputOwner::display, 1};
        request.pin = PinId{static_cast<std::uint16_t>(c.backlight)};
        request.frequency_hz = 1000;
        request.max_error_ppm = kMaximumPwmErrorPpm;
        request.min_resolution_bits = 8;
        request.inverted = (c.backlight_flags & 1) != 0;
        request.initial_duty = 0;
        PwmPlanToken token{};
        PwmRequestResult allocation{};
        if (pwm_prepare(&request, 1, token, &allocation) != PwmResult::success ||
            pwm_activate(token, &allocation, 1) != PwmResult::success)
            return Result::failed;
        o.backlight = allocation.handle;
    }
    return Result::success;
}

/** @brief Converts logical pixel coordinates to the physical retained framebuffer. */
void physical(const Owner& o, unsigned& x, unsigned& y) {
    const auto width = o.config.info.width, height = o.config.info.height;
    switch (o.orientation) {
    case 1:
        std::swap(x, y);
        x = width - 1U - x;
        break;
    case 2:
        x = width - 1U - x;
        y = height - 1U - y;
        break;
    case 3:
        std::swap(x, y);
        y = height - 1U - y;
        break;
    }
}

Result window(Owner& o, unsigned x, unsigned y, unsigned width, unsigned height) {
    const auto& c = o.config;
    x += c.x_offset[o.orientation];
    y += c.y_offset[o.orientation];
    unsigned last_x = x + width - 1, last_y = y + height - 1;
    std::uint8_t xs[]{static_cast<std::uint8_t>(x >> 8), static_cast<std::uint8_t>(x),
        static_cast<std::uint8_t>(last_x >> 8), static_cast<std::uint8_t>(last_x)};
    std::uint8_t ys[]{static_cast<std::uint8_t>(y >> 8), static_cast<std::uint8_t>(y),
        static_cast<std::uint8_t>(last_y >> 8), static_cast<std::uint8_t>(last_y)};
    if (c.address_mode == 8) {
        if (o.orientation & 1U) {
            std::swap(x, y);
            std::swap(last_x, last_y);
        }
        const std::uint8_t x8[]{static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(last_x)},
            y8[]{static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(last_y)};
        auto result = command(o, c.addr_x, x8, 2);
        return result == Result::success ? command(o, c.addr_y, y8, 2) : result;
    }
    auto result = command(o, c.addr_x, xs, sizeof(xs));
    return result == Result::success ? command(o, c.addr_y, ys, sizeof(ys)) : result;
}

/** @brief Streams source tiles using retained DMA storage and a synchronous queue drain. */
Result direct_pixels(Owner& o, const std::uint16_t* data, std::size_t count) {
    const auto& c = o.config;
    const std::size_t bytes_per_pixel =
        c.color_mode == 18 ? (c.bus == Bus::i80 && c.data_width == 16 ? 6U : 3U) : 2U;
    auto result = Result::success;
    if (c.bus != Bus::qspi)
        result = command(o, c.ram == 255 ? -1 : c.ram);
#if USE_DISPLAY_QSPI
    bool first = true;
#endif
    while (count && result == Result::success) {
        const auto pixels = std::min(count, kScratchSize / bytes_per_pixel);
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto color = data[i];
            if (c.color_mode == 18) {
                const auto step = bytes_per_pixel / 3U;
                o.scratch[i * bytes_per_pixel] = ((color >> 11) & 31U) * 255U / 31U;
                o.scratch[i * bytes_per_pixel + step] = ((color >> 5) & 63U) * 255U / 63U;
                o.scratch[i * bytes_per_pixel + step * 2U] = (color & 31U) * 255U / 31U;
                if (step == 2)
                    for (unsigned channel = 0; channel < 3; ++channel)
                        o.scratch[i * bytes_per_pixel + channel * 2U + 1U] = 0;
            } else if (c.bus == Bus::i80 && c.data_width == 16) {
                o.scratch[i * 2] = color;
                o.scratch[i * 2 + 1] = color >> 8;
            } else {
                o.scratch[i * 2] = color >> 8;
                o.scratch[i * 2 + 1] = color;
            }
        }
        const auto bytes = pixels * bytes_per_pixel;
#if USE_DISPLAY_QSPI
        if (c.bus == Bus::qspi)
            result = detail::native_qspi_write(true, first ? c.ram : 0x3c, o.scratch, bytes);
        else
#endif
            if (c.bus == Bus::i80) {
#if defined(ESP_PLATFORM)
            result = native(esp_lcd_panel_io_tx_color(o.io, -1, o.scratch, bytes));
            // tx_param waits for all pending DMA before returning, even without a command.
            if (result == Result::success)
                result = native(esp_lcd_panel_io_tx_param(o.io, -1, nullptr, 0));
#else
            if (!set(c.cs, false))
                return Result::failed;
            result = parallel_bytes(o, true, o.scratch, bytes, true);
            if (!set(c.cs, true))
                return Result::failed;
#endif
        } else
            result = command(o, -1, o.scratch, bytes);
#if USE_DISPLAY_QSPI
        first = false;
#endif
        count -= pixels;
        data += pixels;
    }
    return result;
}

Result refresh_mono(Owner& o) {
    const auto& c = o.config;
    if (c.pack_width) {
        auto result = command(o, c.addr_x, c.ram_bounds.data(), 2);
        if (result == Result::success)
            result = command(o, c.addr_y, c.ram_bounds.data() + 2, 2);
        if (result == Result::success)
            result = command(o, c.ram);
        std::size_t count{};
        for (unsigned x = 0; result == Result::success && x < c.info.width; x += c.pack_width) {
            for (unsigned y = 0; y < c.info.height; y += c.pack_height) {
                std::uint8_t packed = 0, mask = 0x80;
                for (unsigned row = 0; row < c.pack_height; ++row) {
                    const auto sy = (c.pack_flags & 2) ? c.info.height - 1U - y - row : y + row;
                    for (unsigned col = 0; col < c.pack_width; ++col) {
                        bool pixel = (o.mono[x + col + (sy / 8U) * c.info.width] >> (sy & 7U)) & 1U;
                        pixel ^= c.inverted_mono;
                        if (pixel)
                            packed |= mask;
                        mask >>= 1;
                    }
                }
                o.scratch[count++] =
                    (c.pack_flags & 1) ? static_cast<std::uint8_t>(~packed) : packed;
                if (count == kScratchSize) {
                    result = command(o, -1, o.scratch, count);
                    count = 0;
                }
            }
        }
        return result == Result::success && count ? command(o, -1, o.scratch, count) : result;
    }
    auto result = command(o, c.addr_x);
    if (result == Result::success)
        result = command(o, c.page_start);
    if (result == Result::success)
        result = command(o, c.page_end);
    for (unsigned page = 0; result == Result::success && page < (c.info.height + 7U) / 8U; ++page) {
        result = command(o, 0xb0 + page + c.addr_y);
        if (result == Result::success)
            result = command(o, c.col_start & 15U);
        if (result == Result::success)
            result = command(o, 0x10 | (c.col_start >> 4));
        for (unsigned x = 0; result == Result::success && x < c.info.width;) {
            const auto size =
                std::min<unsigned>(c.info.width - x, c.bus == Bus::i2c ? 127 : kScratchSize);
            const auto prefix = c.bus == Bus::i2c ? 1U : 0U;
            o.scratch[0] = 0x40;
            for (unsigned i = 0; i < size; ++i)
                o.scratch[i + prefix] =
                    o.mono[x + i + page * c.info.width] ^ (c.inverted_mono ? 255 : 0);
            result = c.bus == Bus::i2c ? i2c_bytes(c.i2c_bus, c.address, o.scratch, size + 1)
                                       : command(o, -1, o.scratch, size);
            x += size;
        }
    }
    return result;
}

/** @brief Schedules an EPD pause without sleeping the cooperative application task. */
void pause_epd(Owner& o, unsigned milliseconds) {
    o.due = monotonic_milliseconds() + milliseconds;
    o.due_wait = milliseconds != 0;
}

void begin_busy(Owner& o, unsigned fallback) {
    if (o.config.busy < 0)
        pause_epd(o, fallback);
    else {
        o.busy_wait = true;
        o.busy_begin = monotonic_milliseconds();
    }
}

void begin_frame(Owner& o, int opcode, bool blank = false, unsigned delay = 0) {
    o.epd_frame_opcode = opcode;
    o.epd_frame_stage = 0;
    o.epd_frame_offset = 0;
    o.epd_frame_blank = blank;
    o.frame_delay = delay;
}

/** @brief Sends one bounded span from the immutable EPD refresh snapshot. */
Result poll_frame(Owner& o) {
    const auto& c = o.config;
    if (o.epd_frame_stage == 0) {
        auto result = command(o, o.epd_frame_opcode);
        if (result != Result::success)
            return result;
        o.epd_frame_stage = 1;
    }
    const std::size_t row_bytes = (c.info.width + 7U) / 8U;
    const auto total = row_bytes * c.info.height;
    const auto count = std::min<std::size_t>(128, total - o.epd_frame_offset);
    for (std::size_t i = 0; i < count; ++i) {
        const auto position = o.epd_frame_offset + i;
        const auto row = position / row_bytes, xb = position % row_bytes;
        const auto y = c.epd_mode == 3 ? c.info.height - 1U - row : row;
        std::uint8_t packed = o.epd_frame_blank ? 255 : 0;
        if (!o.epd_frame_blank) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                const auto x = xb * 8U + bit;
                const bool white = x >= c.info.width ||
                                   ((o.snapshot[x + (y / 8U) * c.info.width] >> (y & 7U)) & 1U);
                if (white)
                    packed |= 0x80U >> bit;
            }
            if (o.frame_inverted ^ c.inverted_mono ^ ((c.info.flags & 16) != 0))
                packed = ~packed;
        }
        o.scratch[i] = packed;
    }
    auto result = command(o, -1, o.scratch, count);
    if (result != Result::success)
        return result;
    o.epd_frame_offset += count;
    if (o.epd_frame_offset == total) {
        o.epd_frame_opcode = -1;
        pause_epd(o, o.frame_delay);
    }
    return Result::success;
}

/** @brief Executes one EPD record; every delay, reset and BUSY wait is deferred. */
Result poll_epd(Owner& o) {
    using Phase = Owner::EpdPhase;
    const auto& c = o.config;
    const auto now = monotonic_milliseconds();
    if (o.due_wait) {
        if (static_cast<std::int32_t>(now - o.due) < 0)
            return Result::success;
        o.due_wait = false;
    }
    if (o.busy_wait) {
        const bool busy_level = !(c.info.flags & 8);
        bool high{};
        if (gpio_read(PinId{static_cast<std::uint16_t>(c.busy)}, high) != GpioResult::success)
            return Result::failed;
        if (high == busy_level) {
            return now - o.busy_begin >= 3000U ? Result::timeout : Result::success;
        }
        o.busy_wait = false;
    }
    if (o.reset_stage) {
        if (o.reset_stage == 1) {
            if (!set(c.reset, true))
                return Result::failed;
            o.reset_stage = 2;
            pause_epd(o, o.reset_high);
        } else
            o.reset_stage = 0;
        return Result::success;
    }
    if (o.epd_frame_opcode >= 0)
        return poll_frame(o);
    if (o.epd_phase == Phase::reset) {
        if (c.reset >= 0) {
            if (!set(c.reset, false))
                return Result::failed;
            o.reset_stage = 1;
            pause_epd(o, o.reset_low);
        }
        o.epd_phase = Phase::init;
        o.sequence_bytes = &c.init;
        o.sequence_at = 0;
        return Result::success;
    }
    if (o.sequence_bytes && o.sequence_at < o.sequence_bytes->size()) {
        const auto& bytes = *o.sequence_bytes;
        const auto opcode = bytes[o.sequence_at++], argument = bytes[o.sequence_at++];
        const auto count = argument & 0x1fU;
        const auto* data = bytes.data() + o.sequence_at;
        o.sequence_at += count;
        const auto delay = encoded_delay(argument);
        Result result = Result::success;
        bool frame{};
        if (c.epd_mode != 2 && opcode >= 0x60 && opcode <= 0x6a) {
            switch (opcode) {
            case 0x60:
                if (c.reset >= 0) {
                    if (!set(c.reset, false))
                        return Result::failed;
                    o.reset_stage = 1;
                    o.reset_low = count ? data[0] : 10;
                    o.reset_high = count ? data[0] : 10;
                    pause_epd(o, o.reset_low);
                }
                break;
            case 0x61:
            case 0x62: {
                const auto& lut = c.luts[opcode == 0x62];
                if (!lut.bytes.empty())
                    result = command(o, lut.command, lut.bytes.data(), lut.bytes.size());
                break;
            }
            case 0x63:
                begin_busy(o, count ? data[0] * 10U : c.update_time);
                break;
            case 0x64:
                result = epd_area(o);
                break;
            case 0x65:
                result = epd_pointer(o);
                break;
            case 0x66:
                begin_frame(o, c.ram == 255 ? 0x24 : c.ram, false, delay);
                frame = true;
                break;
            case 0x67:
            case 0x68:
                result = epd_area(o);
                if (result == Result::success)
                    result = epd_pointer(o);
                if (result == Result::success) {
                    begin_frame(o, 0x24, opcode == 0x67, delay);
                    frame = true;
                }
                break;
            case 0x69:
            case 0x6a:
#if defined(ESP_PLATFORM)
                if (count && ((data[0] == esp_reset_reason()) == (opcode == 0x69)))
                    o.sequence_at = bytes.size();
#else
                return Result::unsupported; // Rejected in preflight, before GPIO ownership.
#endif
                break;
            }
        } else
            result = command(o, opcode, data, count);
        if (result != Result::success)
            return result;
        if (!frame && !o.reset_stage && !o.busy_wait && !o.due_wait)
            pause_epd(o, delay);
        return Result::success;
    }
    o.sequence_bytes = nullptr;
    if (o.epd_phase == Phase::init) {
        o.epd_initialized = true;
        o.epd_phase = Phase::idle;
        const auto result = power(true);
        if (result != Result::success && result != Result::unsupported)
            return result;
    }
    if (o.epd_phase == Phase::idle) {
        if (!o.enabled || !o.refresh_pending)
            return Result::success;
        o.refresh_pending = false;
        if (!o.dirty)
            return Result::success;
        o.active_partial = o.pending_partial && o.epd_has_refreshed;
        o.frame_inverted = o.inverted;
        o.dirty = false;
        const auto size = static_cast<std::size_t>(c.info.width) * ((c.info.height + 7U) / 8U);
        std::memcpy(o.snapshot, o.mono, size);
        o.epd_phase = Phase::refresh;
        o.epd_refresh_stage = 0;
        if (c.epd_mode != 2) {
            o.sequence_bytes = o.active_partial ? &c.partial : &c.full;
            o.sequence_at = 0;
            return Result::success;
        }
    }
    if (o.epd_phase == Phase::refresh) {
        if (c.epd_mode == 2) {
            const auto step = o.epd_refresh_stage++;
            if (step == 0) {
                begin_frame(o, c.addr_x, true);
                return Result::success;
            }
            if (step == 1) {
                begin_frame(o, c.addr_y);
                return Result::success;
            }
            if (step < 7) {
                const auto& lut = c.luts[step];
                return command(o, lut.command, lut.bytes.data(), lut.bytes.size());
            }
            auto result = command(o, c.ram);
            if (result != Result::success)
                return result;
        }
        o.epd_phase = Phase::finish;
        begin_busy(o, o.active_partial ? c.partial_time : std::abs(c.full_time));
        return Result::success;
    }
    if (o.epd_phase == Phase::finish) {
        o.epd_has_refreshed = true;
        o.epd_phase = Phase::idle;
    }
    return Result::success;
}

Result backlight(Owner& o, std::uint8_t value) {
    if (o.config.backlight < 0)
        return Result::success;
    if (o.backlight.valid()) {
        PwmDutyUpdate update{
            o.backlight, static_cast<std::uint16_t>(static_cast<unsigned>(value) * 65535U / 255U)};
        PwmOperationResult result{};
        return pwm_write_batch(&update, 1, &result) == PwmResult::success ? Result::success
                                                                          : Result::failed;
    }
    return set(o.config.backlight, (value != 0) ^ ((o.config.backlight_flags & 1) != 0))
               ? Result::success
               : Result::failed;
}
} // namespace

namespace detail {
Result preflight(Config& c, char* error, std::size_t capacity) {
    auto fail = [&](const char* reason, Result result = Result::invalid_descriptor) {
        if (error && capacity)
            std::snprintf(error, capacity, "%s", reason);
        return result;
    };
#if USE_DISPLAY_QSPI
    if (c.bus == Bus::qspi) {
        const auto result = native_qspi_preflight(c);
        if (result != Result::success)
            return fail("Unsupported native QSPI bus, pins or rate", result);
    }
#endif
#if !defined(ESP_PLATFORM) || !SOC_LCD_RGB_SUPPORTED
    if (c.bus == Bus::rgb)
        return fail("RGB is unavailable on this chip", Result::unsupported);
#endif
#if defined(ESP_PLATFORM) && !SOC_LCD_I80_SUPPORTED
    if (c.bus == Bus::i80)
        return fail("I80 is unavailable on this chip", Result::unsupported);
#endif
#if !defined(ESP_PLATFORM) || !SOC_MIPI_DSI_SUPPORTED
    if (c.bus == Bus::dsi)
        return fail("DSI is unavailable on this chip", Result::unsupported);
#endif
#if defined(ESP_PLATFORM)
    if ((c.bus == Bus::spi || c.bus == Bus::epd) && c.spi_bus == 2 && SOC_SPI_PERIPH_NUM < 3)
        return fail("SPI bus 2 is unavailable on this chip", Result::unsupported);
#else
    if ((c.bus == Bus::spi || c.bus == Bus::epd) && c.spi_bus <= 2) {
        const auto result = native_spi_preflight(c);
        if (result != Result::success)
            return fail("Unsupported native SPI bus, pins, rate or framing", result);
    }
#endif
#if !defined(ESP_PLATFORM)
    if (c.bus == Bus::epd && c.epd_mode != 2) {
        for (const auto* sequence : {&c.init, &c.full, &c.partial})
            for (std::size_t at = 0; at < sequence->size();) {
                const auto opcode = (*sequence)[at++];
                const auto argument = (*sequence)[at++];
                if (opcode == 0x69 || opcode == 0x6a)
                    return fail(
                        "EPD ESP reset-reason conditions are unavailable", Result::unsupported);
                at += argument & 0x1fU;
            }
    }
#endif
    c.info.pin_count = 0;
    auto pin = [&](int pad, bool required = false, bool input = false) {
        if (pad < 0)
            return !required && pad == -1;
        if (pad > 65534)
            return false;
        const PinId physical{static_cast<std::uint16_t>(pad)};
        const auto capability = input ? PinCapability::input : PinCapability::output;
        if (!gpio_available(physical) ||
            !(gpio_capabilities(physical) & capability_mask(capability)))
            return false;
        for (std::size_t i = 0; i < c.info.pin_count; ++i)
            if (c.info.pins[i].value() == pad)
                return false;
        if (c.info.pin_count == std::size(c.info.pins))
            return false;
        c.info.pins[c.info.pin_count++] = physical;
        return true;
    };
    if (!pin(c.reset) || !pin(c.backlight) || !pin(c.busy, false, true))
        return fail("Invalid, duplicate or unavailable control GPIO");
    if (c.bus == Bus::qspi) {
        if (!pin(c.cs, true) || !pin(c.clock, true))
            return fail("Invalid QSPI control GPIO");
        for (auto pad : c.quad_pins)
            if (!pin(pad, true))
                return fail("Invalid, duplicate or unavailable QSPI data GPIO");
    } else if (c.bus == Bus::spi || c.bus == Bus::epd) {
        if (!pin(c.cs) || !pin(c.clock, true) || !pin(c.mosi, true) || !pin(c.dc) ||
            !pin(c.miso, false, true))
            return fail("Invalid, duplicate or unavailable SPI GPIO");
    } else if (c.bus == Bus::i2c) {
        if (c.scl < 0) {
            if (!i2c::available(c.i2c_bus))
                return fail("Descriptor requires an active shared I2C bus", Result::unsupported);
            c.info.shared_i2c_bus = c.i2c_bus;
        } else if (i2c::available(c.i2c_bus))
            return fail("Explicit I2C pins require an unused bus", Result::busy);
        else if (!pin(c.scl, true) || !pin(c.sda, true) ||
                 !(gpio_capabilities(PinId{static_cast<std::uint16_t>(c.scl)}) &
                     capability_mask(PinCapability::input)) ||
                 !(gpio_capabilities(PinId{static_cast<std::uint16_t>(c.sda)}) &
                     capability_mask(PinCapability::input)))
            return fail("Invalid I2C GPIO");
    } else if (c.bus == Bus::i80) {
        if (!pin(c.cs) || !pin(c.dc, true) || !pin(c.wr, true) || !pin(c.rd))
            return fail("Invalid I80 control GPIO");
        for (int i = 0; i < c.data_width; ++i)
            if (!pin(c.data_pins[i], true))
                return fail("Invalid I80 data GPIO");
    } else if (c.bus == Bus::rgb) {
        if (!pin(c.de) || !pin(c.hsync) || !pin(c.vsync) || !pin(c.pclk, true))
            return fail("Invalid RGB control GPIO");
        for (auto pad : c.data_pins)
            if (!pin(pad, true))
                return fail("Invalid RGB data GPIO");
        if (c.init_kind == 1 && (!pin(c.init_clock, true) || !pin(c.init_mosi, true) ||
                                    !pin(c.init_cs) || !pin(c.init_reset)))
            return fail("Invalid RGB initialization GPIO");
        if (c.init_kind == 2) {
            if (!i2c::available(c.init_bus))
                return fail("RGB initialization requires shared I2C", Result::unsupported);
            c.info.shared_i2c_bus = c.init_bus;
        }
    } else if (c.bus == Bus::dsi && !pin(c.te, false, true))
        return fail("Invalid DSI TE GPIO");
    if (c.bus == Bus::epd && c.info.width > 2048)
        return fail("EPD RAM X address exceeds byte range");
    if ((c.bus == Bus::spi || c.bus == Bus::i80 || c.bus == Bus::qspi) && c.info.bpp == 16) {
        if (c.addr_x == 255 || c.addr_y == 255)
            return fail("Missing address commands");
        if (c.address_mode == 8)
            for (unsigned i = 0; i < 4; ++i) {
                const unsigned width = (i & 1U) ? c.info.height : c.info.width,
                               height = (i & 1U) ? c.info.width : c.info.height;
                if (c.x_offset[i] + width > 256 || c.y_offset[i] + height > 256)
                    return fail("Eight-bit address window overflow");
            }
    }
#if USE_UNIVERSAL_TOUCH
    if (c.touch.configured) {
        if (!i2c::available(c.touch.bus))
            return fail("Universal touch requires an active shared I2C bus", Result::unsupported);
        if (c.bus == Bus::i2c && c.i2c_bus == c.touch.bus && c.address == c.touch.address)
            return fail("Touch and display cannot use the same I2C slave address");
        if (!pin(c.touch.reset) || !pin(c.touch.irq, false, true))
            return fail("Invalid, duplicate or unavailable touch control GPIO");
        c.info.touch_i2c_bus = c.touch.bus;
        c.info.touch_i2c_address = c.touch.address;
    }
#endif
    if (error && capacity)
        error[0] = 0;
    return Result::success;
}
} // namespace detail

Result describe(
    const char* text, std::size_t length, Info& info, char* error, std::size_t capacity) {
    Config config;
    auto result = detail::parse(text, length, config, error, capacity);
    if (result == Result::success)
        result = detail::preflight(config, error, capacity);
    if (result == Result::success)
        info = config.info;
    return result;
}

Result open(const char* text, std::size_t length, Info& info, char* error, std::size_t capacity) {
    if (owner)
        return Result::busy;
    auto* candidate = new (std::nothrow) Owner;
    if (!candidate)
        return Result::no_memory;
    auto result = detail::parse(text, length, candidate->config, error, capacity);
    if (result == Result::success)
        result = detail::preflight(candidate->config, error, capacity);
    if (result != Result::success) {
        delete candidate;
        return result;
    }
    owner = candidate;
    owner->current = owner->config.info;
    result = initialize(*owner);
    if (result != Result::success) {
        owner->faulted = true;
        if (error && capacity)
            std::snprintf(error, capacity, "Display hardware initialization failed (%u)",
                static_cast<unsigned>(result));
        return result;
    }
    owner->ready = true;
    result = rotation(0);
    if (result == Result::success && owner->config.bus != Bus::epd) {
        result = power(true);
        if (result == Result::unsupported)
            result = Result::success;
    }
    if (result != Result::success) {
        owner->faulted = true;
        return result;
    }
#if USE_UNIVERSAL_TOUCH
    if (owner->config.touch.configured) {
        result = detail::touch_open(
            owner->config.touch, owner->config.info.width, owner->config.info.height);
        if (result != Result::success) {
            owner->faulted = true;
            if (error && capacity)
                std::snprintf(
                    error, capacity, "Universal touch identification or initialization failed");
            return result;
        }
    }
#endif
    info = owner->current;
    return Result::success;
}

const Info* info() {
    return owner && owner->ready ? &owner->current : nullptr;
}

bool state(State& value) {
    if (!owner || !owner->ready)
        return false;
    value = {ready(), owner->enabled, owner->inverted, owner->level, owner->orientation};
    return true;
}

void set_geometry_callback(GeometryCallback callback) {
    geometry_callback = callback;
}

Result write(std::uint16_t x, std::uint16_t y, std::uint16_t width, std::uint16_t height,
    const std::uint16_t* source, std::size_t count) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    auto& o = *owner;
    if (!source || !width || !height || static_cast<std::size_t>(width) * height != count ||
        static_cast<unsigned>(x) + width > o.current.width ||
        static_cast<unsigned>(y) + height > o.current.height)
        return Result::invalid_descriptor;
    if (o.current.buffered) {
        for (unsigned row = 0; row < height; ++row)
            for (unsigned col = 0; col < width; ++col) {
                unsigned px = x + col, py = y + row;
                physical(o, px, py);
                auto color = *source++;
                if (o.mono) {
                    // Weighted integer luma, using native component widths.
                    const bool white =
                        (((color >> 11) & 31U) * 299U * 2U + ((color >> 5) & 63U) * 587U +
                            (color & 31U) * 114U * 2U) >= 31500U;
                    auto& byte = o.mono[px + (py / 8U) * o.config.info.width];
                    const auto bit = 1U << (py & 7U);
                    byte = white ? byte | bit : byte & ~bit;
                } else {
                    if (o.config.bus == Bus::dsi) {
                        if (o.config.rgb_order)
                            color =
                                ((color & 31U) << 11) | (color & 0x7e0U) | ((color >> 11) & 31U);
                        if (o.config.endian)
                            color = (color >> 8) | (color << 8);
                    }
                    o.pixels[px + static_cast<std::size_t>(py) * o.config.info.width] = color;
                }
            }
        o.dirty = true;
        return Result::success;
    }
    auto result = window(o, x, y, width, height);
    if (result == Result::success)
        result = direct_pixels(o, source, count);
    if (result != Result::success)
        o.faulted = true;
    return result;
}

bool framebuffer(Framebuffer& frame) {
    if (!owner || !owner->ready || owner->faulted || !owner->mono || owner->orientation)
        return false;
    auto& o = *owner;
    o.framebuffer_shared = true;
    frame.data = o.mono;
    frame.size = static_cast<std::size_t>(o.config.info.width) * ((o.config.info.height + 7U) / 8U);
    return true;
}

Result poll() {
    if (!owner || !owner->ready || owner->faulted) {
#if USE_UNIVERSAL_TOUCH
        detail::touch_release();
#endif
        return Result::failed;
    }
#if USE_UNIVERSAL_TOUCH
    detail::touch_poll(owner->orientation, owner->current.width, owner->current.height);
#endif
    if (owner->config.bus != Bus::epd)
        return Result::success;
    auto result = poll_epd(*owner);
    if (result != Result::success)
        owner->faulted = true;
    return result;
}

bool ready() {
    return owner && owner->ready && !owner->faulted &&
           (owner->config.bus != Bus::epd || owner->epd_initialized);
}

Result refresh(bool partial) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    auto& o = *owner;
    const auto& c = o.config;
    if (o.framebuffer_shared)
        o.dirty = true;
    if (!o.current.buffered || !o.dirty)
        return Result::success;
    Result result = Result::success;
    if (c.bus == Bus::epd) {
        const bool requested_partial = partial && !c.partial.empty() && c.epd_mode != 2;
        o.pending_partial =
            o.refresh_pending ? o.pending_partial && requested_partial : requested_partial;
        o.refresh_pending = true;
        return Result::success;
    } else if (o.mono)
        result = refresh_mono(o);
    else {
#if defined(ESP_PLATFORM)
        result = native(
            esp_cache_msync(o.pixels, static_cast<std::size_t>(c.info.width) * c.info.height * 2U,
                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED));
#else
        result = Result::unsupported;
#endif
    }
    if (result == Result::success)
        o.dirty = false;
    else if (result != Result::unsupported)
        o.faulted = true;
    return result;
}

Result power(bool on) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    auto& o = *owner;
    const auto& c = o.config;
    if (c.bus == Bus::epd && (!o.epd_initialized || o.epd_phase != Owner::EpdPhase::idle))
        return Result::busy;
    const auto opcode = on ? c.on : c.off;
    const bool commands = c.bus != Bus::rgb;
    if (c.backlight < 0 && (!commands || (opcode == 255 && c.dim == 255)))
        return Result::unsupported;
    Result result = Result::success;
    if (commands && opcode != 255)
        result = command(o, opcode);
    if (result == Result::success && commands && c.dim != 255) {
        const std::uint8_t level = on ? o.level : 0;
        result = command(o, c.dim, &level, 1);
    }
    if (result == Result::success)
        result = backlight(o, on ? o.level : 0);
    if (result == Result::success)
        o.enabled = on;
    else
        o.faulted = true;
    return result;
}

Result invert(bool enabled) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    auto& o = *owner;
    if (o.config.bus == Bus::epd) {
        o.inverted = enabled;
        o.dirty = true;
        return Result::success;
    }
    const auto opcode = enabled ? o.config.inv_on : o.config.inv_off;
    if (opcode == 255)
        return Result::unsupported;
    const auto result = command(o, opcode);
    if (result == Result::success)
        o.inverted = enabled;
    return result;
}

Result rotation(std::uint8_t value) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    if (value > 3)
        return Result::invalid_descriptor;
    auto& o = *owner;
    const auto& c = o.config;
    if (!c.info.buffered && c.madctl != 255) {
        auto result = command(o, c.madctl, &c.rot[value], 1);
        if (result != Result::success)
            return result;
        if (c.startline != 255) {
            const std::uint8_t start = 0;
            result = command(o, c.startline, &start, 1);
            if (result != Result::success)
                return result;
        }
    } else if (!c.info.buffered && value)
        return Result::unsupported;
    o.orientation = value;
#if USE_UNIVERSAL_TOUCH
    detail::touch_release();
#endif
    o.current.width = (value & 1U) ? c.info.height : c.info.width;
    o.current.height = (value & 1U) ? c.info.width : c.info.height;
    o.dirty = true;
    if (geometry_callback)
        geometry_callback(o.current);
    return Result::success;
}

Result brightness(std::uint8_t value) {
    if (!owner || !owner->ready || owner->faulted)
        return Result::failed;
    auto& o = *owner;
    if (o.config.bus == Bus::epd && (!o.epd_initialized || o.epd_phase != Owner::EpdPhase::idle))
        return Result::busy;
    if (o.config.bus == Bus::rgb && o.config.backlight < 0)
        return Result::unsupported;
    if (o.config.bus != Bus::rgb && o.config.dim != 255 && o.enabled) {
        auto result = command(o, o.config.dim, &value, 1);
        if (result != Result::success)
            return result;
    } else if (o.config.dim == 255 && o.config.backlight < 0)
        return Result::unsupported;
    auto result = backlight(o, o.enabled ? value : 0);
    if (result == Result::success)
        o.level = value;
    return result;
}
} // namespace newmota::platform::display
