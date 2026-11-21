/*
  serial.cpp - Bouffalo native serial adapter

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
#include <initializer_list>
extern "C" {
#include "bflb_uart.h"
#include "bflb_gpio.h"
#include "bflb_irq.h"
#include "hardware/uart_reg.h"
}

namespace newmota::platform::serial {
namespace {
RxRing ring;
bflb_device_s* device{};
Config config;
bool active{};

/** @brief Captures native RX bytes and error/completion events without VM callbacks. */
void interrupt(int, void*) {
    const auto status = bflb_uart_get_intstatus(device);
    if (status & (UART_INTSTS_PCE | UART_INTSTS_RX_FER))
        ring.error();
    for (unsigned n = 0; n < 128 && bflb_uart_rxavailable(device); ++n)
        ring.push(static_cast<std::uint8_t>(bflb_uart_getchar(device)));
    bflb_uart_int_clear(device, UART_INTCLR_RTO | UART_INTCLR_PCE | UART_INTCLR_RX_END);
}

/** @brief Maps physical RX or TX pins using the pinned SDK UART route table. */
bool route(PinId pin) {
    // BL602 has eight shared UART signal slots: the console uses slots 0/7.
    // Remapping an alias of either slot would silently disconnect UART0.
#if !defined(BL602)
    return !pin.valid();
#else
    return !pin.valid() || (pin.value() < 23 && pin.value() % 8 != 0 && pin.value() % 8 != 7);
#endif
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
    if (c.inverted || !route(c.rx) || !route(c.tx) ||
        (c.rx.valid() && c.tx.valid() && c.rx.value() % 8 == c.tx.value() % 8))
        return Result::unsupported;
    if (active)
        return Result::busy;
    device = bflb_device_get_by_name("uart1");
    auto* gpio = bflb_device_get_by_name("gpio");
    if (!device || !gpio)
        return Result::unavailable;
    if (!ring.allocate(c.rx_capacity))
        return Result::no_memory;
    bflb_uart_config_s cfg{};
    cfg.baudrate = c.baud;
    cfg.direction = (c.rx.valid() ? UART_DIRECTION_RX : 0) | (c.tx.valid() ? UART_DIRECTION_TX : 0);
    cfg.data_bits = c.data_bits - 5;
    cfg.stop_bits = c.stop_bits == 1 ? UART_STOP_BITS_1 : UART_STOP_BITS_2;
    cfg.parity = c.parity == Parity::none   ? UART_PARITY_NONE
                 : c.parity == Parity::even ? UART_PARITY_EVEN
                                            : UART_PARITY_ODD;
    cfg.rx_fifo_threshold = 1;
    cfg.tx_fifo_threshold = 1;
    bflb_uart_init(device, &cfg);
    if (c.rx.valid())
        bflb_gpio_uart_init(gpio, static_cast<std::uint8_t>(c.rx.value()), GPIO_UART_FUNC_UART1_RX);
    if (c.tx.valid())
        bflb_gpio_uart_init(gpio, static_cast<std::uint8_t>(c.tx.value()), GPIO_UART_FUNC_UART1_TX);
    bflb_uart_feature_control(device, UART_CMD_SET_RTO_VALUE, 8);
    bflb_uart_txint_mask(device, true);
    bflb_uart_rxint_mask(device, !c.rx.valid());
    bflb_uart_errint_mask(device, false);
    bflb_irq_attach(device->irq_num, interrupt, nullptr);
    bflb_irq_enable(device->irq_num);
    config = c;
    active = true;
    p = 1;
    return Result::success;
}

/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Port p) {
    if (p != 1 || !active)
        return Result::invalid_argument;
    bflb_irq_disable(device->irq_num);
    bflb_irq_detach(device->irq_num);
    bflb_uart_deinit(device);
    bool ok = true;
    for (auto pin : {config.rx, config.tx})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            ok = false;
    ring.release();
    active = false;
    return ok ? Result::success : Result::failed;
}

/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Port p) {
    if (p != 1 || !active)
        return 0;
    return ring.available();
}

/** @brief Peeks or consumes currently buffered bytes without waiting for input. */
Transfer read(Port p, std::uint8_t* data, std::size_t size, bool peek) {
    if (p != 1 || !active || (!data && size))
        return {Result::invalid_argument, 0};
    return ring.read(data, size, peek);
}

/** @brief Accepts a bounded FIFO burst and reports the accepted byte count. */
Transfer write_some(Port p, const std::uint8_t* data, std::size_t size) {
    if (p != 1 || !active || !config.tx.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    std::size_t n{};
    while (n < size && n < UART_FIFO_MAX && bflb_uart_txready(device))
        bflb_uart_putchar(device, data[n++]);
    return {Result::success, n};
}

/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Port p) {
    if (p != 1 || !active)
        return false;
    return bflb_uart_txempty(device) &&
           !(getreg32(device->reg_base + UART_STATUS_OFFSET) & UART_STS_UTX_BUS_BUSY);
}

/** @brief Discards queued RX bytes without changing port configuration. */
void discard_rx(Port p) {
    if (p != 1 || !active)
        return;
    ring.discard();
}

/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Port p) {
    if (p != 1 || !active)
        return {};
    return ring.status();
}
} // namespace newmota::platform::serial
