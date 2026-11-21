/*
  serial.cpp - AmebaD native serial adapter

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
extern "C" {
#include "ameba_soc.h"
}
#include <array>

namespace newmota::platform::serial {
namespace {
struct State {
    bool active{};
    Config config{};
    RxRing ring{};
};

std::array<State, 4> states{};

/** @brief Maps physical RX or TX pins using the pinned SDK UART route table. */
int route(PinId pin, bool rx) {
    if (!pin.valid())
        return -1;
    const unsigned value = pin.value();
    if (rx ? (value == 13 || value == 34 || value == 25)
           : (value == 12 || value == 33 || value == 26))
        return 3;
    if (rx ? (value == 19 || value == 22 || value == 40 || value == 50)
           : (value == 18 || value == 21 || value == 41 || value == 51))
        return 0;
    return -2;
}

/** @brief Captures native RX bytes and error/completion events without VM callbacks. */
uint32_t interrupt(void* context) {
    auto* s = static_cast<State*>(context);
    const auto index = static_cast<unsigned>(s - states.data());
    auto* u = UART_DEV_TABLE[index].UARTx;
    const auto status = UART_LineStatusGet(u);
    if (status & RUART_LINE_STATUS_ERR)
        s->ring.error();
    for (unsigned n = 0; n < 128 && UART_Readable(u); ++n) {
        u8 byte{};
        UART_CharGet(u, &byte);
        s->ring.push(byte);
    }
    return 0;
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
    if (c.inverted || c.data_bits < 7)
        return Result::unsupported;
    const int rx = route(c.rx, true), tx = route(c.tx, false);
    if (rx == -2 || tx == -2 || (rx >= 0 && tx >= 0 && rx != tx))
        return Result::unsupported;
    p = static_cast<Port>(rx >= 0 ? rx : tx);
    auto& s = states[p];
    if (s.active)
        return Result::busy;
    if (!s.ring.allocate(c.rx_capacity))
        return Result::no_memory;
    auto* u = UART_DEV_TABLE[p].UARTx;
    UART_InitTypeDef cfg{};
    UART_StructInit(&cfg);
    cfg.WordLen = c.data_bits == 8 ? RUART_WLS_8BITS : RUART_WLS_7BITS;
    cfg.StopBit = c.stop_bits == 1 ? RUART_STOP_BIT_1 : RUART_STOP_BIT_2;
    cfg.Parity = c.parity == Parity::none ? RUART_PARITY_DISABLE : RUART_PARITY_ENABLE;
    cfg.ParityType = c.parity == Parity::even ? RUART_EVEN_PARITY : RUART_ODD_PARITY;
    UART_Init(u, &cfg);
    UART_SetBaud(u, c.baud);
    for (auto pin : {c.rx, c.tx})
        if (pin.valid()) {
            Pinmux_Config(pin.value(), PINMUX_FUNCTION_UART);
            PAD_PullCtrl(pin.value(), GPIO_PuPd_UP);
        }
    UART_RxCmd(u, c.rx.valid() ? ENABLE : DISABLE);
    s.config = c;
    InterruptRegister(reinterpret_cast<IRQ_FUN>(interrupt), UART_DEV_TABLE[p].IrqNum,
        reinterpret_cast<u32>(&s), 5);
    InterruptEn(UART_DEV_TABLE[p].IrqNum, 5);
    UART_INTConfig(
        u, RUART_IER_ERBI | RUART_IER_ELSI | RUART_IER_ETOI, c.rx.valid() ? ENABLE : DISABLE);
    s.active = true;
    return Result::success;
}

/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Port p) {
    if (p >= states.size() || !states[p].active)
        return Result::invalid_argument;
    InterruptDis(UART_DEV_TABLE[p].IrqNum);
    InterruptUnRegister(UART_DEV_TABLE[p].IrqNum);
    UART_DeInit(UART_DEV_TABLE[p].UARTx);
    bool ok = true;
    for (auto pin : {states[p].config.rx, states[p].config.tx})
        if (pin.valid() && gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
            ok = false;
    states[p].ring.release();
    states[p].active = false;
    return ok ? Result::success : Result::failed;
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
    auto* u = UART_DEV_TABLE[p].UARTx;
    std::size_t n{};
    while (n < size && n < 64 && UART_Writable(u))
        UART_CharPut(u, data[n++]);
    return {Result::success, n};
}

/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Port p) {
    if (p >= states.size() || !states[p].active)
        return false;
    return (UART_LineStatusGet(UART_DEV_TABLE[p].UARTx) & RUART_LINE_STATUS_REG_TEMT) != 0;
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
