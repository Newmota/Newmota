/*
  serial.cpp - Pico SDK native serial adapter

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
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/regs/uart.h"
#include <array>

namespace newmota::platform::serial {
namespace {
struct State {
    bool active{};
    Config config{};
    RxRing ring{};
};

std::array<State, 2> states{};

/** @brief Maps a validated portable port number to its SDK UART instance. */
uart_inst_t* uart(Port p) {
    return p ? uart1 : uart0;
}

/** @brief Maps a validated portable port number to its native IRQ number. */
unsigned irq(Port p) {
    return p ? UART1_IRQ : UART0_IRQ;
}

/** @brief Maps physical RX or TX pins using the pinned SDK UART route table. */
int route(PinId pin, bool rx) {
    if (!pin.valid())
        return -1;
    // RP2040 and RP2350 UART TX/RX occupy the same function-2 route table.
#if PICO_RP2040
    if (pin.value() % 4 != (rx ? 1 : 0))
        return -2;
#else
    if (pin.value() % 2 != (rx ? 1 : 0))
        return -2;
#endif
    return static_cast<int>(((pin.value() + 4) / 8) % 2);
}

/** @brief Drains bounded native input into retained buffers and records RX errors. */
void capture(Port p) {
    auto* u = uart(p);
    while (uart_is_readable(u)) {
        const auto value = uart_get_hw(u)->dr;
        if (value & 0xf00U)
            states[p].ring.error();
        states[p].ring.push(static_cast<std::uint8_t>(value));
    }
    uart_get_hw(u)->icr = UART_UARTICR_RTIC_BITS | UART_UARTICR_OEIC_BITS | UART_UARTICR_BEIC_BITS |
                          UART_UARTICR_PEIC_BITS | UART_UARTICR_FEIC_BITS;
}

/** @brief Captures UART0 RX bytes from interrupt context. */
void irq0() {
    capture(0);
}

/** @brief Captures UART1 RX bytes from interrupt context. */
void irq1() {
    capture(1);
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
    if (c.inverted)
        return Result::unsupported;
    const int rx = route(c.rx, true), tx = route(c.tx, false);
    if (rx == -2 || tx == -2 || (rx >= 0 && tx >= 0 && rx != tx))
        return Result::unsupported;
    p = static_cast<Port>(rx >= 0 ? rx : tx);
    auto& s = states[p];
    if (s.active || uart_is_enabled(uart(p)) || irq_has_handler(irq(p)))
        return Result::busy;
    if (!s.ring.allocate(c.rx_capacity))
        return Result::no_memory;
    s.config = c;
    uart_init(uart(p), c.baud);
    uart_set_format(uart(p), c.data_bits, c.stop_bits,
        c.parity == Parity::none   ? UART_PARITY_NONE
        : c.parity == Parity::even ? UART_PARITY_EVEN
                                   : UART_PARITY_ODD);
    uart_set_hw_flow(uart(p), false, false);
    uart_set_fifo_enabled(uart(p), true);
    for (auto pin : {c.rx, c.tx})
        if (pin.valid())
            gpio_set_function(pin.value(), UART_FUNCSEL_NUM(uart(p), pin.value()));
    irq_set_exclusive_handler(irq(p), p ? irq1 : irq0);
    irq_set_enabled(irq(p), true);
    uart_set_irq_enables(uart(p), c.rx.valid(), false);
    s.active = true;
    return Result::success;
}

/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Port p) {
    if (p >= states.size() || !states[p].active)
        return Result::invalid_argument;
    uart_set_irq_enables(uart(p), false, false);
    irq_set_enabled(irq(p), false);
    irq_remove_handler(irq(p), p ? irq1 : irq0);
    uart_deinit(uart(p));
    for (auto pin : {states[p].config.rx, states[p].config.tx})
        if (pin.valid()) {
            gpio_set_function(pin.value(), GPIO_FUNC_SIO);
            gpio_set_dir(pin.value(), false);
            gpio_disable_pulls(pin.value());
        }
    states[p].ring.release();
    states[p].active = false;
    return Result::success;
}

/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Port p) {
    if (p >= states.size() || !states[p].active)
        return 0;
    return states[p].ring.available();
}

/** @brief Peeks or consumes currently buffered bytes without waiting for input. */
Transfer read(Port p, std::uint8_t* data, std::size_t size, bool peek) {
    if (p >= states.size() || !states[p].active || (!data && size))
        return {Result::invalid_argument, 0};
    return states[p].ring.read(data, size, peek);
}

/** @brief Accepts a bounded FIFO burst and reports the accepted byte count. */
Transfer write_some(Port p, const std::uint8_t* data, std::size_t size) {
    if (p >= states.size() || !states[p].active || !states[p].config.tx.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    std::size_t n{};
    while (n < size && n < 32 && uart_is_writable(uart(p)))
        uart_get_hw(uart(p))->dr = data[n++];
    return {Result::success, n};
}

/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Port p) {
    if (p >= states.size() || !states[p].active)
        return false;
    return !(uart_get_hw(uart(p))->fr & UART_UARTFR_BUSY_BITS);
}

/** @brief Discards queued RX bytes without changing port configuration. */
void discard_rx(Port p) {
    if (p >= states.size() || !states[p].active)
        return;
    states[p].ring.discard();
}

/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Port p) {
    if (p >= states.size() || !states[p].active)
        return {};
    return states[p].ring.status();
}
} // namespace newmota::platform::serial
