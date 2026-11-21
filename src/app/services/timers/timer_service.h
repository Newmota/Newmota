/*
  timer_service.h - cooperative civil timer execution for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "services/timers/timer_settings.h"
#include <array>
#include <cstdint>

namespace newmota {
class Settings;
class TimeService;
class PowerService;

/** @brief A bounded Action 3 notification; scheduled is a local Unix minute. */
struct TimerEvent final {
    std::uint8_t timer{};
    std::uint8_t output{};
    std::uint8_t action{};
    std::uint32_t scheduled{};
};

/** @brief Executes observed local minutes without replaying missed civil time. */
class TimerService final {
  public:
    constexpr TimerService(Settings& settings, TimeService& time, PowerService& power)
        : settings_(settings), time_(time), power_(power) {}

    /** @brief Evaluates the current valid minute once, saving one-shots before dispatch. */
    void poll(std::uint32_t now);
    /** @brief Removes one queued Action 3 event on the safe Berry polling context. */
    [[nodiscard]] bool pop_event(TimerEvent& event);
    /** @brief Calculates a solar event's UTC minute for one local calendar date.
     * @return false for a polar date without the requested crossing.
     */
    [[nodiscard]] static bool solar_utc(
        std::uint32_t date, bool sunset, const TimerSettings& settings, std::uint32_t& seconds);

  private:
    /** @brief Chooses an inclusive unbiased noncryptographic random offset. */
    [[nodiscard]] std::int16_t random_window(std::uint8_t window);
    /** @brief Finds today's occurrence including solar offsets crossing date boundaries. */
    [[nodiscard]] bool due(
        const TimerSettings::Entry& timer, std::uint32_t local, std::int16_t window) const;
    Settings& settings_;
    TimeService& time_;
    PowerService& power_;
    std::array<std::int16_t, TimerSettings::kCount> windows_{};
    std::array<std::uint8_t, TimerSettings::kCount> window_sizes_{};
    std::array<std::uint32_t, TimerSettings::kCount> observed_through_{};
    std::array<std::uint32_t, TimerSettings::kCount> consumed_dates_{};
    std::array<TimerEvent, TimerSettings::kCount> events_{};
    std::uint32_t observed_minute_{};
    std::uint32_t window_day_{};
    std::uint32_t random_state_{};
    std::uint8_t event_head_{};
    std::uint8_t event_count_{};
    bool observed_{};
};
} // namespace newmota
