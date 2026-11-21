/*
  serial.cpp - ESP-IDF native serial adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file serial.cpp
 * @brief Configured UART ownership and bounded receive storage.
 */
#include "newmota/platform/serial_ring.h"
#include "newmota/platform/gpio.h"
#include "driver/uart.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"
#include "console.h"
#include <array>

namespace newmota::platform::serial {
namespace {
#ifdef SOC_UART_HP_NUM
constexpr unsigned kCount = SOC_UART_HP_NUM;
#else
constexpr unsigned kCount = SOC_UART_NUM;
#endif
struct State {
    bool active{};
    Config config{};
    QueueHandle_t events{};
    RxRing ring{};
};

std::array<State, kCount> states{};

/** @brief Protects the actual console controller and installed native UART drivers. */
bool reserved(unsigned p) {
    if (p == 0 && console().baudrate() != 0)
        return true;
#if CONFIG_ESP_CONSOLE_UART
    if (p == CONFIG_ESP_CONSOLE_UART_NUM)
        return true;
#endif
#if defined(CONFIG_ESP_CONSOLE_SECONDARY_UART) && CONFIG_ESP_CONSOLE_SECONDARY_UART
    if (p == CONFIG_ESP_CONSOLE_UART_NUM)
        return true;
#endif
    return uart_is_driver_installed(static_cast<uart_port_t>(p));
}

/** @brief Drains bounded native input into retained buffers and records RX errors. */
void capture(Port p) {
    auto& s = states[p];
    uart_event_t event{};
    for (unsigned i = 0; i < 16 && xQueueReceive(s.events, &event, 0) == pdTRUE; ++i)
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR)
            s.ring.error();
    std::uint8_t data[64];
    // Staging only consumes bytes that fit; peek never discards unread data.
    while (s.ring.available() < s.config.rx_capacity) {
        const auto space = s.config.rx_capacity - s.ring.available();
        const int n = uart_read_bytes(
            static_cast<uart_port_t>(p), data, space < sizeof(data) ? space : sizeof(data), 0);
        if (n <= 0)
            break;
        for (int i = 0; i < n; ++i)
            s.ring.push(data[i]);
    }
}
} // namespace

/** @brief Validates and opens an exclusively owned native UART port. */
Result open(const Config& c, Port& p) {
    if ((!c.rx.valid() && !c.tx.valid()) || !c.baud || c.data_bits < 5 || c.data_bits > 8 ||
        c.stop_bits < 1 || c.stop_bits > 2 || !c.rx_capacity || c.rx_capacity > kMaximumRxCapacity)
        return Result::invalid_argument;
    if ((c.rx.valid() && !(gpio_capabilities(c.rx) & capability_mask(PinCapability::input))) ||
        (c.tx.valid() && !(gpio_capabilities(c.tx) & capability_mask(PinCapability::output))))
        return Result::unavailable;
    unsigned i{};
    while (i < kCount && (states[i].active || reserved(i)))
        ++i;
    if (i == kCount)
        return Result::busy;
    auto& s = states[i];
    if (!s.ring.allocate(c.rx_capacity))
        return Result::no_memory;
    const auto port = static_cast<uart_port_t>(i);
    uart_config_t cfg{};
    cfg.baud_rate = static_cast<int>(c.baud);
    cfg.data_bits = static_cast<uart_word_length_t>(c.data_bits - 5);
    cfg.parity = c.parity == Parity::none   ? UART_PARITY_DISABLE
                 : c.parity == Parity::even ? UART_PARITY_EVEN
                                            : UART_PARITY_ODD;
    cfg.stop_bits = c.stop_bits == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(port, c.rx_capacity < 256 ? 256 : c.rx_capacity, 0, 16, &s.events, 0) !=
        ESP_OK) {
        s.ring.release();
        return Result::no_memory;
    }
    bool ok = uart_param_config(port, &cfg) == ESP_OK &&
              uart_set_pin(port, c.tx.valid() ? c.tx.value() : UART_PIN_NO_CHANGE,
                  c.rx.valid() ? c.rx.value() : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                  UART_PIN_NO_CHANGE) == ESP_OK &&
              uart_set_line_inverse(
                  port, c.inverted ? (UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV) : 0) == ESP_OK;
    s.active = true;
    s.config = c;
    p = static_cast<Port>(i);
    if (!ok)
        return close(p) == Result::success ? Result::unsupported : Result::failed;
    return Result::success;
}

/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Port p) {
    if (p >= kCount || !states[p].active)
        return Result::invalid_argument;
    auto& s = states[p];
    if (uart_driver_delete(static_cast<uart_port_t>(p)) != ESP_OK)
        return Result::failed;
    bool ok = true;
    for (auto pin : {s.config.rx, s.config.tx})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            ok = false;
    s.active = false;
    s.ring.release();
    return ok ? Result::success : Result::failed;
}

/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Port p) {
    if (p >= kCount || !states[p].active)
        return 0;
    capture(p);
    return states[p].ring.available();
}

/** @brief Peeks or consumes currently buffered bytes without waiting for input. */
Transfer read(Port p, std::uint8_t* data, std::size_t size, bool peek) {
    if (p >= kCount || !states[p].active || (!data && size))
        return {Result::invalid_argument, 0};
    capture(p);
    return states[p].ring.read(data, size, peek);
}

/** @brief Accepts a bounded FIFO burst and reports the accepted byte count. */
Transfer write_some(Port p, const std::uint8_t* data, std::size_t size) {
    if (p >= kCount || !states[p].active || !states[p].config.tx.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    const int n = uart_tx_chars(static_cast<uart_port_t>(p), reinterpret_cast<const char*>(data),
        size > 128 ? 128 : static_cast<std::uint32_t>(size));
    return {n < 0 ? Result::failed : Result::success, n < 0 ? 0U : static_cast<std::size_t>(n)};
}

/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Port p) {
    if (p >= kCount || !states[p].active)
        return false;
    return uart_wait_tx_done(static_cast<uart_port_t>(p), 0) == ESP_OK;
}

/** @brief Discards queued RX bytes without changing port configuration. */
void discard_rx(Port p) {
    if (p >= kCount || !states[p].active)
        return;
    (void)uart_flush_input(static_cast<uart_port_t>(p));
    states[p].ring.discard();
}

/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Port p) {
    if (p >= kCount || !states[p].active)
        return {};
    capture(p);
    return states[p].ring.status();
}
} // namespace newmota::platform::serial
