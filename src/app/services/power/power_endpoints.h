/*
  power_endpoints.h - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "core/hardware/hardware.h"

namespace newmota {
enum class PowerOwner : std::uint8_t { none, relay, light };

struct PowerEndpoint {
    PowerOwner owner{};
    std::uint8_t instance{};
};

/** Boundary projection only: state and relay policies stay with each owner. */
class PowerEndpoints final {
  public:
    /** @param extra_relays Relays without a pin, one bit per one-based instance. */
    [[nodiscard]] bool configure(const Hardware& hardware, const std::uint8_t light_count,
        const std::uint32_t extra_relays = 0U) {
        entries_ = {};
        count_ = 0U;
        std::uint8_t highest = 0U;
        for (std::uint8_t i = 1U; i <= 32U; ++i) {
            if (hardware.find_function(PinRole::relay, i) == nullptr &&
                (extra_relays & (1U << (i - 1U))) == 0U)
                continue;
            entries_[i - 1U] = {PowerOwner::relay, i};
            highest = i;
            ++count_;
        }
        if (static_cast<unsigned>(highest) + light_count > entries_.size())
            return false;
        for (std::uint8_t i = 0U; i < light_count; ++i) {
            entries_[highest + i] = {PowerOwner::light, static_cast<std::uint8_t>(i + 1U)};
            ++count_;
        }
        return true;
    }

    [[nodiscard]] PowerEndpoint get(const std::uint8_t index) const {
        return index == 0U || index > 32U ? PowerEndpoint{} : entries_[index - 1U];
    }

    [[nodiscard]] std::uint8_t index(const PowerOwner owner, const std::uint8_t instance) const {
        for (std::size_t i = 0U; i < entries_.size(); ++i)
            if (entries_[i].owner == owner && entries_[i].instance == instance)
                return static_cast<std::uint8_t>(i + 1U);
        return 0U;
    }

    [[nodiscard]] std::uint8_t count() const { return count_; }

  private:
    std::array<PowerEndpoint, 32> entries_{};
    std::uint8_t count_{};
};
} // namespace newmota
