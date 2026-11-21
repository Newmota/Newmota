/*
  timer_service.cpp - cooperative persisted civil timers for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file timer_service.cpp
 * @brief Dispatches persisted civil timers with explicit Power-operation origin.
 */
#include "services/timers/timer_service.h"
#include "newmota/float_math.h"
#include "newmota/log.h"
#include "services/settings/settings.h"
#include "services/time/time_service.h"
#include "services/power/power_service.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace newmota {
namespace {
constexpr double kRadians = 0.017453292519943295;

/** @brief Wraps an angle or hour into a positive cycle. */
double wrap(double value, const double period) {
    value = std::fmod(value, period);
    return value < 0 ? value + period : value;
}

/** @brief Reflects a random-window target into its scheduled civil day. */
std::int32_t windowed(const std::int32_t minute, const std::int16_t window) {
    auto result = minute + window;
    if (result < 0)
        result = -result;
    if (result > 1439)
        result = 2880 - result;
    return std::min(result, std::int32_t{1439});
}

/** @brief Adds signed seconds to an epoch, failing outside the 32-bit epoch range. */
bool offset_epoch(const std::uint32_t epoch, const std::int32_t delta, std::uint32_t& result) {
    const auto magnitude =
        delta < 0 ? 0U - static_cast<std::uint32_t>(delta) : static_cast<std::uint32_t>(delta);
    if (delta < 0 ? magnitude > epoch : magnitude > UINT32_MAX - epoch)
        return false;
    result = delta < 0 ? epoch - magnitude : epoch + magnitude;
    return true;
}
} // namespace

bool TimerService::solar_utc(const std::uint32_t date, const bool sunset,
    const TimerSettings& settings, std::uint32_t& seconds) {
    // Solar mean anomaly, true longitude, right ascension and hour angle.
    // The longitude-hour estimate selects the requested civil-date crossing;
    // the final UTC hour remains signed so remote timezones retain date changes.
    const auto civil = TimeService::civil_time(date);
    std::uint32_t year_start = 0;
    if (!TimeService::epoch_seconds(CivilTime{civil.year, 1, 1}, year_start))
        return false;
    const double day = (date - year_start) / 86400U + 1;
    const double longitude = settings.longitude / 1000000.0;
    const double latitude = settings.latitude / 1000000.0;
    const double estimate = day + ((sunset ? 18.0 : 6.0) - longitude / 15.0) / 24.0;
    const double anomaly = 0.9856 * estimate - 3.289;
    const double longitude_sun = wrap(anomaly + 1.916 * math::sin(anomaly * kRadians) +
                                          0.020 * math::sin(2 * anomaly * kRadians) + 282.634,
        360.0);
    double ascension =
        wrap(math::atan(0.91764 * math::tan(longitude_sun * kRadians)) / kRadians, 360.0);
    ascension += std::floor(longitude_sun / 90.0) * 90.0 - std::floor(ascension / 90.0) * 90.0;
    ascension /= 15.0;
    const double sin_declination = 0.39782 * math::sin(longitude_sun * kRadians);
    const double cos_declination = math::cos(math::asin(sin_declination));
    constexpr double zenith[] = {90.8333, 96.0, 102.0, 108.0};
    const double denominator = cos_declination * math::cos(latitude * kRadians);
    if (std::abs(denominator) < 1e-12)
        return false;
    const double cosine = (math::cos(zenith[settings.sunrise] * kRadians) -
                              sin_declination * math::sin(latitude * kRadians)) /
                          denominator;
    if (cosine < -1 || cosine > 1)
        return false;
    const double hour =
        (sunset ? math::acos(cosine) / kRadians : 360.0 - math::acos(cosine) / kRadians) / 15.0;
    const double mean_local = wrap(hour + ascension - 0.06571 * estimate - 6.622, 24.0);
    // Valid longitudes keep this within -720..2160 minutes; the guard also rejects NaN.
    const double minutes = std::floor((mean_local - longitude / 15.0) * 60.0 + 0.5);
    if (!(minutes > -10000.0 && minutes < 10000.0))
        return false;
    return offset_epoch(date / 86400U * 86400U, static_cast<std::int32_t>(minutes) * 60, seconds);
}

std::int16_t TimerService::random_window(const std::uint8_t window) {
    if (window == 0)
        return 0;
    const std::uint32_t range = window * 2U + 1U;
    // xorshift32 has a period of 2^32-1; reject its incomplete final bucket.
    const auto limit = std::numeric_limits<std::uint32_t>::max() -
                       std::numeric_limits<std::uint32_t>::max() % range;
    std::uint32_t value = 0;
    do {
        random_state_ ^= random_state_ << 13;
        random_state_ ^= random_state_ >> 17;
        random_state_ ^= random_state_ << 5;
        value = random_state_ - 1U;
    } while (value >= limit);
    return static_cast<std::int16_t>(value % range) - window;
}

bool TimerService::due(
    const TimerSettings::Entry& timer, const std::uint32_t local, const std::int16_t window) const {
    const auto date = local / 86400U * 86400U;
    const auto minute = static_cast<std::int32_t>(local % 86400U / 60U);
    if (timer.mode == 0) {
        const auto day = TimeService::civil_time(date).day_of_week - 1U;
        return (timer.days & (1U << day)) != 0 && minute == windowed(timer.time, window);
    }
    // Days refer to the solar event's calendar date, including offset crossings.
    for (int shift = -2; shift <= 2; ++shift) {
        std::uint32_t source_date = 0;
        if (!offset_epoch(date, shift * 86400, source_date))
            continue;
        const auto day = TimeService::civil_time(source_date).day_of_week - 1U;
        if ((timer.days & (1U << day)) == 0)
            continue;
        std::uint32_t utc = 0;
        if (!solar_utc(source_date, timer.mode == 2, settings_.timer_settings(), utc))
            continue;
        const auto offset =
            timer.time <= 719 ? static_cast<int>(timer.time) : -static_cast<int>(timer.time - 720);
        // Offset is civil minutes, like Tasmota, after projecting the solar event.
        std::uint32_t target = 0;
        if (!offset_epoch(time_.to_local(utc), offset * 60, target) ||
            target / 86400U != date / 86400U)
            continue;
        if (minute == windowed(static_cast<std::int32_t>(target % 86400U / 60U), window))
            return true;
    }
    return false;
}

void TimerService::poll(const std::uint32_t now) {
    const auto snapshot = time_.snapshot(now);
    if (!snapshot.valid)
        return;
    const auto minute = snapshot.local_seconds / 60U;
    if (observed_ && minute == observed_minute_)
        return;
    const bool first = !observed_;
    observed_ = true;
    observed_minute_ = minute;
    if (random_state_ == 0) {
        random_state_ = snapshot.utc_seconds ^ now ^ 0xa341316cU;
        if (random_state_ == 0)
            random_state_ = 1;
    }
    const auto date = snapshot.local_seconds / 86400U;
    const auto& policy = settings_.timer_settings();
    for (std::uint8_t index = 0; index < TimerSettings::kCount; ++index) {
        const auto& timer = policy.entries[index];
        if (first || date != window_day_ || window_sizes_[index] != timer.window) {
            windows_[index] = random_window(timer.window);
            window_sizes_[index] = timer.window;
        }
    }
    window_day_ = date;
    const bool timers_enabled = policy.enabled;
    for (std::uint8_t index = 0; index < TimerSettings::kCount; ++index) {
        const bool previously_observed = !first && minute <= observed_through_[index];
        observed_through_[index] = std::max(observed_through_[index], minute);
        // Copy: saving a one-shot replaces the settings object backing policy.
        const auto timer = settings_.timer_settings().entries[index];
        if (!timers_enabled || !timer.enabled || consumed_dates_[index] == date ||
            previously_observed || !due(timer, snapshot.local_seconds, windows_[index]))
            continue;
        consumed_dates_[index] = date;
        if (!timer.repeat) {
            auto candidate = settings_.timer_settings();
            candidate.entries[index].enabled = false;
            if (settings_.set_timer_settings(candidate) == SettingsUpdateResult::failed) {
                AddLog(LogLevel::error, "TIM: Timer%u one-shot save failed; action suppressed",
                    index + 1U);
                continue;
            }
        }
        if (timer.action < 3) {
            (void)power_.set(
                timer.output, static_cast<PowerAction>(timer.action), PowerOperationSource::timer);
        } else if (event_count_ < events_.size()) {
            events_[(event_head_ + event_count_) % events_.size()] = TimerEvent{
                static_cast<std::uint8_t>(index + 1), timer.output, timer.action, minute * 60U};
            ++event_count_;
        } else {
            AddLog(LogLevel::warning, "TIM: Timer%u event queue full; event dropped", index + 1U);
        }
    }
}

bool TimerService::pop_event(TimerEvent& event) {
    if (event_count_ == 0)
        return false;
    event = events_[event_head_];
    event_head_ = static_cast<std::uint8_t>((event_head_ + 1U) % events_.size());
    --event_count_;
    return true;
}
} // namespace newmota
