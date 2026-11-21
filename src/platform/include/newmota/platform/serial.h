/*
  serial.h - Platform serial ownership and I/O contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file serial.h
 * @brief Portable UART configuration, ownership and nonblocking byte-transfer contract.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::serial {
/** @brief Default RX allocation; ordinary UART instances keep their existing footprint. */
inline constexpr std::size_t kCapacity = 256;
/** @brief Largest explicitly requested RX allocation, needed by Standard Teleinfo frames. */
inline constexpr std::size_t kMaximumRxCapacity = 1536;
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unavailable,
    unsupported,
    busy,
    no_memory,
    timeout,
    failed
};
enum class Parity : std::uint8_t { none, even, odd };

struct Config {
    PinId rx{}, tx{};
    std::uint32_t baud{9600};
    std::uint8_t data_bits{8}, stop_bits{1};
    Parity parity{Parity::none};
    bool inverted{};
    std::size_t rx_capacity{kCapacity};
};

struct Transfer {
    Result result{Result::success};
    std::size_t count{};
};

struct Status {
    std::uint32_t dropped{}, errors{};
};

using Port = std::uint8_t;
// Calls belong to the application task. Capture continues in SDK/IRQ context.
// Every failed open except `failed` has left native hardware untouched or safely
// disabled. `failed` requires quarantining pins because cleanup is uncertain.
/** @brief Validates and opens an exclusively owned native UART port. */
Result open(const Config&, Port&);
/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Port);
/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Port);
/** @brief Peeks or consumes currently buffered bytes without waiting for input. */
Transfer read(Port, std::uint8_t*, std::size_t, bool peek = false);
/** @brief Accepts a bounded FIFO burst and reports the accepted byte count. */
Transfer write_some(Port, const std::uint8_t*, std::size_t);
/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Port); // FIFO and shift register empty, including the final stop bit.
/** @brief Discards queued RX bytes without changing port configuration. */
void discard_rx(Port);
/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Port);
} // namespace newmota::platform::serial
