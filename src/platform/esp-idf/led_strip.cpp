/*
  led_strip.cpp - ESP-IDF native LED strip adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.cpp
 * @brief ESP-IDF native LED strip adapter.
 */

#include "newmota/platform/led_strip.h"
#include "newmota/platform/gpio.h"
#include "soc/soc_caps.h"
#include "driver/spi_master.h"
#if SOC_RMT_SUPPORTED
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_private/rmt.h" // Pinned IDF 5.5/6.1 actual resolution query.
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#include "spi_host.h"
#endif
#include <cstdlib>

namespace newmota::platform::led_strip {
namespace {
PinId pin{};
bool active{}, sending{};
std::size_t capacity{};
#if SOC_RMT_SUPPORTED
rmt_channel_handle_t channel{};
rmt_encoder_handle_t encoder{};
rmt_symbol_word_t* symbols{};
portMUX_TYPE completion_lock = portMUX_INITIALIZER_UNLOCKED;
bool transmission_done{}; ///< Access only while completion_lock is held.

/** @brief Publishes completion after the final symbol and reset have drained. */
bool IRAM_ATTR on_transmission_done(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&completion_lock);
    transmission_done = true;
    portEXIT_CRITICAL_ISR(&completion_lock);
    return false;
}
#else
spi_host::Lease lease{};
spi_device_handle_t device{};
spi_transaction_t transaction{};
constexpr auto host = SPI2_HOST;
#endif
} // namespace

Result open(PinId pad, std::size_t pixels, Port& port) {
    if (!pixels || pixels > kMaximumPixels)
        return Result::invalid_argument;
    if (!pad.valid() || !gpio_available(pad))
        return Result::unsupported;
    if (active)
        return Result::busy;
#if SOC_RMT_SUPPORTED
    symbols = static_cast<rmt_symbol_word_t*>(heap_caps_calloc(wire_size(pixels, true) * 2,
        sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!symbols)
        return Result::no_memory;
    rmt_tx_channel_config_t config{};
    config.gpio_num = static_cast<gpio_num_t>(pad.value());
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = 3200000;
    config.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    config.trans_queue_depth = 1;
    auto error = rmt_new_tx_channel(&config, &channel);
    if (error != ESP_OK) {
        heap_caps_free(symbols);
        symbols = nullptr;
        return Result::busy;
    }
    rmt_copy_encoder_config_t copy{};
    std::uint32_t rate{};
    error = rmt_get_channel_resolution(channel, &rate);
    if (error == ESP_OK && rate >= 3000000 && rate <= 3250000)
        error = rmt_new_copy_encoder(&copy, &encoder);
    else
        error = ESP_ERR_NOT_SUPPORTED;
    if (error == ESP_OK) {
        rmt_tx_event_callbacks_t callbacks{};
        callbacks.on_trans_done = on_transmission_done;
        error = rmt_tx_register_event_callbacks(channel, &callbacks, nullptr);
    }
    if (error == ESP_OK)
        error = rmt_enable(channel);
    if (error != ESP_OK) {
        bool cleaned = true;
        if (encoder && rmt_del_encoder(encoder) != ESP_OK)
            cleaned = false;
        if (rmt_del_channel(channel) != ESP_OK)
            cleaned = false;
        heap_caps_free(symbols);
        symbols = nullptr;
        encoder = nullptr;
        channel = nullptr;
        return cleaned ? Result::unsupported : Result::failed;
    }
#else
    spi_bus_config_t bus{};
    bus.mosi_io_num = pad.value();
    bus.miso_io_num = bus.sclk_io_num = bus.quadwp_io_num = bus.quadhd_io_num = -1;
    bus.max_transfer_sz = wire_size(kMaximumPixels, true);
    // The strip owns the host: its fixed 3.2 MHz frames leave no gaps for other devices.
    const auto leased = spi_host::acquire(host, bus, spi_host::Use::exclusive, lease);
    if (leased != spi_host::Result::success)
        return leased == spi_host::Result::no_memory ? Result::no_memory : Result::busy;
    esp_err_t error = ESP_OK;
    spi_device_interface_config_t config{};
    config.clock_speed_hz = 3200000;
    config.spics_io_num = -1;
    config.queue_size = 1;
    config.flags = SPI_DEVICE_HALFDUPLEX;
    error = spi_bus_add_device(host, &config, &device);
    int rate{};
    if (error == ESP_OK)
        error = spi_device_get_actual_freq(device, &rate);
    if (error != ESP_OK || rate < 3000 || rate > 3250) {
        bool cleaned = true;
        if (device && spi_bus_remove_device(device) != ESP_OK)
            cleaned = false;
        if (spi_host::release(lease) != spi_host::Result::success)
            cleaned = false;
        device = nullptr;
        return cleaned ? Result::unsupported : Result::failed;
    }
#endif
    capacity = wire_size(pixels, true);
    pin = pad;
    active = true;
    sending = false;
    port = 0;
    return Result::success;
}

Result readiness(Port port) {
    if (port != 0 || !active)
        return Result::busy;
    if (!sending)
        return Result::success;
#if SOC_RMT_SUPPORTED
    // IDF logs a timeout for an ordinary busy poll. The callback runs after
    // the completed descriptor is queued, so only reap once it is available.
    portENTER_CRITICAL(&completion_lock);
    const bool done = transmission_done;
    portEXIT_CRITICAL(&completion_lock);
    if (!done)
        return Result::busy;
    if (rmt_tx_wait_all_done(channel, 0) != ESP_OK)
        return Result::failed;
#else
    spi_transaction_t* completed{};
    if (spi_device_get_trans_result(device, &completed, 0) != ESP_OK)
        return Result::busy;
#endif
    sending = false;
    return Result::success;
}

Result transmit(Port port, const std::uint8_t* data, std::size_t size) {
    if (port != 0 || !active || !data || !size || size > capacity)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
#if SOC_RMT_SUPPORTED
    for (std::size_t i = 0; i < size * 2; ++i) {
        const auto nibble = (data[i / 2] >> ((i & 1) ? 0 : 4)) & 0xf;
        auto& symbol = symbols[i];
        symbol = {};
        symbol.level0 = nibble != 0;
        symbol.duration0 = nibble == 0xc ? 2 : 1;
        symbol.level1 = 0;
        symbol.duration1 = 4 - symbol.duration0;
    }
    rmt_transmit_config_t config{};
    config.flags.eot_level = 0;
    portENTER_CRITICAL(&completion_lock);
    transmission_done = false;
    portEXIT_CRITICAL(&completion_lock);
    const auto error =
        rmt_transmit(channel, encoder, symbols, size * 2 * sizeof(*symbols), &config);
#else
    transaction = {};
    transaction.length = size * 8;
    transaction.tx_buffer = data;
    const auto error = spi_device_queue_trans(device, &transaction, 0);
#endif
    if (error != ESP_OK)
        return Result::failed;
    sending = true;
    return Result::success;
}

Result close(Port port) {
    if (port != 0 || !active)
        return Result::invalid_argument;
    if (const auto result = readiness(port); result != Result::success)
        return result;
#if SOC_RMT_SUPPORTED
    if (rmt_disable(channel) != ESP_OK || rmt_del_channel(channel) != ESP_OK)
        return Result::failed;
    channel = nullptr;
    if (rmt_del_encoder(encoder) != ESP_OK)
        return Result::failed;
    encoder = nullptr;
    heap_caps_free(symbols);
    symbols = nullptr;
#else
    if (spi_bus_remove_device(device) != ESP_OK)
        return Result::failed;
    device = nullptr;
    if (spi_host::release(lease) != spi_host::Result::success)
        return Result::failed;
#endif
    if (gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    active = false;
    capacity = 0;
    pin = {};
    return Result::success;
}
} // namespace newmota::platform::led_strip
