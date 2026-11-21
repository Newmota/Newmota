/*
  scheduler.h - cooperative scheduler interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

enum class ScheduledTick : std::uint8_t {
    none = 0,
    every_50_ms = 1U << 0,
    every_100_ms = 1U << 1,
    every_250_ms = 1U << 2,
    every_second = 1U << 3,
};

/**
 * @brief Combines two scheduler tick masks.
 * @return The union of both masks.
 */
[[nodiscard]] constexpr ScheduledTick operator|(
    const ScheduledTick left, const ScheduledTick right) {
    return static_cast<ScheduledTick>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

/**
 * @brief Tests whether a scheduler result contains a specific tick.
 * @return true when @p tick is present in @p ticks.
 */
[[nodiscard]] constexpr bool has_tick(const ScheduledTick ticks, const ScheduledTick tick) {
    return (static_cast<std::uint8_t>(ticks) & static_cast<std::uint8_t>(tick)) != 0;
}

class Scheduler final {
  public:
    /**
     * @brief Initializes all cadence deadlines relative to @p now.
     */
    explicit Scheduler(std::uint32_t now);

    /**
     * @brief Advances due deadlines and reports their cadence bits.
     * @return A mask containing every cadence due at @p now.
     */
    [[nodiscard]] ScheduledTick poll(std::uint32_t now);

  private:
    /**
     * @brief Advances one deadline past @p now when it is due.
     * @return true when the deadline was due and advanced.
     */
    static bool advance_if_due(std::uint32_t now, std::uint32_t period, std::uint32_t& deadline);

    std::uint32_t next_50_ms_;
    std::uint32_t next_100_ms_;
    std::uint32_t next_250_ms_;
    std::uint32_t next_second_;
};

} // namespace newmota
