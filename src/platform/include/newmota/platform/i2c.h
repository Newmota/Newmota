/*
  i2c.h - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::i2c {
inline constexpr std::size_t kBusCount = 2;
inline constexpr std::size_t kMaxTransfer = 128;
enum class Result : std::uint8_t {
    success,
    unavailable,
    invalid_argument,
    busy,
    nack,
    timeout,
    failed
};
// Bus indices are zero based. All calls belong to the cooperative application task.
// External pull-ups are required unless a bus enables the weak internal ones, as camera
// SCCB wiring expects. High levels release the pad; never drive high.
/** @brief Reports support for a zero-based bus index. */
[[nodiscard]] bool supported(std::uint8_t bus);
/** @brief Reports runtime activation of the configured bus pins. */
[[nodiscard]] bool available(std::uint8_t bus);
/** @brief Initializes one retained bus with both pads released. */
[[nodiscard]] Result configure(
    std::uint8_t bus, PinId scl, PinId sda, bool internal_pull_up = false);
// A write followed by a read uses repeated START. Empty transfer probes address.
// One complete operation owns the bus through STOP, including on error.
/** @brief Completes a bounded atomic probe, write, read or repeated-start transfer. */
[[nodiscard]] Result transfer(std::uint8_t bus, std::uint8_t address, const std::uint8_t* write,
    std::size_t write_size, std::uint8_t* read, std::size_t read_size,
    std::uint32_t timeout_ms = 25);
} // namespace newmota::platform::i2c
