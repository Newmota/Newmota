/*
  pin.h - portable physical pin identity for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

class PinId final {
  public:
    static constexpr std::uint16_t kInvalid = 0xffffU;

    /** @brief Constructs an invalid physical pin identifier. */
    constexpr PinId() = default;

    /** @brief Constructs a physical pin identifier from its platform number. */
    explicit constexpr PinId(const std::uint16_t value) : value_(value) {}

    /** @brief Returns the platform's numeric physical pin identifier. */
    [[nodiscard]] constexpr std::uint16_t value() const { return value_; }

    /** @brief Reports whether this identifier names a physical pin. */
    [[nodiscard]] constexpr bool valid() const { return value_ != kInvalid; }

  private:
    std::uint16_t value_{kInvalid};
};

enum class PinCapability : std::uint16_t {
    input = 1U << 0U,
    output = 1U << 1U,
    pull_up = 1U << 2U,
    pull_down = 1U << 3U,
    open_drain = 1U << 4U,
    interrupt = 1U << 5U,
    analog = 1U << 6U,
};

/** @brief Returns the compact mask for one physical-pin capability. */
[[nodiscard]] constexpr std::uint16_t capability_mask(const PinCapability capability) {
    return static_cast<std::uint16_t>(capability);
}

} // namespace newmota
