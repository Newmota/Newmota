/*
  pwm_policy.cpp - shared PWM request and arithmetic policy

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/pwm_detail.h"

#include <limits>

namespace newmota::platform::detail {

/** @brief Compares two nonnegative fractions without cross multiplication. */
[[nodiscard]] int compare_fractions(std::uint64_t left_numerator, std::uint64_t left_denominator,
    std::uint64_t right_numerator, std::uint64_t right_denominator) {
    bool reversed = false;
    for (;;) {
        const auto left_whole = left_numerator / left_denominator;
        const auto right_whole = right_numerator / right_denominator;
        if (left_whole != right_whole) {
            const auto order = left_whole < right_whole ? -1 : 1;
            return reversed ? -order : order;
        }

        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            const auto order =
                left_remainder == right_remainder ? 0 : (left_remainder == 0U ? -1 : 1);
            return reversed ? -order : order;
        }

        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        reversed = !reversed;
    }
}

/** @brief Checks one hard request before platform routing is considered. */
PwmResult validate_pwm_request(const PwmRequest& request) {
    if (!request.pin.valid() || request.frequency_hz == 0U ||
        request.max_error_ppm > kMaximumPwmErrorPpm || request.min_resolution_bits == 0U ||
        request.min_resolution_bits > 16U) {
        return PwmResult::invalid_request;
    }
    return PwmResult::success;
}

/** @brief Validates a whole request set and marks every duplicate claimant. */
PwmResult validate_pwm_request_set(
    const PwmRequest* const requests, const std::size_t count, PwmRequestResult* const results) {
    if (count > kMaximumPwmOutputs) {
        return PwmResult::too_many_outputs;
    }
    if (count == 0U || requests == nullptr || results == nullptr) {
        return PwmResult::invalid_request;
    }

    bool valid = true;
    bool eligible[kMaximumPwmOutputs]{};
    bool duplicate_output[kMaximumPwmOutputs]{};
    bool duplicate_pin[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {};
        results[index].result = validate_pwm_request(requests[index]);
        eligible[index] = results[index].result == PwmResult::success;
        if (!eligible[index]) {
            valid = false;
        }
    }

    for (std::size_t first = 0U; first < count; ++first) {
        for (std::size_t second = first + 1U; second < count; ++second) {
            if (!eligible[first] || !eligible[second]) {
                continue;
            }
            if (requests[first].logical_output.owner == requests[second].logical_output.owner &&
                requests[first].logical_output.instance ==
                    requests[second].logical_output.instance) {
                duplicate_output[first] = true;
                duplicate_output[second] = true;
                valid = false;
            }
            if (requests[first].pin.value() == requests[second].pin.value()) {
                duplicate_pin[first] = true;
                duplicate_pin[second] = true;
                valid = false;
            }
        }
    }

    // Stable precedence: identity conflicts are reported before pad conflicts.
    for (std::size_t index = 0U; index < count; ++index) {
        if (!eligible[index]) {
            continue;
        }
        if (duplicate_output[index]) {
            results[index].result = PwmResult::duplicate_output;
        } else if (duplicate_pin[index]) {
            results[index].result = PwmResult::duplicate_pin;
        }
    }
    return valid ? PwmResult::success : PwmResult::invalid_request;
}

/** @brief Tests the exact relative-error inequality without wide extensions. */
bool pwm_frequency_meets_request(const PwmFrequency& effective, const PwmRequest& request) {
    if (validate_pwm_request(request) != PwmResult::success || effective.numerator_hz == 0U ||
        effective.denominator == 0U) {
        return false;
    }

    constexpr std::uint64_t kPartsPerMillion = 1'000'000U;
    const auto quotient = effective.numerator_hz / effective.denominator;
    const auto remainder = effective.numerator_hz % effective.denominator;

    std::uint64_t distance_whole = 0U;
    std::uint64_t distance_numerator = 0U;
    std::uint64_t distance_denominator = effective.denominator;
    if (quotient > request.frequency_hz) {
        distance_whole = quotient - request.frequency_hz;
        distance_numerator = remainder;
    } else if (quotient < request.frequency_hz) {
        const auto whole_gap = static_cast<std::uint64_t>(request.frequency_hz) - quotient;
        if (remainder == 0U) {
            distance_whole = whole_gap;
        } else {
            distance_whole = whole_gap - 1U;
            distance_numerator = effective.denominator - remainder;
        }
    } else {
        distance_numerator = remainder;
    }

    const auto tolerance_scaled =
        static_cast<std::uint64_t>(request.frequency_hz) * request.max_error_ppm;
    const auto tolerance_whole = tolerance_scaled / kPartsPerMillion;
    const auto tolerance_numerator = tolerance_scaled % kPartsPerMillion;
    if (distance_whole != tolerance_whole) {
        return distance_whole < tolerance_whole;
    }
    return compare_fractions(distance_numerator, distance_denominator, tolerance_numerator,
               kPartsPerMillion) <= 0;
}

/** @brief Computes resolution from maximum normalized adjacent step size. */
std::uint8_t pwm_resolution_bits(const std::uint32_t intervals) {
    std::uint8_t bits = 0U;
    for (std::uint8_t candidate = 1U; candidate <= 16U; ++candidate) {
        const auto required = (std::uint32_t{1U} << candidate) - 1U;
        if (intervals < required) {
            break;
        }
        bits = candidate;
    }
    return bits;
}

/** @brief Returns the interval floor required by a requested resolution. */
std::uint32_t pwm_minimum_intervals(const std::uint8_t resolution_bits) {
    if (resolution_bits == 0U) {
        return 1U;
    }
    if (resolution_bits > 16U) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return (std::uint32_t{1U} << resolution_bits) - 1U;
}

/** @brief Rounds the normalized 16-bit value onto a native duty interval. */
std::uint32_t pwm_duty_interval(const std::uint16_t duty, const std::uint32_t intervals) {
    if (duty == 0U || intervals == 0U) {
        return 0U;
    }
    if (duty == std::numeric_limits<std::uint16_t>::max()) {
        return intervals;
    }
    constexpr std::uint64_t kDutyMaximum = std::numeric_limits<std::uint16_t>::max();
    const auto scaled = static_cast<std::uint64_t>(duty) * intervals;
    const auto whole = scaled / kDutyMaximum;
    const auto remainder = scaled % kDutyMaximum;
    return static_cast<std::uint32_t>(whole + (remainder * 2U >= kDutyMaximum ? 1U : 0U));
}

} // namespace newmota::platform::detail
