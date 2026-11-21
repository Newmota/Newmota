/*
  spi_service.h - Shared bounded general device SPI ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file spi_service.h
 * @brief Shared bounded general device SPI ownership.
 */

#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::device_spi {
inline constexpr std::size_t kMaximumBuses = 4;
inline constexpr std::size_t kMaximumLeases = 16;
inline constexpr std::size_t kMaximumDevices = 16;
inline constexpr std::size_t kMaximumTransfer = 256;
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unsupported,
    busy,
    no_memory,
    failed,
    timeout
};

/** @brief GPIO backend physical wires; MISO or MOSI may be omitted for one-way devices. */
struct BusConfiguration {
    PinId clock{}, mosi{}, miso{};
};

/** @brief Independent lease on one physical bus, shared only by identical wire configurations. */
struct Bus {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Modes are CPOL/CPHA bits; rate is an upper bound, not a qualified wire frequency. */
struct DeviceConfiguration {
    PinId select{};
    std::uint32_t frequency_hz{100000};
    std::uint16_t setup_us{1}, hold_us{2};
    std::uint8_t mode{};
    bool select_active_high{}, lsb_first{};
};

struct Device {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Claims/configures wires or shares an existing bus through a distinct lease.
 * @param bus Populated after claims are acquired, even on failed GPIO initialization; retain
 * it and call close() to retry confirmed cleanup. Other failures leave an empty handle.
 */
Result open(const BusConfiguration&, Bus& bus);
/** @brief Releases this lease; the final lease releases wires only after confirmed cleanup. */
Result close(Bus);
/** @brief Claims a distinct CS on a healthy bus lease.
 * @param device Retained on uncertain GPIO failure so close() can retry cleanup.
 */
Result attach(Bus, const DeviceConfiguration&, Device& device);
/** @brief Deasserts and releases CS only after confirmed GPIO cleanup. */
Result close(Device);
/** @brief Reports a healthy configured device and its retained bus lease. */
bool active(Device);
/** @brief Atomically holds the bus and CS through one bounded full-duplex transaction.
 * @param write Null sends 0xff; otherwise supplies size bytes. In-place write/read is supported.
 * @param read Null discards input; otherwise receives size bytes and requires MISO.
 * @param timeout_ms Maximum application-task occupation, 1..100 ms including CS delays.
 * No call is permitted from IRQs or another core/task. GPIO edges may stretch during interrupts.
 */
Result transfer(Device, const std::uint8_t* write, std::uint8_t* read, std::size_t size,
    std::uint32_t timeout_ms = 25);
} // namespace newmota::device_spi
