/*
  spi_controller.h - native SPI controller owner for Bouffalo, AmebaD and Pico

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_controller.h
 * @brief One owner per hardware SPI controller, shared by devices with their own CS.
 *
 * ESP-IDF has its own host owner in `esp-idf/spi_host.h`; this contract covers the families
 * whose SDKs expose the controller registers directly. Users with identical clock, MOSI and
 * MISO pins share one controller; each transfer names its own clock and mode, and the owner
 * reprograms the controller only when the previous transfer used another format. CS is a
 * GPIO driven by the device driver around its transfers. Calls belong to the application task
 * and run synchronously: a transfer returns after the last bit left the wire.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform::spi_controller {

/** @brief Signals of one controller; @ref bus is zero-based (Tasmota's SPI bus 1 is 0). */
struct Wiring final {
    std::uint8_t bus{};
    int clock{-1}, mosi{-1}, miso{-1};
};

/** @brief Per-transfer framing; a shared controller changes it between devices. */
struct Format final {
    std::uint32_t hz{};   /**< Upper clock; the controller rounds down. */
    std::uint8_t mode{};  /**< SPI mode 0 to 3. */
    std::uint8_t bits{8}; /**< 8, or 9 for MSB-first 3-wire display commands. */
};

enum class Result : std::uint8_t {
    success,
    unsupported, /**< No controller, route, mode or rate for this request. */
    busy,        /**< The controller runs outside this owner. */
    conflict,    /**< The controller is leased with different pins. */
    timeout,
    failed,
};

/** @brief One claim on a controller; empty when @ref valid() is false. */
class Lease final {
  public:
    [[nodiscard]] constexpr bool valid() const { return bus_ != kNone; }

    [[nodiscard]] constexpr std::uint8_t bus() const { return bus_; }

  private:
    static constexpr std::uint8_t kNone = 0xff;
    friend Result acquire(const Wiring&, Lease&);
    friend Result release(Lease&);

    std::uint8_t bus_{kNone};
};

/** @brief Checks that the pins reach the controller and that it can clock @p format. */
[[nodiscard]] Result preflight(const Wiring& wiring, const Format& format);

/** @brief Starts the controller for its first user or joins identical wiring. */
[[nodiscard]] Result acquire(const Wiring& wiring, Lease& lease);

/** @brief Drops one lease; the last one stops the controller and releases its pins. */
[[nodiscard]] Result release(Lease& lease);

/**
 * @brief Clocks @p size bytes: sends @p tx (0xff when null) and stores into @p rx when set.
 * @p format must use 8 bits.
 */
[[nodiscard]] Result exchange(const Lease& lease, const Format& format, const std::uint8_t* tx,
    std::uint8_t* rx, std::size_t size);

/**
 * @brief Sends 9-bit words, each byte below a leading @p high bit (3-wire display data/command).
 */
[[nodiscard]] Result write_9bit(const Lease& lease, const Format& format, bool high,
    const std::uint8_t* bytes, std::size_t size);

} // namespace newmota::platform::spi_controller
