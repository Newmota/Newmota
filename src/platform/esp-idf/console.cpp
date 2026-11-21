/*
  console.cpp - ESP-IDF console implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "console.h"

#include "driver/uart.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if ESP_IDF_VERSION_MAJOR >= 6
#include "soc/uart_pins.h"
#else
#include "soc/uart_channel.h"
#endif

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>

#if defined(NEWMOTA_CONSOLE_USB_CDC)
#include "esp_private/usb_console.h"
#include "hal/usb_dwc_ll.h"
#endif

#if defined(NEWMOTA_CONSOLE_USB_SERIAL_JTAG)
#include "hal/usb_serial_jtag_ll.h"
#endif

namespace newmota::platform {
namespace {

constexpr int kConsoleBaud = 115200;
constexpr int kUartReceiveBufferSize = 256;
constexpr std::int64_t kUsbProbeMicroseconds = 250'000;
constexpr std::int64_t kUsbWriteTimeoutMicroseconds = 100'000;
constexpr std::size_t kSdkLogLineCapacity = 384;

/**
 * @brief Detects fresh USB start-of-frame activity during the boot probe window.
 * @return true when the frame counter changes before the deadline.
 */
template <typename ReadFrameNumber> [[nodiscard]] bool ProbeSof(ReadFrameNumber read_frame_number) {
    // Read the peripheral counter only during startup.  ESP-IDF's
    // usb_serial_jtag_is_connected() installs a permanent tick hook instead.
    auto previous = read_frame_number();
    const auto deadline = esp_timer_get_time() + kUsbProbeMicroseconds;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(1);
        const auto current = read_frame_number();
        if (current != previous) {
            return true;
        }
        previous = current;
    }
    return false;
}

#if defined(NEWMOTA_CONSOLE_USB_CDC)
/** @brief Probes the USB-OTG controller for host SOF traffic. */
[[nodiscard]] bool UsbCdcHasSof() {
    return ProbeSof([] { return USB_DWC_LL_GET_HW(0)->dsts_reg.soffn; });
}
#endif

#if defined(NEWMOTA_CONSOLE_USB_SERIAL_JTAG)
/** @brief Probes USB Serial/JTAG for one fresh host SOF interrupt. */
[[nodiscard]] bool UsbSerialJtagHasSof() {
    // SOF is reported as a latched raw interrupt even when its interrupt is
    // disabled.  Clear any stale indication, then wait for one fresh packet.
    // This is the same hardware signal ESP-IDF's connection monitor uses,
    // without leaving its FreeRTOS tick hook installed for the lifetime of the
    // application.
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);
    const auto deadline = esp_timer_get_time() + kUsbProbeMicroseconds;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(1);
        if ((usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SOF) != 0) {
            usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);
            return true;
        }
    }
    return false;
}
#endif

/**
 * @brief Redirects an ESP-IDF formatted log call to the selected console.
 * @return The written byte count, the formatter error, or -1 on output failure.
 */
int ConsoleVprintf(const char* format, va_list arguments) {
    std::array<char, kSdkLogLineCapacity> line{};
    va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(line.data(), line.size(), format, copy);
    va_end(copy);
    if (length <= 0) {
        return length;
    }

    const auto size = std::min(static_cast<std::size_t>(length), line.size() - 1);
    return console().write({line.data(), size}) ? static_cast<int>(size) : -1;
}

} // namespace

/** @brief Returns the process-wide ESP-IDF console instance. */
Console& console() {
    static Console instance;
    return instance;
}

/**
 * @brief Selects USB or UART once at boot and redirects ESP-IDF logging.
 */
void Console::initialize() {
    if (backend_ != Backend::uninitialized) {
        return;
    }

#if defined(NEWMOTA_CONSOLE_USB_CDC)
    if (UsbCdcHasSof()) {
        backend_ = Backend::usb_cdc;
    } else if (initialize_uart()) {
        backend_ = Backend::uart0;
        usb_fallback_ = true;
    } else {
        backend_ = Backend::usb_cdc;
    }
#elif defined(NEWMOTA_CONSOLE_USB_SERIAL_JTAG)
    if (UsbSerialJtagHasSof()) {
        backend_ = Backend::usb_serial_jtag;
    } else if (initialize_uart()) {
        backend_ = Backend::uart0;
        usb_fallback_ = true;
    } else {
        backend_ = Backend::usb_serial_jtag;
    }
#else
    if (initialize_uart()) {
        backend_ = Backend::uart0;
    }
#endif

    esp_log_set_vprintf(ConsoleVprintf);
}

/**
 * @brief Installs and configures UART0 as the serial console fallback.
 */
bool Console::initialize_uart() {
    const uart_config_t configuration{
        .baud_rate = kConsoleBaud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
#if ESP_IDF_VERSION_MAJOR >= 6
        .rx_glitch_filt_thresh = 0,
#endif
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };

    if (!uart_is_driver_installed(UART_NUM_0) &&
        uart_driver_install(UART_NUM_0, kUartReceiveBufferSize, 0, 0, nullptr, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(UART_NUM_0, &configuration) != ESP_OK) {
        return false;
    }
#if ESP_IDF_VERSION_MAJOR >= 6
    return uart_set_pin(UART_NUM_0, U0TXD_GPIO_NUM, U0RXD_GPIO_NUM, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE) == ESP_OK;
#else
    return uart_set_pin(UART_NUM_0, UART_NUM_0_TXD_DIRECT_GPIO_NUM, UART_NUM_0_RXD_DIRECT_GPIO_NUM,
               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK;
#endif
}

/**
 * @brief Reads one character without blocking from the selected backend.
 */
bool Console::read_character(char& character) {
    switch (backend_) {
    case Backend::uart0: {
        auto byte = std::uint8_t{};
        if (uart_read_bytes(UART_NUM_0, &byte, 1, 0) != 1) {
            return false;
        }
        character = static_cast<char>(byte);
        return true;
    }
#if defined(NEWMOTA_CONSOLE_USB_CDC)
    case Backend::usb_cdc:
        esp_usb_console_poll_interrupts();
        return esp_usb_console_read_buf(&character, 1) == 1;
#endif
#if defined(NEWMOTA_CONSOLE_USB_SERIAL_JTAG)
    case Backend::usb_serial_jtag: {
        auto byte = std::uint8_t{};
        if (usb_serial_jtag_ll_read_rxfifo(&byte, 1) != 1) {
            return false;
        }
        character = static_cast<char>(byte);
        return true;
    }
#endif
    case Backend::uninitialized:
        return false;
    default:
        return false;
    }
}

/**
 * @brief Writes all supplied bytes through the selected backend.
 */
bool Console::write(const std::string_view text) {
    switch (backend_) {
    case Backend::uart0:
        return uart_write_bytes(UART_NUM_0, text.data(), text.size()) ==
               static_cast<int>(text.size());
#if defined(NEWMOTA_CONSOLE_USB_CDC)
    case Backend::usb_cdc: {
        std::size_t written = 0;
        while (written < text.size()) {
            const auto count =
                esp_usb_console_write_buf(text.data() + written, text.size() - written);
            if (count <= 0) {
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
        return true;
    }
#endif
#if defined(NEWMOTA_CONSOLE_USB_SERIAL_JTAG)
    case Backend::usb_serial_jtag: {
        std::size_t written = 0;
        auto deadline = esp_timer_get_time() + kUsbWriteTimeoutMicroseconds;
        while (written < text.size()) {
            if (!usb_serial_jtag_ll_txfifo_writable()) {
                if (esp_timer_get_time() >= deadline) {
                    return false;
                }
                vTaskDelay(1);
                continue;
            }
            const auto count = usb_serial_jtag_ll_write_txfifo(
                reinterpret_cast<const std::uint8_t*>(text.data() + written),
                text.size() - written);
            if (count == 0) {
                return false;
            }
            written += count;
            usb_serial_jtag_ll_txfifo_flush();
            deadline = esp_timer_get_time() + kUsbWriteTimeoutMicroseconds;
        }
        return true;
    }
#endif
    case Backend::uninitialized:
        return false;
    default:
        return false;
    }
}

/** @brief Describes the selected backend and any boot-time fallback. */
const char* Console::status() const {
    if (usb_fallback_) {
        return "Falling back to UART0; no USB SOF detected at boot";
    }
    switch (backend_) {
    case Backend::uart0:
        return "Using UART0";
    case Backend::usb_cdc:
        return "Using USB CDC";
    case Backend::usb_serial_jtag:
        return "Using USB Serial/JTAG";
    case Backend::uninitialized:
        return "Console not initialized";
    }
    return "Console not initialized";
}

/** @brief Reports the fixed UART rate when UART0 is the active backend. */
std::uint32_t Console::baudrate() const {
    return backend_ == Backend::uart0 ? static_cast<std::uint32_t>(kConsoleBaud) : 0U;
}

} // namespace newmota::platform
