/*
  spi_host.h - ESP-IDF SPI controller owner for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_host.h
 * @brief Single owner of the GPSPI controllers' bus initialization.
 *
 * Every Newmota user of an ESP-IDF SPI host takes a lease here instead of calling
 * `spi_bus_initialize()` or `spi_bus_free()`. Shared users with identical pins attach their
 * own devices (`spi_bus_add_device()`) to one initialized bus; the last lease frees the bus.
 * An exclusive lease keeps every other user off the host. Calls belong to the application
 * task; device transactions may run on any task, as ESP-IDF serializes them per bus.
 */
#pragma once

#include "driver/spi_master.h"

#include <cstdint>

namespace newmota::platform::spi_host {

/** @brief Largest transfer on a shared bus: the display's 4096-byte scratch chunks. */
inline constexpr int kSharedTransferBytes = 4096;

enum class Use : std::uint8_t {
    shared,    /**< Devices attach to a bus with the same pins; max transfer is fixed. */
    exclusive, /**< One user owns the host and its bus configuration. */
};

enum class Result : std::uint8_t {
    success,
    unsupported, /**< The host does not exist on this chip. */
    busy,        /**< An exclusive lease holds the host, or shared users block one. */
    conflict,    /**< The host is shared with different pins, flags or a larger transfer. */
    no_memory,
    failed,
};

/** @brief A claim on one host; empty when @ref valid() is false. */
class Lease final {
  public:
    [[nodiscard]] constexpr bool valid() const { return held_; }

    [[nodiscard]] constexpr spi_host_device_t host() const { return host_; }

  private:
    friend Result acquire(spi_host_device_t, const spi_bus_config_t&, Use, Lease&);
    friend Result release(Lease&);

    spi_host_device_t host_{SPI2_HOST};
    bool held_{false};
};

/**
 * @brief Initializes @p host for the first user or joins an identical shared bus.
 *
 * A shared user's `max_transfer_sz` must not exceed @ref kSharedTransferBytes; the bus is
 * always initialized with that size so that the arrival order of users does not matter.
 * @return success when @p lease holds the host; @p lease stays empty otherwise.
 */
[[nodiscard]] Result acquire(
    spi_host_device_t host, const spi_bus_config_t& bus, Use use, Lease& lease);

/**
 * @brief Drops one lease after its devices are removed; the last lease frees the bus.
 * @return failed when ESP-IDF refuses to free the bus; @p lease is kept for a retry.
 */
[[nodiscard]] Result release(Lease& lease);

} // namespace newmota::platform::spi_host
