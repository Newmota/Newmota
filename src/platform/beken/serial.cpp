/*
  serial.cpp - Beken native serial adapter

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
#include "serial_native.h"
#include "newmota/platform/platform.h"
#include <initializer_list>

namespace newmota::platform {
/** @brief Reports whether the Beken console was stopped for the template; see platform.cpp. */
bool beken_console_released();
} // namespace newmota::platform

namespace newmota::platform::serial {
namespace {
RxRing ring;
Config config;
bool active{};
std::atomic<bool> idle{true};
std::uint32_t idle_after{};

/** @brief Captures bytes/errors without allocating or calling application code. */
void interrupt() {
    const auto status = newmota_beken_serial_interrupt();
    if (status & 1U)
        ring.error();
    for (unsigned n = 0; n < 128; ++n) {
        const int byte = newmota_beken_serial_read();
        if (byte < 0)
            break;
        ring.push(static_cast<std::uint8_t>(byte));
    }
    if (status & 2U)
        idle.store(true, std::memory_order_release);
}
} // namespace

/** @brief Selects the UART of the given pads: 2 for GPIO0 Tx/GPIO1 Rx, 1 for GPIO11 Tx/GPIO10 Rx,
 * 0 for any other combination.
 */
int uart_of(const Config& c) {
    const bool uart2 = (!c.rx.valid() || c.rx.value() == 1) && (!c.tx.valid() || c.tx.value() == 0);
    const bool uart1 =
        (!c.rx.valid() || c.rx.value() == 10) && (!c.tx.valid() || c.tx.value() == 11);
    return uart2 ? 2 : uart1 ? 1 : 0;
}

/** @brief Opens UART2 on GPIO0/1 or UART1 on GPIO10/11. A UART that carries the console is
 * taken only once the console is off; SDK log output then moves to the other UART's pads-free
 * transmitter, as Tasmota's ClaimSerial gives up the serial log.
 */
Result open(const Config& c, Port& p) {
    if ((!c.rx.valid() && !c.tx.valid()) || !c.baud || c.data_bits < 5 || c.data_bits > 8 ||
        c.stop_bits < 1 || c.stop_bits > 2 || !c.rx_capacity || c.rx_capacity > kMaximumRxCapacity)
        return Result::invalid_argument;
    if ((c.rx.valid() && !(gpio_capabilities(c.rx) & capability_mask(PinCapability::input))) ||
        (c.tx.valid() && !(gpio_capabilities(c.tx) & capability_mask(PinCapability::output))))
        return Result::unavailable;
    const int uart = uart_of(c);
    if (c.inverted || uart == 0 || c.baud > 3250000)
        return Result::unsupported;
    if (active)
        return Result::busy;
    if (newmota_beken_serial_reserved(uart)) {
        (void)release_console();
        if (!beken_console_released())
            return Result::busy;
    }
    if (!ring.allocate(c.rx_capacity))
        return Result::no_memory;
    if (newmota_beken_serial_reserved(uart))
        newmota_beken_serial_move_log(uart);
    newmota_beken_serial_start(uart, c.baud, c.data_bits, static_cast<unsigned>(c.parity),
        c.stop_bits, c.rx.valid(), c.tx.valid(), interrupt);
    config = c;
    active = true;
    idle = true;
    idle_after = monotonic_milliseconds();
    p = 1;
    newmota_beken_serial_irq(1);
    return Result::success;
}

/** @brief Stops IRQ capture before releasing the receive ring and pad muxes. */
Result close(Port p) {
    if (p != 1 || !active)
        return Result::invalid_argument;
    newmota_beken_serial_stop();
    bool ok = true;
    for (auto pin : {config.rx, config.tx})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            ok = false;
    ring.release();
    active = false;
    return ok ? Result::success : Result::failed;
}

/** @brief Returns unread IRQ-captured bytes. */
std::size_t available(Port p) {
    return p == 1 && active ? ring.available() : 0;
}

/** @brief Consumes or peeks currently buffered bytes. */
Transfer read(Port p, std::uint8_t* data, std::size_t size, bool peek) {
    if (p != 1 || !active || (!data && size))
        return {Result::invalid_argument, 0};
    return ring.read(data, size, peek);
}

/** @brief Writes one bounded FIFO burst without waiting for wire drain. */
Transfer write_some(Port p, const std::uint8_t* data, std::size_t size) {
    if (p != 1 || !active || !config.tx.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    newmota_beken_serial_irq(0);
    const auto n = newmota_beken_serial_write(data, size);
    if (n) {
        const auto now = monotonic_milliseconds();
        const auto start = now - idle_after < 0x80000000U ? now : idle_after;
        const auto bits =
            1U + config.data_bits + config.stop_bits + (config.parity != Parity::none ? 1U : 0U);
        // A previous burst's stop IRQ can overlap new FIFO writes. Require a
        // conservative full-frame duration in addition to the completion IRQ.
        idle_after =
            start + static_cast<std::uint32_t>((n * bits * 1000U + config.baud - 1) / config.baud) +
            1;
        idle.store(false, std::memory_order_release);
    }
    newmota_beken_serial_irq(1);
    return {Result::success, n};
}

/** @brief Requires the hardware's stop-bit completion plus an empty FIFO. */
bool tx_idle(Port p) {
    return p == 1 && active && idle.load(std::memory_order_acquire) &&
           monotonic_milliseconds() - idle_after < 0x80000000U && newmota_beken_serial_fifo_empty();
}

/** @brief Discards staged RX bytes while preserving capture. */
void discard_rx(Port p) {
    if (p == 1 && active)
        ring.discard();
}

/** @brief Returns accumulated overflow and native error counters. */
Status status(Port p) {
    return p == 1 && active ? ring.status() : Status{};
}
} // namespace newmota::platform::serial
