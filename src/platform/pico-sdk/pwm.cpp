/*
  pwm.cpp - RP2350 hardware PWM adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/pwm_detail.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/regs/pwm.h"
#include "hardware/structs/pwm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

namespace newmota::platform::detail {
namespace {

constexpr std::size_t kSliceCount = NUM_PWM_SLICES;
constexpr std::uint64_t kPartsPerMillion = 1'000'000U;

struct SliceState {
    bool initialized{};
    bool quarantined{};
    std::uint16_t divider_q4{};
    std::uint16_t period{};
    std::uint8_t active_mask{};
    bool inverted[2]{};
    std::uint16_t pins[2]{};
    std::uint16_t duties[2]{};
    PwmHandle handles[2]{};
};

struct PlanEntry {
    bool valid{};
    PwmRequest request{};
    PwmHandle handle{};
    PwmEffectiveSettings effective{};
    std::uint16_t old_duty{};
    std::uint16_t divider_q4{};
    std::uint16_t period{};
    std::uint8_t slice{};
    std::uint8_t channel{};
    bool reconfiguration{};
    bool retune{};
};

struct NativePlan {
    bool valid{};
    bool reconfiguration{};
    PwmPlanToken token{};
    std::uint32_t clock_hz{};
    std::size_t count{};
    PlanEntry entries[kMaximumPwmOutputs]{};
};

struct NativeState {
    SliceState slices[kSliceCount]{};
    /** @brief Exists from prepare until activation or discard. */
    std::unique_ptr<NativePlan> plan;
};

/** @brief Compares tagged logical output identities. */
[[nodiscard]] bool same_output_id(const PwmOutputId& left, const PwmOutputId& right) {
    return left.owner == right.owner && left.instance == right.instance;
}

/** @brief Reserves zero as the invalid shared-slice failure domain. */
[[nodiscard]] std::uint16_t slice_failure_domain(const std::uint8_t slice) {
    return static_cast<std::uint16_t>(static_cast<unsigned>(slice) + 1U);
}

/** @brief Returns this GPIO's channel-specific compare value. */
[[nodiscard]] std::uint16_t compare_level(const std::uint8_t slice, const std::uint8_t channel) {
    const auto cc = pwm_hw->slice[slice].cc;
    const auto shift = channel == 0U ? PWM_CH0_CC_A_LSB : PWM_CH0_CC_B_LSB;
    return static_cast<std::uint16_t>((cc >> shift) & 0xffffU);
}

/** @brief Returns the exact clock fraction for one divider and period. */
[[nodiscard]] PwmFrequency effective_frequency(
    const std::uint32_t clock_hz, const std::uint16_t divider_q4, const std::uint16_t period) {
    return {static_cast<std::uint64_t>(clock_hz) * 16U,
        static_cast<std::uint64_t>(divider_q4) * period};
}

/** @brief Returns one request's relative frequency error as an exact fraction. */
void frequency_error(const PwmRequest& request, const PwmFrequency frequency,
    std::uint64_t& numerator, std::uint64_t& denominator) {
    const auto target = static_cast<std::uint64_t>(request.frequency_hz) * frequency.denominator;
    numerator = frequency.numerator_hz > target ? frequency.numerator_hz - target
                                                : target - frequency.numerator_hz;
    denominator = target;
}

/** @brief Scores one timing config by its worst request's exact relative error. */
void worst_group_error(const PwmRequest* requests, const std::size_t count,
    const PwmFrequency frequency, std::uint64_t& worst_numerator,
    std::uint64_t& worst_denominator) {
    worst_numerator = 0U;
    worst_denominator = 1U;
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint64_t numerator = 0U;
        std::uint64_t denominator = 0U;
        frequency_error(requests[index], frequency, numerator, denominator);
        if (compare_fractions(numerator, denominator, worst_numerator, worst_denominator) > 0) {
            worst_numerator = numerator;
            worst_denominator = denominator;
        }
    }
}

/** @brief Finds the most accurate accepted timing shared by a slice. */
[[nodiscard]] bool find_group_timing(const PwmRequest* requests, const std::size_t count,
    const std::uint32_t clock_hz, std::uint16_t& best_divider_q4, std::uint16_t& best_period,
    PwmEffectiveSettings& best_effective) {
    if (count == 0U || clock_hz == 0U) {
        return false;
    }
    const auto clock_numerator = static_cast<std::uint64_t>(clock_hz) * 16U;
    const auto scaled_clock = clock_numerator * kPartsPerMillion;
    bool found = false;
    std::uint8_t best_bits = 0U;
    std::uint64_t best_error_numerator = 0U;
    std::uint64_t best_error_denominator = 1U;
    std::uint32_t minimum_target = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_target = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        minimum_target = std::min(minimum_target, requests[index].frequency_hz);
        maximum_target = std::max(maximum_target, requests[index].frequency_hz);
    }

    for (std::uint16_t divider_q4 = 16U; divider_q4 <= 4095U; ++divider_q4) {
        std::uint64_t minimum_period = 2U;
        std::uint64_t maximum_period = 65'535U;
        bool period_possible = true;
        for (std::size_t index = 0U; index < count; ++index) {
            const auto target = requests[index].frequency_hz;
            const auto resolution_floor = static_cast<std::uint64_t>(
                pwm_minimum_intervals(requests[index].min_resolution_bits));
            if (resolution_floor > minimum_period) {
                minimum_period = resolution_floor;
            }
            const auto high_factor = kPartsPerMillion + requests[index].max_error_ppm;
            const auto low_factor = kPartsPerMillion - requests[index].max_error_ppm;
            const auto high_multiplier = static_cast<std::uint64_t>(divider_q4) * high_factor;
            const auto high_limit = std::numeric_limits<std::uint64_t>::max() / high_multiplier;
            const auto high_denominator =
                target > high_limit ? std::numeric_limits<std::uint64_t>::max()
                                    : static_cast<std::uint64_t>(target) * high_multiplier;
            const auto low_denominator =
                static_cast<std::uint64_t>(target) * divider_q4 * low_factor;
            if (high_denominator == 0U || low_denominator == 0U) {
                period_possible = false;
                break;
            }
            const auto lower_numerator = scaled_clock;
            const auto lower_bound =
                lower_numerator / high_denominator + (lower_numerator % high_denominator != 0U);
            const auto upper_bound = scaled_clock / low_denominator;
            minimum_period = std::max(minimum_period, lower_bound);
            maximum_period = std::min(maximum_period, upper_bound);
            if (minimum_period > maximum_period || minimum_period > 65'535U) {
                period_possible = false;
                break;
            }
        }
        if (!period_possible) {
            continue;
        }

        const auto minimum_period_for_lowest =
            clock_numerator / (static_cast<std::uint64_t>(divider_q4) * minimum_target);
        const auto minimum_period_for_highest =
            clock_numerator / (static_cast<std::uint64_t>(divider_q4) * maximum_target);
        const auto center = (minimum_period_for_lowest + minimum_period_for_highest) / 2U;
        const auto consider = [&](const std::uint64_t candidate) {
            if (candidate < minimum_period || candidate > maximum_period || candidate > 65'535U) {
                return;
            }
            const auto period = static_cast<std::uint16_t>(candidate);
            const auto frequency = effective_frequency(clock_hz, divider_q4, period);
            const auto bits = pwm_resolution_bits(period);
            for (std::size_t index = 0U; index < count; ++index) {
                if (!pwm_frequency_meets_request(frequency, requests[index]) ||
                    bits < requests[index].min_resolution_bits) {
                    return;
                }
            }
            std::uint64_t error_numerator = 0U;
            std::uint64_t error_denominator = 1U;
            worst_group_error(requests, count, frequency, error_numerator, error_denominator);
            const auto error_order = compare_fractions(
                error_numerator, error_denominator, best_error_numerator, best_error_denominator);
            const bool better =
                !found || error_order < 0 ||
                (error_order == 0 &&
                    (bits > best_bits ||
                        (bits == best_bits &&
                            (period > best_period ||
                                (period == best_period && divider_q4 < best_divider_q4)))));
            if (better) {
                found = true;
                best_bits = bits;
                best_error_numerator = error_numerator;
                best_error_denominator = error_denominator;
                best_divider_q4 = divider_q4;
                best_period = period;
                best_effective = {frequency, bits};
            }
        };

        consider(minimum_period);
        consider(maximum_period);
        for (std::int8_t adjustment = -1; adjustment <= 2; ++adjustment) {
            if (adjustment < 0 && center < static_cast<std::uint64_t>(-adjustment)) {
                continue;
            }
            consider(static_cast<std::uint64_t>(static_cast<std::int64_t>(center) + adjustment));
        }
    }
    return found;
}

/** @brief Tests the active-low electrical input latch and SIO route. */
[[nodiscard]] bool prepare_safe_gpio(const std::uint16_t pin, const bool inverted) {
    const auto gpio = static_cast<uint>(pin);
    gpio_set_function(gpio, GPIO_FUNC_SIO);
    gpio_put(gpio, inverted);
    gpio_set_dir(gpio, GPIO_OUT);
    return gpio_get_function(gpio) == GPIO_FUNC_SIO && gpio_get_dir(gpio) != 0U &&
           gpio_get(gpio) == inverted;
}

/** @brief Retains enough native identity to release any uncertain activation. */
void retain_plan_claims(NativeState& state, const std::size_t* entries, const std::size_t count) {
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[entries[index]];
        auto& slice = state.slices[entry.slice];
        slice.active_mask |= static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
        slice.inverted[entry.channel] = entry.request.inverted;
        slice.pins[entry.channel] = entry.request.pin.value();
        slice.duties[entry.channel] =
            entry.reconfiguration ? entry.old_duty : entry.request.initial_duty;
        slice.handles[entry.channel] = entry.handle;
    }
}

/** @brief Drops provisional claims after cleanup has been positively checked. */
void clear_plan_claims(NativeState& state, const std::size_t* entries, const std::size_t count) {
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[entries[index]];
        auto& slice = state.slices[entry.slice];
        const auto bit = static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
        if (slice.handles[entry.channel].slot == entry.handle.slot &&
            slice.handles[entry.channel].generation == entry.handle.generation) {
            slice.active_mask &= static_cast<std::uint8_t>(~bit);
            slice.handles[entry.channel] = {};
            slice.pins[entry.channel] = 0U;
            slice.duties[entry.channel] = 0U;
            slice.inverted[entry.channel] = false;
        }
    }
}

/** @brief Routes a pad to PWM and verifies the function-select readback. */
[[nodiscard]] bool route_to_pwm(const std::uint16_t pin) {
    const auto gpio = static_cast<uint>(pin);
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    return gpio_get_function(gpio) == GPIO_FUNC_PWM;
}

/** @brief Verifies divider, wrap, counter mode, and phase-correct settings. */
[[nodiscard]] bool timing_matches(
    const std::uint8_t slice, const std::uint16_t divider_q4, const std::uint16_t period) {
    const auto& registers = pwm_hw->slice[slice];
    const auto expected_divider =
        (static_cast<std::uint32_t>(divider_q4 / 16U) << PWM_CH0_DIV_INT_LSB) |
        (static_cast<std::uint32_t>(divider_q4 % 16U) << PWM_CH0_DIV_FRAC_LSB);
    const auto csr = registers.csr;
    return registers.div == expected_divider &&
           registers.top == static_cast<std::uint32_t>(period - 1U) &&
           (csr & PWM_CH0_CSR_DIVMODE_BITS) == 0U && (csr & PWM_CH0_CSR_PH_CORRECT_BITS) == 0U;
}

/** @brief Writes and verifies both channel polarities without touching timing. */
[[nodiscard]] bool set_polarities(
    const std::uint8_t slice, const bool inverted_a, const bool inverted_b) {
    pwm_set_output_polarity(slice, inverted_a, inverted_b);
    const auto csr = pwm_hw->slice[slice].csr;
    return ((csr & PWM_CH0_CSR_A_INV_BITS) != 0U) == inverted_a &&
           ((csr & PWM_CH0_CSR_B_INV_BITS) != 0U) == inverted_b;
}

/** @brief Converts one normalized duty to RP2350 compare counts. */
[[nodiscard]] std::uint16_t compare_for(const std::uint16_t duty, const std::uint16_t period) {
    return static_cast<std::uint16_t>(pwm_duty_interval(duty, period));
}

/** @brief Initializes a previously unowned slice and verifies every register. */
[[nodiscard]] bool configure_fresh_slice(const std::uint8_t slice, const std::uint16_t divider_q4,
    const std::uint16_t period, const bool inverted_a, const bool inverted_b,
    const std::uint16_t level_a, const std::uint16_t level_b) {
    pwm_config configuration = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac4(
        &configuration, divider_q4 / 16U, static_cast<std::uint8_t>(divider_q4 % 16U));
    pwm_config_set_wrap(&configuration, static_cast<std::uint16_t>(period - 1U));
    pwm_config_set_phase_correct(&configuration, false);
    pwm_init(slice, &configuration, false);
    if (!timing_matches(slice, divider_q4, period) ||
        (pwm_hw->en & (std::uint32_t{1U} << slice)) != 0U) {
        return false;
    }
    if (!set_polarities(slice, inverted_a, inverted_b)) {
        return false;
    }
    pwm_set_both_levels(slice, level_a, level_b);
    const auto expected_cc =
        (static_cast<std::uint32_t>(level_b) << 16U) | static_cast<std::uint32_t>(level_a);
    return pwm_hw->slice[slice].cc == expected_cc;
}

/** @brief Confirms one group's member list matches a complete active slice. */
[[nodiscard]] bool complete_slice_participation(const SliceState& state, const PwmRequest* requests,
    const PwmHandle* handles, const std::uint8_t* channels, const std::uint8_t* slices,
    const std::size_t count, const std::uint8_t slice) {
    for (std::uint8_t channel = 0U; channel < 2U; ++channel) {
        if ((state.active_mask & (std::uint8_t{1U} << channel)) == 0U) {
            continue;
        }
        bool present = false;
        for (std::size_t index = 0U; index < count; ++index) {
            if (slices[index] == slice && channels[index] == channel &&
                handles[index].slot == state.handles[channel].slot &&
                handles[index].generation == state.handles[channel].generation &&
                requests[index].pin.value() == state.pins[channel]) {
                present = true;
                break;
            }
        }
        if (!present) {
            return false;
        }
    }
    return true;
}

/** @brief Plans every candidate route and its timing group without writes. */
PwmResult prepare_native(void* const opaque, const PwmPlanToken token, const bool reconfiguration,
    const PwmBackendActiveOutput* const active, const std::size_t active_count,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendPlanResult* const results) {
    auto& state = *static_cast<NativeState*>(opaque);
    state.plan.reset();
    state.plan.reset(new (std::nothrow) NativePlan{});
    if (!state.plan) {
        return PwmResult::no_memory;
    }
    state.plan->token = token;
    state.plan->reconfiguration = reconfiguration;
    state.plan->clock_hz = clock_get_hz(clk_sys);
    if (count > kMaximumPwmOutputs || active_count > kMaximumPwmOutputs || requests == nullptr ||
        handles == nullptr || results == nullptr || state.plan->clock_hz == 0U) {
        return PwmResult::native_failure;
    }

    std::uint8_t slices[kMaximumPwmOutputs]{};
    std::uint8_t channels[kMaximumPwmOutputs]{};
    bool route_valid[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {};
        const auto pin = static_cast<uint>(requests[index].pin.value());
        if (pin >= NUM_BANK0_GPIOS) {
            results[index].result = PwmResult::unsupported_route;
            continue;
        }
        const auto slice = pwm_gpio_to_slice_num(pin);
        const auto channel = pwm_gpio_to_channel(pin);
        if (slice >= kSliceCount || channel > 1U) {
            results[index].result = PwmResult::unsupported_route;
            continue;
        }
        slices[index] = static_cast<std::uint8_t>(slice);
        channels[index] = static_cast<std::uint8_t>(channel);
        route_valid[index] = true;
        if (state.slices[slice].quarantined) {
            results[index].result = PwmResult::quarantined;
            route_valid[index] = false;
            continue;
        }
        const bool is_existing_target =
            reconfiguration && state.slices[slice].active_mask != 0U &&
            state.slices[slice].handles[channel].slot == handles[index].slot &&
            state.slices[slice].handles[channel].generation == handles[index].generation;
        if ((state.slices[slice].active_mask & (std::uint8_t{1U} << channel)) != 0U &&
            !is_existing_target) {
            results[index].result = PwmResult::ownership_conflict;
            route_valid[index] = false;
            continue;
        }
        if (state.slices[slice].active_mask == 0U &&
            (pwm_hw->en & (std::uint32_t{1U} << slice)) != 0U) {
            results[index].result = PwmResult::resource_unavailable;
            route_valid[index] = false;
            continue;
        }
    }

    const auto add_entry = [&](const std::size_t index, const std::uint16_t divider_q4,
                               const std::uint16_t period, const bool retune) {
        auto& entry = state.plan->entries[index];
        entry.valid = true;
        entry.request = requests[index];
        entry.handle = handles[index];
        entry.slice = slices[index];
        entry.channel = channels[index];
        entry.divider_q4 = divider_q4;
        entry.period = period;
        entry.reconfiguration = reconfiguration;
        entry.retune = retune;
        entry.effective = {effective_frequency(state.plan->clock_hz, divider_q4, period),
            pwm_resolution_bits(period)};
        if (reconfiguration) {
            for (std::size_t active_index = 0U; active_index < active_count; ++active_index) {
                if (active[active_index].handle.slot == handles[index].slot &&
                    active[active_index].handle.generation == handles[index].generation) {
                    entry.old_duty = active[active_index].applied_duty;
                    break;
                }
            }
        } else {
            entry.old_duty = requests[index].initial_duty;
        }
        results[index].result = PwmResult::success;
        results[index].effective = entry.effective;
        results[index].failure_domain = slice_failure_domain(entry.slice);
    };

    bool processed[kSliceCount]{};
    for (std::size_t index = 0U; index < count; ++index) {
        if (!route_valid[index] || results[index].result != PwmResult::invalid_request) {
            continue;
        }
        const auto slice = slices[index];
        if (processed[slice]) {
            continue;
        }
        processed[slice] = true;
        auto& slice_state = state.slices[slice];
        std::size_t group_indices[kMaximumPwmOutputs]{};
        std::size_t group_count = 0U;
        for (std::size_t candidate = index; candidate < count; ++candidate) {
            if (route_valid[candidate] && slices[candidate] == slice &&
                results[candidate].result == PwmResult::invalid_request) {
                group_indices[group_count] = candidate;
                ++group_count;
            }
        }
        if (group_count == 0U) {
            continue;
        }

        if (slice_state.active_mask != 0U && !reconfiguration) {
            const auto current_frequency = effective_frequency(
                state.plan->clock_hz, slice_state.divider_q4, slice_state.period);
            bool admitted_channels[2]{};
            for (std::size_t member = 0U; member < group_count; ++member) {
                const auto input = group_indices[member];
                const auto channel = channels[input];
                const auto channel_mask = static_cast<std::uint8_t>(std::uint8_t{1U} << channel);
                if ((slice_state.active_mask & channel_mask) != 0U || admitted_channels[channel]) {
                    results[input].result = PwmResult::ownership_conflict;
                    route_valid[input] = false;
                    continue;
                }
                if (!pwm_frequency_meets_request(current_frequency, requests[input]) ||
                    pwm_resolution_bits(slice_state.period) < requests[input].min_resolution_bits) {
                    results[input].result = PwmResult::incompatible;
                    route_valid[input] = false;
                    continue;
                }
                admitted_channels[channel] = true;
                add_entry(input, slice_state.divider_q4, slice_state.period, false);
            }
            continue;
        }

        bool retune = false;
        std::uint16_t divider_q4 = slice_state.divider_q4;
        std::uint16_t period = slice_state.period;
        PwmRequest group_requests[kMaximumPwmOutputs]{};
        for (std::size_t member = 0U; member < group_count; ++member) {
            group_requests[member] = requests[group_indices[member]];
        }

        if (slice_state.active_mask != 0U) {
            bool current_config_meets = true;
            const auto current_frequency = effective_frequency(
                state.plan->clock_hz, slice_state.divider_q4, slice_state.period);
            for (std::size_t member = 0U; member < group_count; ++member) {
                const auto input = group_indices[member];
                if (!reconfiguration &&
                    (slice_state.active_mask & (std::uint8_t{1U} << channels[input])) != 0U) {
                    results[input].result = PwmResult::ownership_conflict;
                    route_valid[input] = false;
                    current_config_meets = false;
                    continue;
                }
                if (!pwm_frequency_meets_request(current_frequency, requests[input]) ||
                    pwm_resolution_bits(slice_state.period) < requests[input].min_resolution_bits) {
                    current_config_meets = false;
                }
            }
            if (reconfiguration && current_config_meets) {
                retune = false;
            } else if (reconfiguration && complete_slice_participation(slice_state, requests,
                                              handles, channels, slices, count, slice)) {
                PwmEffectiveSettings selected{};
                if (!find_group_timing(group_requests, group_count, state.plan->clock_hz,
                        divider_q4, period, selected)) {
                    for (std::size_t member = 0U; member < group_count; ++member) {
                        results[group_indices[member]].result = PwmResult::requirements_unsatisfied;
                        route_valid[group_indices[member]] = false;
                    }
                    continue;
                }
                retune = divider_q4 != slice_state.divider_q4 || period != slice_state.period;
            } else if (reconfiguration) {
                for (std::size_t member = 0U; member < group_count; ++member) {
                    const auto input = group_indices[member];
                    if (!pwm_frequency_meets_request(current_frequency, requests[input]) ||
                        pwm_resolution_bits(slice_state.period) <
                            requests[input].min_resolution_bits) {
                        results[input].result = PwmResult::incompatible;
                        route_valid[input] = false;
                    }
                }
                current_config_meets = true;
                for (std::size_t member = 0U; member < group_count; ++member) {
                    current_config_meets =
                        current_config_meets && route_valid[group_indices[member]];
                }
                if (!current_config_meets) {
                    continue;
                }
            } else {
                for (std::size_t member = 0U; member < group_count; ++member) {
                    const auto input = group_indices[member];
                    if (!pwm_frequency_meets_request(current_frequency, requests[input]) ||
                        pwm_resolution_bits(slice_state.period) <
                            requests[input].min_resolution_bits) {
                        results[input].result = PwmResult::incompatible;
                        route_valid[input] = false;
                    }
                }
            }
        } else {
            if (reconfiguration) {
                for (std::size_t member = 0U; member < group_count; ++member) {
                    results[group_indices[member]].result = PwmResult::stale_handle;
                    route_valid[group_indices[member]] = false;
                }
                continue;
            }
            std::size_t accepted_count = 0U;
            std::size_t accepted_indices[kMaximumPwmOutputs]{};
            for (std::size_t member = 0U; member < group_count; ++member) {
                const auto input = group_indices[member];
                bool route_already_admitted = false;
                for (std::size_t prior = 0U; prior < accepted_count; ++prior) {
                    route_already_admitted = route_already_admitted ||
                                             channels[accepted_indices[prior]] == channels[input];
                }
                if (route_already_admitted) {
                    results[input].result = PwmResult::ownership_conflict;
                    route_valid[input] = false;
                    continue;
                }
                PwmRequest provisional[kMaximumPwmOutputs]{};
                for (std::size_t prior = 0U; prior < accepted_count; ++prior) {
                    provisional[prior] = requests[accepted_indices[prior]];
                }
                provisional[accepted_count] = requests[input];
                PwmEffectiveSettings selected{};
                std::uint16_t selected_divider = 0U;
                std::uint16_t selected_period = 0U;
                if (!find_group_timing(provisional, accepted_count + 1U, state.plan->clock_hz,
                        selected_divider, selected_period, selected)) {
                    results[input].result = PwmResult::requirements_unsatisfied;
                    route_valid[input] = false;
                    continue;
                }
                accepted_indices[accepted_count] = input;
                ++accepted_count;
                divider_q4 = selected_divider;
                period = selected_period;
            }
        }

        for (std::size_t member = 0U; member < group_count; ++member) {
            const auto input = group_indices[member];
            if (route_valid[input] && results[input].result == PwmResult::invalid_request) {
                add_entry(input, divider_q4, period, retune);
            }
        }
    }

    std::size_t compact_count = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        if (!state.plan->entries[index].valid) {
            continue;
        }
        if (compact_count != index) {
            state.plan->entries[compact_count] = state.plan->entries[index];
        }
        ++compact_count;
    }
    state.plan->count = compact_count;
    state.plan->valid = compact_count > 0U;
    return PwmResult::success;
}

/** @brief Confirms all members of a newly configured slice as one operation. */
void set_slice_outcomes(PwmBackendOperationResult* results, const std::size_t* plan_indices,
    const std::size_t count, const PwmApplyState applied, const PwmResult result,
    const PwmOutputState state, const std::uint16_t domain) {
    for (std::size_t index = 0U; index < count; ++index) {
        results[plan_indices[index]] = {applied, result, state, domain};
    }
}

/** @brief Restores safe SIO levels and verifies GPIO readback. */
[[nodiscard]] bool safe_group_pads(
    const NativePlan& plan, const std::size_t* entries, const std::size_t count) {
    bool safe = true;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = plan.entries[entries[index]];
        safe = prepare_safe_gpio(entry.request.pin.value(), entry.request.inverted) && safe;
    }
    return safe;
}

/** @brief Activates a slice's initial outputs after preload and readback. */
[[nodiscard]] bool activate_fresh_slice(NativeState& state, const std::size_t* entries,
    const std::size_t count, PwmBackendOperationResult* results) {
    const auto& first = state.plan->entries[entries[0]];
    const auto slice = first.slice;
    if ((pwm_hw->en & (std::uint32_t{1U} << slice)) != 0U) {
        set_slice_outcomes(results, entries, count, PwmApplyState::failed,
            PwmResult::resource_unavailable, PwmOutputState::active, slice_failure_domain(slice));
        return false;
    }

    // Keep the native claim addressable if any subsequent register or pad
    // mutation becomes uncertain, so pwm_release can still recover it.
    retain_plan_claims(state, entries, count);

    bool safe = true;
    bool polarity[2]{};
    std::uint16_t levels[2]{};
    std::uint8_t active_mask = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[entries[index]];
        safe = prepare_safe_gpio(entry.request.pin.value(), entry.request.inverted) && safe;
        polarity[entry.channel] = entry.request.inverted;
        levels[entry.channel] = compare_for(entry.request.initial_duty, entry.period);
        active_mask |= static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
    }
    if (!safe || !configure_fresh_slice(slice, first.divider_q4, first.period, polarity[0],
                     polarity[1], levels[0], levels[1])) {
        const bool pads_safe = safe_group_pads(*state.plan, entries, count);
        pwm_set_enabled(slice, false);
        const bool slice_stopped = (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U;
        const bool confirmed = pads_safe && slice_stopped;
        if (!confirmed) {
            state.slices[slice].quarantined = true;
        } else {
            clear_plan_claims(state, entries, count);
        }
        set_slice_outcomes(results, entries, count,
            confirmed ? PwmApplyState::failed : PwmApplyState::unknown,
            confirmed ? PwmResult::native_failure : PwmResult::quarantined,
            confirmed ? PwmOutputState::active : PwmOutputState::quarantined,
            slice_failure_domain(slice));
        return false;
    }

    bool routed = true;
    for (std::size_t index = 0U; index < count; ++index) {
        routed = route_to_pwm(state.plan->entries[entries[index]].request.pin.value()) && routed;
    }
    if (!routed) {
        const bool pads_safe = safe_group_pads(*state.plan, entries, count);
        pwm_set_enabled(slice, false);
        const bool slice_stopped = (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U;
        const bool confirmed = pads_safe && slice_stopped;
        if (!confirmed) {
            state.slices[slice].quarantined = true;
        } else {
            clear_plan_claims(state, entries, count);
        }
        set_slice_outcomes(results, entries, count,
            confirmed ? PwmApplyState::failed : PwmApplyState::unknown,
            confirmed ? PwmResult::native_failure : PwmResult::quarantined,
            confirmed ? PwmOutputState::active : PwmOutputState::quarantined,
            slice_failure_domain(slice));
        return false;
    }

    pwm_set_enabled(slice, true);
    if ((pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U ||
        !timing_matches(slice, first.divider_q4, first.period)) {
        const bool pads_safe = safe_group_pads(*state.plan, entries, count);
        pwm_set_enabled(slice, false);
        const bool confirmed = pads_safe && (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U;
        if (!confirmed) {
            state.slices[slice].quarantined = true;
        } else {
            clear_plan_claims(state, entries, count);
        }
        set_slice_outcomes(results, entries, count,
            confirmed ? PwmApplyState::failed : PwmApplyState::unknown,
            confirmed ? PwmResult::native_failure : PwmResult::quarantined,
            confirmed ? PwmOutputState::active : PwmOutputState::quarantined,
            slice_failure_domain(slice));
        return false;
    }

    auto& slice_state = state.slices[slice];
    slice_state.initialized = true;
    slice_state.divider_q4 = first.divider_q4;
    slice_state.period = first.period;
    slice_state.active_mask = active_mask;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto plan_index = entries[index];
        const auto& entry = state.plan->entries[plan_index];
        slice_state.inverted[entry.channel] = entry.request.inverted;
        slice_state.pins[entry.channel] = entry.request.pin.value();
        slice_state.duties[entry.channel] = entry.request.initial_duty;
        slice_state.handles[entry.channel] = entry.handle;
        results[plan_index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            slice_failure_domain(slice)};
    }
    return true;
}

/** @brief Adds channels to an already running slice without resetting siblings. */
void activate_existing_slice(NativeState& state, const std::size_t* entries,
    const std::size_t count, PwmBackendOperationResult* results) {
    if (count == 0U) {
        return;
    }
    const auto& first = state.plan->entries[entries[0]];
    const auto slice = first.slice;
    auto& slice_state = state.slices[slice];
    retain_plan_claims(state, entries, count);
    if (slice_state.quarantined ||
        !timing_matches(slice, slice_state.divider_q4, slice_state.period) ||
        (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U) {
        set_slice_outcomes(results, entries, count, PwmApplyState::unknown, PwmResult::quarantined,
            PwmOutputState::quarantined, slice_failure_domain(slice));
        slice_state.quarantined = true;
        return;
    }

    for (std::size_t index = 0U; index < count; ++index) {
        const auto plan_index = entries[index];
        const auto& entry = state.plan->entries[plan_index];
        if (!prepare_safe_gpio(entry.request.pin.value(), entry.request.inverted)) {
            slice_state.quarantined = true;
            pwm_set_enabled(slice, false);
            set_slice_outcomes(results, entries, count, PwmApplyState::unknown,
                PwmResult::quarantined, PwmOutputState::quarantined, slice_failure_domain(slice));
            return;
        }
        auto inversion_a = slice_state.inverted[0];
        auto inversion_b = slice_state.inverted[1];
        if (entry.channel == 0U) {
            inversion_a = entry.request.inverted;
        } else {
            inversion_b = entry.request.inverted;
        }
        if (!set_polarities(slice, inversion_a, inversion_b)) {
            slice_state.quarantined = true;
            pwm_set_enabled(slice, false);
            set_slice_outcomes(results, entries, count, PwmApplyState::unknown,
                PwmResult::quarantined, PwmOutputState::quarantined, slice_failure_domain(slice));
            return;
        }
        const auto duty = entry.reconfiguration ? entry.old_duty : entry.request.initial_duty;
        const auto level = compare_for(duty, slice_state.period);
        pwm_set_chan_level(slice, entry.channel, level);
        if (compare_level(slice, entry.channel) != level) {
            slice_state.quarantined = true;
            pwm_set_enabled(slice, false);
            set_slice_outcomes(results, entries, count, PwmApplyState::unknown,
                PwmResult::quarantined, PwmOutputState::quarantined, slice_failure_domain(slice));
            return;
        }
        if (!route_to_pwm(entry.request.pin.value())) {
            slice_state.quarantined = true;
            pwm_set_enabled(slice, false);
            set_slice_outcomes(results, entries, count, PwmApplyState::unknown,
                PwmResult::quarantined, PwmOutputState::quarantined, slice_failure_domain(slice));
            return;
        }
        slice_state.active_mask |= static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
        slice_state.inverted[entry.channel] = entry.request.inverted;
        slice_state.pins[entry.channel] = entry.request.pin.value();
        slice_state.duties[entry.channel] = duty;
        slice_state.handles[entry.channel] = entry.handle;
        results[plan_index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            slice_failure_domain(slice)};
    }
}

/** @brief Retunes a participating slice while its pads are held inactive. */
void activate_retuned_slice(NativeState& state, const std::size_t* entries, const std::size_t count,
    PwmBackendOperationResult* results) {
    const auto& first = state.plan->entries[entries[0]];
    const auto slice = first.slice;
    auto& slice_state = state.slices[slice];
    bool safe = true;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[entries[index]];
        safe = prepare_safe_gpio(entry.request.pin.value(), entry.request.inverted) && safe;
    }
    if (!safe) {
        slice_state.quarantined = true;
        set_slice_outcomes(results, entries, count, PwmApplyState::unknown, PwmResult::quarantined,
            PwmOutputState::quarantined, slice_failure_domain(slice));
        return;
    }

    pwm_set_enabled(slice, false);
    bool programmed = (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U;
    pwm_set_clkdiv_int_frac4(slice, static_cast<std::uint8_t>(first.divider_q4 / 16U),
        static_cast<std::uint8_t>(first.divider_q4 % 16U));
    pwm_set_wrap(slice, static_cast<std::uint16_t>(first.period - 1U));
    bool polarity[2]{};
    std::uint16_t levels[2]{};
    std::uint8_t seen = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[entries[index]];
        polarity[entry.channel] = entry.request.inverted;
        levels[entry.channel] = compare_for(entry.old_duty, entry.period);
        seen |= static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
    }
    for (std::uint8_t channel = 0U; channel < 2U; ++channel) {
        if ((seen & (std::uint8_t{1U} << channel)) == 0U) {
            polarity[channel] = slice_state.inverted[channel];
            levels[channel] = compare_for(slice_state.duties[channel], first.period);
        }
    }
    programmed = set_polarities(slice, polarity[0], polarity[1]) && programmed;
    pwm_set_both_levels(slice, levels[0], levels[1]);
    programmed = programmed &&
                 pwm_hw->slice[slice].cc == ((static_cast<std::uint32_t>(levels[1]) << 16U) |
                                                static_cast<std::uint32_t>(levels[0])) &&
                 timing_matches(slice, first.divider_q4, first.period);
    if (programmed) {
        for (std::size_t index = 0U; index < count; ++index) {
            programmed =
                route_to_pwm(state.plan->entries[entries[index]].request.pin.value()) && programmed;
        }
    }
    if (programmed) {
        pwm_set_enabled(slice, true);
        programmed = (pwm_hw->en & (std::uint32_t{1U} << slice)) != 0U;
    }

    if (!programmed) {
        bool pads_safe = true;
        for (std::size_t index = 0U; index < count; ++index) {
            const auto& entry = state.plan->entries[entries[index]];
            pads_safe =
                prepare_safe_gpio(entry.request.pin.value(), entry.request.inverted) && pads_safe;
        }
        pwm_set_enabled(slice, false);
        const bool stopped = (pwm_hw->en & (std::uint32_t{1U} << slice)) == 0U;
        slice_state.quarantined = true;
        set_slice_outcomes(results, entries, count, PwmApplyState::unknown,
            pads_safe && stopped ? PwmResult::native_failure : PwmResult::quarantined,
            PwmOutputState::quarantined, slice_failure_domain(slice));
        return;
    }

    slice_state.divider_q4 = first.divider_q4;
    slice_state.period = first.period;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto plan_index = entries[index];
        const auto& entry = state.plan->entries[plan_index];
        slice_state.inverted[entry.channel] = entry.request.inverted;
        slice_state.pins[entry.channel] = entry.request.pin.value();
        slice_state.duties[entry.channel] = entry.old_duty;
        slice_state.handles[entry.channel] = entry.handle;
        results[plan_index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            slice_failure_domain(slice)};
    }
}

/** @brief Applies each planned slice while preserving unrelated slice state. */
PwmResult activate_native(void* const opaque, const PwmPlanToken token, const bool reconfiguration,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendOperationResult* const results) {
    auto& state = *static_cast<NativeState*>(opaque);
    if (!state.plan || !state.plan->valid || state.plan->token.slot != token.slot ||
        state.plan->token.generation != token.generation ||
        state.plan->reconfiguration != reconfiguration || state.plan->count != count ||
        count > kMaximumPwmOutputs || clock_get_hz(clk_sys) != state.plan->clock_hz) {
        return PwmResult::stale_plan;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (!same_output_id(requests[index].logical_output,
                state.plan->entries[index].request.logical_output) ||
            handles[index].slot != state.plan->entries[index].handle.slot ||
            handles[index].generation != state.plan->entries[index].handle.generation) {
            return PwmResult::stale_plan;
        }
        results[index] = {};
    }

    bool processed[kSliceCount]{};
    for (std::size_t index = 0U; index < count; ++index) {
        const auto slice = state.plan->entries[index].slice;
        if (processed[slice]) {
            continue;
        }
        processed[slice] = true;
        std::size_t entries[kMaximumPwmOutputs]{};
        std::size_t entry_count = 0U;
        bool retune = false;
        for (std::size_t candidate = index; candidate < count; ++candidate) {
            if (state.plan->entries[candidate].slice == slice) {
                entries[entry_count] = candidate;
                ++entry_count;
                retune = retune || state.plan->entries[candidate].retune;
            }
        }
        auto& slice_state = state.slices[slice];
        if (retune) {
            activate_retuned_slice(state, entries, entry_count, results);
        } else if (slice_state.active_mask == 0U) {
            (void)activate_fresh_slice(state, entries, entry_count, results);
        } else {
            activate_existing_slice(state, entries, entry_count, results);
        }
    }
    state.plan.reset();
    return PwmResult::success;
}

/** @brief Confirms one output level or quarantines uncertain slice readback. */
PwmResult write_native(void* const opaque, const PwmDutyUpdate* const updates,
    const std::size_t count, PwmBackendOperationResult* const results) {
    auto& state = *static_cast<NativeState*>(opaque);
    if (updates == nullptr || results == nullptr || count == 0U || count > kMaximumPwmOutputs) {
        return PwmResult::invalid_request;
    }
    std::uint8_t slices[kMaximumPwmOutputs]{};
    std::uint8_t channels[kMaximumPwmOutputs]{};
    std::uint16_t levels[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < count; ++index) {
        bool found = false;
        for (std::uint8_t slice = 0U; slice < kSliceCount; ++slice) {
            for (std::uint8_t channel = 0U; channel < 2U; ++channel) {
                if (state.slices[slice].handles[channel].slot == updates[index].handle.slot &&
                    state.slices[slice].handles[channel].generation ==
                        updates[index].handle.generation &&
                    (state.slices[slice].active_mask & (std::uint8_t{1U} << channel)) != 0U) {
                    slices[index] = slice;
                    channels[index] = channel;
                    levels[index] = compare_for(updates[index].duty, state.slices[slice].period);
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found || state.slices[slices[index]].quarantined) {
            results[index] = {
                PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        }
    }

    bool processed[kSliceCount]{};
    for (std::size_t index = 0U; index < count; ++index) {
        const auto slice = slices[index];
        if (processed[slice] || results[index].result == PwmResult::stale_handle) {
            continue;
        }
        processed[slice] = true;
        std::size_t members[2]{};
        std::size_t member_count = 0U;
        for (std::size_t candidate = index; candidate < count; ++candidate) {
            if (slices[candidate] == slice &&
                results[candidate].result != PwmResult::stale_handle) {
                members[member_count] = candidate;
                ++member_count;
            }
        }
        if (member_count == 2U && channels[members[0]] != channels[members[1]]) {
            const auto channel_a =
                channels[members[0]] == 0U ? levels[members[0]] : levels[members[1]];
            const auto channel_b =
                channels[members[0]] == 1U ? levels[members[0]] : levels[members[1]];
            pwm_set_both_levels(slice, channel_a, channel_b);
        } else {
            pwm_set_chan_level(slice, channels[members[0]], levels[members[0]]);
        }
        bool confirmed = true;
        for (std::size_t member = 0U; member < member_count; ++member) {
            const auto input = members[member];
            confirmed = compare_level(slice, channels[input]) == levels[input] && confirmed;
        }
        const auto domain = slice_failure_domain(slice);
        if (!confirmed) {
            state.slices[slice].quarantined = true;
        }
        for (std::size_t member = 0U; member < member_count; ++member) {
            results[members[member]] = {confirmed ? PwmApplyState::applied : PwmApplyState::unknown,
                confirmed ? PwmResult::success : PwmResult::quarantined,
                confirmed ? PwmOutputState::active : PwmOutputState::quarantined, domain};
        }
    }
    return PwmResult::success;
}

/** @brief Safely releases one channel while retaining an active sibling. */
PwmResult release_native(
    void* const opaque, const PwmBackendActiveOutput& output, PwmBackendOperationResult& result) {
    auto& state = *static_cast<NativeState*>(opaque);
    bool found = false;
    std::uint8_t slice = 0U;
    std::uint8_t channel = 0U;
    for (std::uint8_t candidate = 0U; candidate < kSliceCount; ++candidate) {
        for (std::uint8_t ch = 0U; ch < 2U; ++ch) {
            if (state.slices[candidate].handles[ch].slot == output.handle.slot &&
                state.slices[candidate].handles[ch].generation == output.handle.generation &&
                (state.slices[candidate].active_mask & (std::uint8_t{1U} << ch)) != 0U) {
                slice = candidate;
                channel = ch;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        result = {PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        return PwmResult::success;
    }

    const auto domain = slice_failure_domain(slice);
    const bool safe = prepare_safe_gpio(output.request.pin.value(), output.request.inverted);
    if (!safe) {
        state.slices[slice].quarantined = true;
        result = {
            PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined, domain};
        return PwmResult::success;
    }
    pwm_set_chan_level(slice, channel, 0U);
    if (compare_level(slice, channel) != 0U) {
        state.slices[slice].quarantined = true;
        result = {
            PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined, domain};
        return PwmResult::success;
    }

    auto& slice_state = state.slices[slice];
    const auto remaining_mask =
        static_cast<std::uint8_t>(slice_state.active_mask & ~(std::uint8_t{1U} << channel));
    if (remaining_mask == 0U) {
        pwm_set_enabled(slice, false);
        if ((pwm_hw->en & (std::uint32_t{1U} << slice)) != 0U) {
            slice_state.quarantined = true;
            result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
                domain};
            return PwmResult::success;
        }
    }
    slice_state.active_mask = remaining_mask;
    slice_state.handles[channel] = {};
    slice_state.pins[channel] = 0U;
    slice_state.duties[channel] = 0U;
    slice_state.inverted[channel] = false;
    if (slice_state.active_mask == 0U) {
        slice_state.initialized = false;
        slice_state.divider_q4 = 0U;
        slice_state.period = 0U;
        slice_state.quarantined = false;
    }
    result = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active, domain};
    return PwmResult::success;
}

/** @brief Frees a plan the controller will not activate. */
void discard_native(void* const opaque) {
    static_cast<NativeState*>(opaque)->plan.reset();
}

NativeState g_native_state{};
const PwmBackendOps kNativeBackend{
    prepare_native, activate_native, write_native, release_native, discard_native};

} // namespace

/** @brief Returns the RP2350's PWM implementation. */
const PwmBackendOps& pwm_native_backend() {
    return kNativeBackend;
}

/** @brief Returns the RP2350's retained slice tables. */
void* pwm_native_context() {
    return &g_native_state;
}

} // namespace newmota::platform::detail
