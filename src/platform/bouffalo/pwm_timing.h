/*
  pwm_timing.h - Bouffalo PWM divider and period planning

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file pwm_timing.h
 * @brief Timing search shared by the BL602 PWM-v1 and BL616 PWM-v2 adapters.
 *
 * Both peripherals divide one source clock by a 16-bit divider and count a
 * 16-bit period.
 */

#pragma once

#include "newmota/platform/pwm_detail.h"

#include <cstdint>

namespace newmota::platform::detail::bouffalo_pwm {

constexpr std::uint32_t kMaximumCounter = 65'535U;

/** @brief One divider/period pair and its exact carrier error. */
struct Timing {
    bool valid{};
    std::uint16_t divider{};
    std::uint16_t period{};
    std::uint8_t resolution{};
    PwmFrequency frequency{};
    std::uint64_t error_numerator{};
    std::uint64_t error_denominator{1U};
};

inline void consider_period(const PwmRequest& request, const std::uint32_t source_hz,
    const std::uint32_t divider, const std::uint32_t period, Timing& selected) {
    if (period < 2U || period > kMaximumCounter) {
        return;
    }
    const auto denominator = static_cast<std::uint64_t>(divider) * period;
    const PwmFrequency frequency{source_hz, denominator};
    if (!pwm_frequency_meets_request(frequency, request)) {
        return;
    }
    const auto target = static_cast<std::uint64_t>(request.frequency_hz) * denominator;
    const auto error = source_hz > target ? source_hz - target : target - source_hz;
    const auto resolution = pwm_resolution_bits(period);
    if (resolution < request.min_resolution_bits) {
        return;
    }
    const auto error_order =
        compare_fractions(error, target, selected.error_numerator, selected.error_denominator);
    const bool better =
        !selected.valid || error_order < 0 ||
        (error_order == 0 && (resolution > selected.resolution ||
                                 (resolution == selected.resolution && period > selected.period)));
    if (better) {
        selected = {true, static_cast<std::uint16_t>(divider), static_cast<std::uint16_t>(period),
            resolution, frequency, error, target};
    }
}

/** @brief Finds the most accurate accepted 16-bit divider/period pair. */
[[nodiscard]] inline bool plan_timing(
    const PwmRequest& request, const std::uint32_t source_hz, Timing& selected) {
    if (source_hz == 0U) {
        return false;
    }
    selected = {};
    const auto resolution_floor = pwm_minimum_intervals(request.min_resolution_bits);
    const auto minimum_period = resolution_floor < 2U ? 2U : resolution_floor;
    for (std::uint32_t divider = 1U; divider <= kMaximumCounter; ++divider) {
        const auto divisor = static_cast<std::uint64_t>(request.frequency_hz) * divider;
        const auto quotient = static_cast<std::uint64_t>(source_hz) / divisor;
        if (quotient < minimum_period) {
            consider_period(request, source_hz, divider, minimum_period, selected);
            continue;
        }
        if (quotient > kMaximumCounter) {
            consider_period(request, source_hz, divider, kMaximumCounter, selected);
            continue;
        }
        consider_period(
            request, source_hz, divider, static_cast<std::uint32_t>(quotient), selected);
        if (quotient < kMaximumCounter) {
            consider_period(
                request, source_hz, divider, static_cast<std::uint32_t>(quotient + 1U), selected);
        }
    }
    return selected.valid;
}

} // namespace newmota::platform::detail::bouffalo_pwm
