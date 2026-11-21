/*
  pwm.cpp - ESP-IDF LEDC adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/pwm_detail.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_clk_tree.h"
#include "esp_idf_version.h"
#include "hal/ledc_hal.h"
#include "soc/ledc_struct.h"
#include "soc/soc_caps.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace newmota::platform::detail {
namespace {

constexpr std::size_t kModeCount = LEDC_SPEED_MODE_MAX;
constexpr std::size_t kTimerCount = SOC_LEDC_TIMER_NUM;
constexpr std::size_t kChannelCount = SOC_LEDC_CHANNEL_NUM;
constexpr std::uint64_t kPpm = 1'000'000U;
constexpr std::uint32_t kDividerFractionBits = 8U;
constexpr std::uint32_t kDividerMin = 256U;
constexpr std::uint32_t kDividerMax = 0x3ffffU;
// Keep one timer count in reserve: IDF documents that full-on at the
// hardware's maximum resolution overflows the LEDC duty counter.
constexpr std::uint8_t kHardwareMaximumResolution = SOC_LEDC_TIMER_BIT_WIDTH;
constexpr std::uint8_t kMaximumResolution =
    kHardwareMaximumResolution <= 16U ? kHardwareMaximumResolution - 1U : 16U;

#if SOC_LEDC_SUPPORT_APB_CLOCK
constexpr ledc_clk_cfg_t kClockSource = LEDC_USE_APB_CLK;
#elif SOC_LEDC_SUPPORT_PLL_DIV_CLOCK
constexpr ledc_clk_cfg_t kClockSource = LEDC_USE_PLL_DIV_CLK;
#else
#error "No checked LEDC clock source is available on this target"
#endif

struct TimerConfig {
    bool valid{};
    std::uint8_t resolution{};
    std::uint32_t divider{};
    std::uint32_t bootstrap_hz{};
    std::uint32_t source_hz{};
    PwmFrequency effective{};
};

struct TimerState {
    bool configured{};
    bool quarantined{};
    std::uint8_t active_mask{};
    TimerConfig config{};
};

struct ChannelState {
    bool active{};
    PwmHandle handle{};
    PwmRequest request{};
    std::uint16_t duty{};
    std::uint8_t timer{};
    PwmEffectiveSettings effective{};
};

struct PlanEntry {
    PwmRequest request{};
    PwmHandle handle{};
    PwmEffectiveSettings effective{};
    std::uint16_t old_duty{};
    std::uint8_t mode{};
    std::uint8_t channel{};
    std::uint8_t timer{};
    bool reconfiguration{};
    bool retune{};
};

struct NativePlan {
    bool valid{};
    bool reconfiguration{};
    PwmPlanToken token{};
    std::uint32_t source_hz{};
    std::size_t count{};
    PlanEntry entries[kMaximumPwmOutputs]{};
    bool configure_timer[kModeCount][kTimerCount]{};
    TimerConfig timers[kModeCount][kTimerCount]{};
};

struct NativeState {
    TimerState timers[kModeCount][kTimerCount]{};
    ChannelState channels[kModeCount][kChannelCount]{};
    /** @brief Exists from prepare until activation or discard. */
    std::unique_ptr<NativePlan> plan;
};

/** @brief Returns one request's exact relative frequency error. */
void frequency_error(const PwmFrequency effective, const PwmRequest& request,
    std::uint64_t& numerator, std::uint64_t& denominator) {
    const auto target = static_cast<std::uint64_t>(request.frequency_hz) * effective.denominator;
    numerator = effective.numerator_hz > target ? effective.numerator_hz - target
                                                : target - effective.numerator_hz;
    denominator = target;
}

/** @brief Tracks worst relative errors on either side of requested carriers. */
struct RelativeErrorSides {
    std::uint64_t above_numerator{};
    std::uint64_t above_denominator{1U};
    std::uint64_t below_numerator{};
    std::uint64_t below_denominator{1U};
};

[[nodiscard]] RelativeErrorSides frequency_error_sides(
    const PwmFrequency effective, const PwmRequest* requests, const std::size_t count) {
    RelativeErrorSides errors{};
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint64_t numerator = 0U;
        std::uint64_t denominator = 1U;
        frequency_error(effective, requests[index], numerator, denominator);
        const auto target =
            static_cast<std::uint64_t>(requests[index].frequency_hz) * effective.denominator;
        if (effective.numerator_hz > target &&
            compare_fractions(
                numerator, denominator, errors.above_numerator, errors.above_denominator) > 0) {
            errors.above_numerator = numerator;
            errors.above_denominator = denominator;
        } else if (effective.numerator_hz < target &&
                   compare_fractions(numerator, denominator, errors.below_numerator,
                       errors.below_denominator) > 0) {
            errors.below_numerator = numerator;
            errors.below_denominator = denominator;
        }
    }
    return errors;
}

[[nodiscard]] int compare_error_sides(const RelativeErrorSides& errors) {
    return compare_fractions(errors.above_numerator, errors.above_denominator,
        errors.below_numerator, errors.below_denominator);
}

/** @brief Compares a planned carrier to one exact request tolerance bound. */
[[nodiscard]] int compare_frequency_to_bound(const std::uint32_t source_hz,
    const std::uint32_t divider, const std::uint8_t resolution, const PwmRequest& request,
    const std::uint64_t bound_ppm) {
    const auto numerator = static_cast<std::uint64_t>(source_hz) << kDividerFractionBits;
    const auto denominator =
        static_cast<std::uint64_t>(divider) * (std::uint64_t{1U} << resolution);
    const auto bound_numerator = static_cast<std::uint64_t>(request.frequency_hz) * bound_ppm;
    return compare_fractions(numerator, denominator, bound_numerator, kPpm);
}

/** @brief Finds the first divider whose carrier is at or below a bound. */
[[nodiscard]] std::uint32_t first_divider_at_or_below(const std::uint32_t source_hz,
    const std::uint8_t resolution, const PwmRequest& request, const std::uint64_t bound_ppm) {
    auto lower = kDividerMin;
    auto upper = kDividerMax + 1U;
    while (lower < upper) {
        const auto middle = lower + (upper - lower) / 2U;
        if (compare_frequency_to_bound(source_hz, middle, resolution, request, bound_ppm) <= 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return lower;
}

/** @brief Finds the last divider whose carrier is at or above a bound. */
[[nodiscard]] std::uint32_t last_divider_at_or_above(const std::uint32_t source_hz,
    const std::uint8_t resolution, const PwmRequest& request, const std::uint64_t bound_ppm) {
    auto lower = kDividerMin;
    auto upper = kDividerMax + 1U;
    while (lower < upper) {
        const auto middle = lower + (upper - lower) / 2U;
        if (compare_frequency_to_bound(source_hz, middle, resolution, request, bound_ppm) < 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return lower - 1U;
}

/** @brief Computes a timer using exact divider ranges and minimax candidates. */
[[nodiscard]] bool make_timer_config(const PwmRequest* requests, const std::size_t count,
    const std::uint32_t source_hz, TimerConfig& selected) {
    if (requests == nullptr || count == 0U || source_hz == 0U) {
        return false;
    }
    bool found = false;
    std::uint8_t best_bits = 0U;
    std::uint64_t best_error_num = 0U;
    std::uint64_t best_error_den = 1U;
    TimerConfig best{};
    for (std::uint8_t bits = kMaximumResolution; bits >= 1U; --bits) {
        bool enough_resolution = true;
        for (std::size_t request_index = 0U; request_index < count; ++request_index) {
            if (bits < requests[request_index].min_resolution_bits) {
                enough_resolution = false;
                break;
            }
        }
        if (!enough_resolution) {
            break;
        }
        const auto scaled_source = static_cast<std::uint64_t>(source_hz) << kDividerFractionBits;
        auto first_allowed = kDividerMin;
        auto last_allowed = kDividerMax;
        for (std::size_t request_index = 0U; request_index < count; ++request_index) {
            const auto& request = requests[request_index];
            const auto upper_bound = kPpm + request.max_error_ppm;
            const auto lower_bound = kPpm - request.max_error_ppm;
            const auto first_for_request =
                first_divider_at_or_below(source_hz, bits, request, upper_bound);
            const auto last_for_request =
                last_divider_at_or_above(source_hz, bits, request, lower_bound);
            first_allowed = std::max(first_allowed, first_for_request);
            last_allowed = std::min(last_allowed, last_for_request);
            if (first_allowed > last_allowed || first_allowed > kDividerMax ||
                last_allowed < kDividerMin) {
                break;
            }
        }
        if (first_allowed > last_allowed || first_allowed > kDividerMax ||
            last_allowed < kDividerMin) {
            continue;
        }

        const auto consider_divider = [&](const std::uint32_t candidate) {
            if (candidate < first_allowed || candidate > last_allowed) {
                return;
            }
            const auto precision = std::uint64_t{1U} << bits;
            const PwmFrequency effective{
                scaled_source, static_cast<std::uint64_t>(candidate) * precision};
            for (std::size_t request_index = 0U; request_index < count; ++request_index) {
                if (!pwm_frequency_meets_request(effective, requests[request_index])) {
                    return;
                }
            }
            const auto errors = frequency_error_sides(effective, requests, count);
            const bool above_is_worst = compare_error_sides(errors) >= 0;
            const auto worst_num = above_is_worst ? errors.above_numerator : errors.below_numerator;
            const auto worst_den =
                above_is_worst ? errors.above_denominator : errors.below_denominator;
            const auto error_order =
                compare_fractions(worst_num, worst_den, best_error_num, best_error_den);
            const bool better =
                !found || error_order < 0 ||
                (error_order == 0 &&
                    (bits > best_bits || (bits == best_bits && candidate < best.divider)));
            if (better) {
                found = true;
                best_bits = bits;
                best_error_num = worst_num;
                best_error_den = worst_den;
                best = {true, bits, candidate, source_hz / 2U, source_hz, effective};
            }
        };

        // The worst error above the requested carriers decreases with the
        // divider, while the worst error below them increases. Their crossing
        // brackets the exact minimax point, including asymmetric tolerances.
        auto crossing = first_allowed;
        auto upper = last_allowed;
        while (crossing < upper) {
            const auto middle = crossing + (upper - crossing) / 2U;
            const PwmFrequency effective{
                scaled_source, static_cast<std::uint64_t>(middle) * (std::uint64_t{1U} << bits)};
            const auto errors = frequency_error_sides(effective, requests, count);
            if (compare_error_sides(errors) <= 0) {
                upper = middle;
            } else {
                crossing = middle + 1U;
            }
        }
        consider_divider(first_allowed);
        consider_divider(last_allowed);
        consider_divider(crossing);
        if (crossing > first_allowed) {
            consider_divider(crossing - 1U);
        }
        if (bits == 1U) {
            break;
        }
    }
    selected = best;
    return found;
}

/** @brief Reads and verifies exact configured LEDC timer state. */
[[nodiscard]] bool read_timer(
    const std::uint8_t mode, const std::uint8_t timer, TimerConfig& config) {
    ledc_hal_context_t hal{};
    ledc_hal_init(&hal, static_cast<ledc_mode_t>(mode));
    std::uint32_t divider = 0U;
    std::uint32_t resolution = 0U;
    ledc_clk_cfg_t source{};
    const auto timer_sel = static_cast<ledc_timer_t>(timer);
#if ESP_IDF_VERSION_MAJOR >= 6
    const auto speed_mode = static_cast<ledc_mode_t>(mode);
    ledc_hal_get_clock_divider(&hal, speed_mode, timer_sel, &divider);
    ledc_hal_get_duty_resolution(&hal, speed_mode, timer_sel, &resolution);
    ledc_hal_get_clk_cfg(&hal, speed_mode, timer_sel, &source);
#else
    ledc_hal_get_clock_divider(&hal, timer_sel, &divider);
    ledc_hal_get_duty_resolution(&hal, timer_sel, &resolution);
    ledc_hal_get_clk_cfg(&hal, timer_sel, &source);
#endif
    if (divider < kDividerMin || divider > kDividerMax || resolution == 0U ||
        resolution > kMaximumResolution) {
        return false;
    }
    std::uint32_t source_hz = 0U;
    if (esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(source),
            ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &source_hz) != ESP_OK ||
        source_hz == 0U) {
        return false;
    }
    const auto precision = std::uint64_t{1U} << resolution;
    config = {true, static_cast<std::uint8_t>(resolution), divider, 0U, source_hz,
        {static_cast<std::uint64_t>(source_hz) << kDividerFractionBits,
            static_cast<std::uint64_t>(divider) * precision}};
    return true;
}

/** @brief Confirms the programmed timer matches a staged rational config. */
[[nodiscard]] bool timer_matches(
    const std::uint8_t mode, const std::uint8_t timer, const TimerConfig& expected);

/** @brief Installs the exact timer plan through ESP-IDF's LEDC HAL. */
[[nodiscard]] bool program_timer(
    const std::uint8_t mode, const std::uint8_t timer, const TimerConfig& config) {
    const auto speed_mode = static_cast<ledc_mode_t>(mode);
    const auto timer_sel = static_cast<ledc_timer_t>(timer);
#if ESP_IDF_VERSION_MAJOR >= 6
    if (ledc_timer_pause(speed_mode, timer_sel) != ESP_OK) {
        return false;
    }

    ledc_hal_context_t hal{};
    ledc_hal_init(&hal, speed_mode);
    ledc_hal_set_clock_divider(&hal, speed_mode, timer_sel, config.divider);
#if SOC_LEDC_HAS_TIMER_SPECIFIC_MUX
    ledc_hal_set_clock_source(
        &hal, speed_mode, timer_sel, static_cast<ledc_clk_src_t>(kClockSource));
#endif
    ledc_hal_set_duty_resolution(
        &hal, speed_mode, timer_sel, static_cast<ledc_timer_bit_t>(config.resolution));
    ledc_hal_ls_timer_update(&hal, speed_mode, timer_sel);

    const bool matches = timer_matches(mode, timer, config);
    const auto reset_result = ledc_timer_rst(speed_mode, timer_sel);
    const auto resume_result = ledc_timer_resume(speed_mode, timer_sel);
    return matches && reset_result == ESP_OK && resume_result == ESP_OK;
#else
    // IDF 5.5 retains ledc_timer_set(), which accepts the exact fixed-point
    // divider selected by the shared PWM planner. It is deprecated there only in
    // favor of ledc_timer_config()/ledc_set_freq(), which recompute the divider.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return ledc_timer_set(speed_mode, timer_sel, config.divider, config.resolution,
               static_cast<ledc_clk_src_t>(kClockSource)) == ESP_OK &&
           timer_matches(mode, timer, config);
#pragma GCC diagnostic pop
#endif
}

/** @brief Confirms the programmed timer matches a staged rational config. */
[[nodiscard]] bool timer_matches(
    const std::uint8_t mode, const std::uint8_t timer, const TimerConfig& expected) {
    TimerConfig actual{};
    return read_timer(mode, timer, actual) && actual.divider == expected.divider &&
           actual.resolution == expected.resolution &&
           compare_fractions(actual.effective.numerator_hz, actual.effective.denominator,
               expected.effective.numerator_hz, expected.effective.denominator) == 0;
}

/** @brief Converts a logical duty to an LEDC interval count. */
[[nodiscard]] std::uint32_t native_duty(const std::uint16_t duty, const std::uint8_t bits) {
    return pwm_duty_interval(duty, std::uint32_t{1U} << bits);
}

/** @brief Reads the programmed duty register, not the currently latched cycle. */
[[nodiscard]] bool duty_register_matches(
    const std::uint8_t mode, const std::uint8_t channel, const std::uint32_t duty) {
#if CONFIG_IDF_TARGET_ESP32S31
    if (mode == 0U) {
        return LEDC0.channel_group[0].channel[channel].duty_init.duty == (duty << 4U);
    }
    if (mode == 1U) {
        return LEDC1.channel_group[0].channel[channel].duty_init.duty == (duty << 4U);
    }
    return false;
#elif CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C61
    return LEDC.channel_group[mode].channel[channel].duty_init.duty == (duty << 4U);
#else
    return LEDC.channel_group[mode].channel[channel].duty.duty == (duty << 4U);
#endif
}

/** @brief Returns a checked pad to a static inactive GPIO output. */
[[nodiscard]] bool safe_gpio(const std::uint16_t pin, const bool inverted) {
    const auto gpio = static_cast<gpio_num_t>(pin);
    const auto level = inverted ? 1U : 0U;
    return gpio_reset_pin(gpio) == ESP_OK && gpio_set_level(gpio, level) == ESP_OK &&
           gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT) == ESP_OK &&
           gpio_get_level(gpio) == static_cast<int>(level);
}

/** @brief Activates or reconfigures one channel with a retained safe level. */
[[nodiscard]] bool configure_channel(
    const PlanEntry& entry, const std::uint16_t duty, const TimerConfig& timing) {
    if (!safe_gpio(entry.request.pin.value(), entry.request.inverted)) {
        return false;
    }
    ledc_channel_config_t channel{};
    channel.gpio_num = entry.request.pin.value();
    channel.speed_mode = static_cast<ledc_mode_t>(entry.mode);
    channel.channel = static_cast<ledc_channel_t>(entry.channel);
    channel.timer_sel = static_cast<ledc_timer_t>(entry.timer);
    channel.duty = native_duty(duty, timing.resolution);
    channel.hpoint = 0;
    channel.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
    channel.flags.output_invert = entry.request.inverted ? 1U : 0U;
    if (ledc_channel_config(&channel) != ESP_OK) {
        return false;
    }
    return timer_matches(entry.mode, entry.timer, timing) &&
           duty_register_matches(entry.mode, entry.channel, channel.duty);
}

/** @brief Encodes a mode/timer pair as one nonzero failure domain. */
[[nodiscard]] std::uint16_t timer_domain(const std::uint8_t mode, const std::uint8_t timer) {
    return static_cast<std::uint16_t>(
        static_cast<unsigned>(mode) * static_cast<unsigned>(kTimerCount) +
        static_cast<unsigned>(timer) + 1U);
}

/** @brief Retains one uncertain native channel so release remains possible. */
void retain_uncertain_channel(
    NativeState& state, const std::size_t index, PwmBackendOperationResult* results) {
    const auto& entry = state.plan->entries[index];
    auto& retained = state.channels[entry.mode][entry.channel];
    retained.active = true;
    retained.handle = entry.handle;
    retained.request = entry.request;
    retained.duty = entry.reconfiguration ? entry.old_duty : entry.request.initial_duty;
    retained.timer = entry.timer;
    retained.effective = entry.effective;
    state.timers[entry.mode][entry.timer].active_mask |=
        static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
    state.timers[entry.mode][entry.timer].quarantined = true;
    results[index] = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
        timer_domain(entry.mode, entry.timer)};
}

/** @brief Stops every staged output in a timer after a partial retune. */
void quarantine_timer_plan(NativeState& state, const std::uint8_t mode, const std::uint8_t timer,
    PwmBackendOperationResult* results) {
    state.timers[mode][timer].quarantined = true;
    for (std::size_t index = 0U; index < state.plan->count; ++index) {
        const auto& entry = state.plan->entries[index];
        if (entry.mode != mode || entry.timer != timer) {
            continue;
        }
        (void)ledc_stop(
            static_cast<ledc_mode_t>(mode), static_cast<ledc_channel_t>(entry.channel), 0U);
        (void)safe_gpio(entry.request.pin.value(), entry.request.inverted);
        retain_uncertain_channel(state, index, results);
    }
}

/** @brief Plans deterministic LEDC channels and compatible timing groups. */
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
    if (count > kMaximumPwmOutputs || active_count > kMaximumPwmOutputs || requests == nullptr ||
        handles == nullptr || results == nullptr || (active_count > 0U && active == nullptr)) {
        return PwmResult::invalid_request;
    }
    std::uint32_t source_hz = 0U;
    if (esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(kClockSource),
            ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &source_hz) != ESP_OK ||
        source_hz == 0U) {
        return PwmResult::native_failure;
    }
    state.plan->source_hz = source_hz;

    if (reconfiguration) {
        bool target_found[kMaximumPwmOutputs]{};
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {};
            for (std::size_t mode = 0U; mode < kModeCount; ++mode) {
                for (std::size_t channel = 0U; channel < kChannelCount; ++channel) {
                    const auto& retained = state.channels[mode][channel];
                    if (retained.active && retained.handle.slot == handles[index].slot &&
                        retained.handle.generation == handles[index].generation &&
                        retained.request.pin.value() == requests[index].pin.value()) {
                        target_found[index] = true;
                        auto& entry = state.plan->entries[index];
                        entry = {requests[index], handles[index], retained.effective, retained.duty,
                            static_cast<std::uint8_t>(mode), static_cast<std::uint8_t>(channel),
                            retained.timer, true, false};
                        results[index].result = PwmResult::success;
                        break;
                    }
                }
            }
            if (!target_found[index]) {
                results[index].result = PwmResult::stale_handle;
            }
        }

        bool processed[kModeCount][kTimerCount]{};
        for (std::size_t index = 0U; index < count; ++index) {
            if (!target_found[index]) {
                continue;
            }
            const auto& first = state.plan->entries[index];
            if (processed[first.mode][first.timer]) {
                continue;
            }
            processed[first.mode][first.timer] = true;
            PwmRequest group[kMaximumPwmOutputs]{};
            std::size_t members[kMaximumPwmOutputs]{};
            std::size_t member_count = 0U;
            for (std::size_t candidate = 0U; candidate < count; ++candidate) {
                const auto& entry = state.plan->entries[candidate];
                if (target_found[candidate] && entry.mode == first.mode &&
                    entry.timer == first.timer) {
                    group[member_count] = requests[candidate];
                    members[member_count++] = candidate;
                }
            }
            TimerConfig current{};
            const bool have_current = read_timer(first.mode, first.timer, current);
            bool current_works = have_current;
            if (current_works) {
                for (std::size_t member = 0U; member < member_count; ++member) {
                    const auto& request = group[member];
                    current_works = current_works &&
                                    pwm_frequency_meets_request(current.effective, request) &&
                                    current.resolution >= request.min_resolution_bits;
                }
            }
            if (current_works) {
                for (std::size_t member = 0U; member < member_count; ++member) {
                    const auto input = members[member];
                    auto& entry = state.plan->entries[input];
                    entry.effective = {current.effective, current.resolution};
                    results[input].effective = entry.effective;
                    results[input].failure_domain = timer_domain(entry.mode, entry.timer);
                }
                continue;
            }

            bool complete = true;
            const auto active_mask = state.timers[first.mode][first.timer].active_mask;
            for (std::size_t channel = 0U; channel < kChannelCount; ++channel) {
                if ((active_mask & (std::uint8_t{1U} << channel)) == 0U) {
                    continue;
                }
                bool included = false;
                for (std::size_t member = 0U; member < member_count; ++member) {
                    included = included || state.plan->entries[members[member]].channel == channel;
                }
                complete = complete && included;
            }
            TimerConfig replacement{};
            if (!complete || !make_timer_config(group, member_count, source_hz, replacement)) {
                for (std::size_t member = 0U; member < member_count; ++member) {
                    const auto input = members[member];
                    results[input].result =
                        complete ? PwmResult::requirements_unsatisfied : PwmResult::incompatible;
                }
                continue;
            }
            state.plan->configure_timer[first.mode][first.timer] = true;
            state.plan->timers[first.mode][first.timer] = replacement;
            for (std::size_t member = 0U; member < member_count; ++member) {
                const auto input = members[member];
                auto& entry = state.plan->entries[input];
                entry.effective = {replacement.effective, replacement.resolution};
                entry.retune = true;
                results[input].effective = entry.effective;
                results[input].failure_domain = timer_domain(entry.mode, entry.timer);
            }
        }
        PlanEntry accepted[kMaximumPwmOutputs]{};
        std::size_t accepted_count = 0U;
        for (std::size_t index = 0U; index < count; ++index) {
            if (results[index].result == PwmResult::success) {
                accepted[accepted_count++] = state.plan->entries[index];
            }
        }
        for (std::size_t index = 0U; index < accepted_count; ++index) {
            state.plan->entries[index] = accepted[index];
        }
        state.plan->count = accepted_count;
        state.plan->valid = true;
        return PwmResult::success;
    }

    bool planned_channels[kModeCount][kChannelCount]{};
    bool planned_timers[kModeCount][kTimerCount]{};
    TimerConfig planned_config[kModeCount][kTimerCount]{};
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {};
        const auto pin = static_cast<gpio_num_t>(requests[index].pin.value());
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
            results[index].result = PwmResult::unsupported_route;
            continue;
        }

        bool found = false;
        std::uint8_t selected_mode = 0U;
        std::uint8_t selected_channel = 0U;
        std::uint8_t selected_timer = 0U;
        TimerConfig selected_config{};
        bool configure_selected = false;
        bool selected_reuses_group = false;
        std::uint64_t selected_error_num = 0U;
        std::uint64_t selected_error_den = 1U;

        for (std::uint8_t mode = 0U; mode < kModeCount; ++mode) {
            for (std::uint8_t timer = 0U; timer < kTimerCount; ++timer) {
                const auto& retained_timer = state.timers[mode][timer];
                if (retained_timer.quarantined) {
                    continue;
                }
                TimerConfig config{};
                bool config_is_new = false;
                if (planned_timers[mode][timer]) {
                    config = planned_config[mode][timer];
                } else if (retained_timer.configured) {
                    if (!read_timer(mode, timer, config)) {
                        state.timers[mode][timer].quarantined = true;
                        continue;
                    }
                    if (config.resolution < requests[index].min_resolution_bits ||
                        !pwm_frequency_meets_request(config.effective, requests[index])) {
                        if (retained_timer.active_mask != 0U ||
                            !make_timer_config(&requests[index], 1U, source_hz, config)) {
                            continue;
                        }
                        config_is_new = true;
                    }
                } else {
                    if (retained_timer.active_mask != 0U) {
                        continue;
                    }
                    if (!make_timer_config(&requests[index], 1U, source_hz, config)) {
                        continue;
                    }
                    config_is_new = true;
                }
                if (config.resolution < requests[index].min_resolution_bits ||
                    !pwm_frequency_meets_request(config.effective, requests[index])) {
                    continue;
                }
                std::uint8_t channel = 0U;
                while (channel < kChannelCount &&
                       (state.channels[mode][channel].active || planned_channels[mode][channel])) {
                    ++channel;
                }
                if (channel == kChannelCount) {
                    continue;
                }
                std::uint64_t error_num = 0U;
                std::uint64_t error_den = 1U;
                frequency_error(config.effective, requests[index], error_num, error_den);
                const auto error_order =
                    compare_fractions(error_num, error_den, selected_error_num, selected_error_den);
                const bool reuses_group =
                    (retained_timer.configured && !config_is_new) || planned_timers[mode][timer];
                const bool better =
                    !found || reuses_group > selected_reuses_group ||
                    (reuses_group == selected_reuses_group &&
                        (error_order < 0 ||
                            (error_order == 0 &&
                                (config.resolution > selected_config.resolution ||
                                    (config.resolution == selected_config.resolution &&
                                        (mode < selected_mode ||
                                            (mode == selected_mode &&
                                                (timer < selected_timer ||
                                                    (timer == selected_timer &&
                                                        channel < selected_channel)))))))));
                if (better) {
                    found = true;
                    selected_mode = mode;
                    selected_channel = channel;
                    selected_timer = timer;
                    selected_config = config;
                    selected_reuses_group = reuses_group;
                    configure_selected = config_is_new || (!retained_timer.configured &&
                                                              !planned_timers[mode][timer]);
                    selected_error_num = error_num;
                    selected_error_den = error_den;
                }
            }
        }
        if (!found) {
            results[index].result = PwmResult::resource_unavailable;
            continue;
        }

        planned_channels[selected_mode][selected_channel] = true;
        planned_timers[selected_mode][selected_timer] = true;
        planned_config[selected_mode][selected_timer] = selected_config;
        if (configure_selected) {
            state.plan->configure_timer[selected_mode][selected_timer] = true;
            state.plan->timers[selected_mode][selected_timer] = selected_config;
        }
        auto& entry = state.plan->entries[state.plan->count++];
        entry = {requests[index], handles[index],
            {selected_config.effective, selected_config.resolution}, requests[index].initial_duty,
            selected_mode, selected_channel, selected_timer, false, false};
        results[index].result = PwmResult::success;
        results[index].effective = entry.effective;
        results[index].failure_domain = timer_domain(selected_mode, selected_timer);
    }
    state.plan->valid = true;
    return PwmResult::success;
}

/** @brief Applies one set of LEDC timer/channel claims with readback. */
PwmResult activate_native(void* const opaque, const PwmPlanToken token, const bool reconfiguration,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendOperationResult* const results) {
    auto& state = *static_cast<NativeState*>(opaque);
    if (!state.plan || !state.plan->valid || state.plan->token.slot != token.slot ||
        state.plan->token.generation != token.generation ||
        state.plan->reconfiguration != reconfiguration || state.plan->count != count ||
        count > kMaximumPwmOutputs || requests == nullptr || handles == nullptr ||
        results == nullptr) {
        return PwmResult::stale_plan;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& entry = state.plan->entries[index];
        if (entry.request.logical_output.owner != requests[index].logical_output.owner ||
            entry.request.logical_output.instance != requests[index].logical_output.instance ||
            entry.handle.slot != handles[index].slot ||
            entry.handle.generation != handles[index].generation) {
            return PwmResult::stale_plan;
        }
        results[index] = {};
        if (state.timers[entry.mode][entry.timer].quarantined) {
            state.plan.reset();
            return PwmResult::quarantined;
        }
    }

    for (std::uint8_t mode = 0U; mode < kModeCount; ++mode) {
        for (std::uint8_t timer = 0U; timer < kTimerCount; ++timer) {
            if (!state.plan->configure_timer[mode][timer]) {
                continue;
            }
            bool safe = true;
            if (state.plan->reconfiguration && state.timers[mode][timer].active_mask != 0U) {
                for (std::size_t index = 0U; index < state.plan->count; ++index) {
                    const auto& entry = state.plan->entries[index];
                    if (entry.mode == mode && entry.timer == timer) {
                        const auto channel_result = ledc_stop(static_cast<ledc_mode_t>(mode),
                            static_cast<ledc_channel_t>(entry.channel), 0U);
                        safe = channel_result == ESP_OK &&
                               safe_gpio(entry.request.pin.value(), entry.request.inverted) && safe;
                    }
                }
            }
            const auto& config = state.plan->timers[mode][timer];
            ledc_timer_config_t timer_config{};
            timer_config.speed_mode = static_cast<ledc_mode_t>(mode);
            timer_config.duty_resolution = static_cast<ledc_timer_bit_t>(1U);
            timer_config.timer_num = static_cast<ledc_timer_t>(timer);
            timer_config.freq_hz = config.bootstrap_hz;
            timer_config.clk_cfg = kClockSource;
            timer_config.deconfigure = false;
            bool programmed = safe && ledc_timer_config(&timer_config) == ESP_OK;
            if (programmed) {
                // IDF 6.1 removes ledc_timer_set(); its branch applies the
                // exact rational plan through HAL setters and verifies readback.
                programmed = program_timer(mode, timer, config);
            }
            if (!programmed) {
                const auto domain = timer_domain(mode, timer);
                if (state.plan->reconfiguration && state.timers[mode][timer].active_mask != 0U) {
                    quarantine_timer_plan(state, mode, timer, results);
                } else {
                    state.timers[mode][timer].quarantined = true;
                    for (std::size_t index = 0U; index < state.plan->count; ++index) {
                        const auto& entry = state.plan->entries[index];
                        if (entry.mode == mode && entry.timer == timer) {
                            results[index] = {PwmApplyState::failed, PwmResult::native_failure,
                                PwmOutputState::active, domain};
                        }
                    }
                }
                continue;
            }
            auto& retained = state.timers[mode][timer];
            retained.configured = true;
            retained.config = config;
        }
    }

    for (std::size_t index = 0U; index < state.plan->count; ++index) {
        const auto& entry = state.plan->entries[index];
        if (results[index].applied != PwmApplyState::not_applied) {
            continue;
        }
        const auto& timing = state.timers[entry.mode][entry.timer].config;
        const auto duty = entry.reconfiguration ? entry.old_duty : entry.request.initial_duty;
        const bool programmed = configure_channel(entry, duty, timing);
        if (!programmed) {
            const bool stopped = ledc_stop(static_cast<ledc_mode_t>(entry.mode),
                                     static_cast<ledc_channel_t>(entry.channel), 0U) == ESP_OK;
            const bool safe = safe_gpio(entry.request.pin.value(), entry.request.inverted);
            if (entry.reconfiguration || !stopped || !safe) {
                quarantine_timer_plan(state, entry.mode, entry.timer, results);
                continue;
            }
            results[index] = {PwmApplyState::failed, PwmResult::native_failure,
                PwmOutputState::active, timer_domain(entry.mode, entry.timer)};
            continue;
        }
        auto& retained = state.channels[entry.mode][entry.channel];
        retained.active = true;
        retained.handle = entry.handle;
        retained.request = entry.request;
        retained.duty = duty;
        retained.timer = entry.timer;
        retained.effective = entry.effective;
        state.timers[entry.mode][entry.timer].active_mask |=
            static_cast<std::uint8_t>(std::uint8_t{1U} << entry.channel);
        results[index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            timer_domain(entry.mode, entry.timer)};
    }
    state.plan.reset();
    return PwmResult::success;
}

/** @brief Applies changed duties and confirms each LEDC channel register. */
PwmResult write_native(void* const opaque, const PwmDutyUpdate* const updates,
    const std::size_t count, PwmBackendOperationResult* const results) {
    auto& state = *static_cast<NativeState*>(opaque);
    if (updates == nullptr || results == nullptr || count == 0U || count > kMaximumPwmOutputs) {
        return PwmResult::invalid_request;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        bool found = false;
        for (std::uint8_t mode = 0U; mode < kModeCount; ++mode) {
            for (std::uint8_t channel = 0U; channel < kChannelCount; ++channel) {
                const auto& output = state.channels[mode][channel];
                if (output.active && output.handle.slot == updates[index].handle.slot &&
                    output.handle.generation == updates[index].handle.generation) {
                    const auto timing = state.timers[mode][output.timer].config;
                    const auto native = native_duty(updates[index].duty, timing.resolution);
                    const auto set = ledc_set_duty(static_cast<ledc_mode_t>(mode),
                        static_cast<ledc_channel_t>(channel), native);
                    const auto update = set == ESP_OK
                                            ? ledc_update_duty(static_cast<ledc_mode_t>(mode),
                                                  static_cast<ledc_channel_t>(channel))
                                            : set;
                    const bool confirmed =
                        update == ESP_OK && duty_register_matches(mode, channel, native);
                    const auto domain = timer_domain(mode, output.timer);
                    results[index] = {confirmed ? PwmApplyState::applied : PwmApplyState::unknown,
                        confirmed ? PwmResult::success : PwmResult::quarantined,
                        confirmed ? PwmOutputState::active : PwmOutputState::quarantined, domain};
                    if (confirmed) {
                        auto& mutable_output = state.channels[mode][channel];
                        mutable_output.duty = updates[index].duty;
                    } else {
                        state.timers[mode][output.timer].quarantined = true;
                        state.plan.reset();
                    }
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            results[index] = {
                PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        }
    }
    return PwmResult::success;
}

/** @brief Stops one channel while keeping its configured timer reusable. */
PwmResult release_native(
    void* const opaque, const PwmBackendActiveOutput& output, PwmBackendOperationResult& result) {
    auto& state = *static_cast<NativeState*>(opaque);
    for (std::uint8_t mode = 0U; mode < kModeCount; ++mode) {
        for (std::uint8_t channel = 0U; channel < kChannelCount; ++channel) {
            auto& retained = state.channels[mode][channel];
            if (!retained.active || retained.handle.slot != output.handle.slot ||
                retained.handle.generation != output.handle.generation) {
                continue;
            }
            const auto domain = timer_domain(mode, retained.timer);
            const auto stopped =
                ledc_stop(static_cast<ledc_mode_t>(mode), static_cast<ledc_channel_t>(channel), 0U);
            const bool safe =
                stopped == ESP_OK && safe_gpio(output.request.pin.value(), output.request.inverted);
            if (!safe) {
                state.timers[mode][retained.timer].quarantined = true;
                result = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, domain};
                return PwmResult::success;
            }
            state.timers[mode][retained.timer].active_mask &=
                static_cast<std::uint8_t>(~(std::uint8_t{1U} << channel));
            if (state.timers[mode][retained.timer].active_mask == 0U) {
                TimerConfig verified{};
                if (read_timer(mode, retained.timer, verified)) {
                    state.timers[mode][retained.timer].config = verified;
                    state.timers[mode][retained.timer].configured = true;
                    state.timers[mode][retained.timer].quarantined = false;
                }
            }
            retained = ChannelState{};
            result = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active, domain};
            return PwmResult::success;
        }
    }
    result = {PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
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

/** @brief Returns the ESP-IDF LEDC backend. */
const PwmBackendOps& pwm_native_backend() {
    return kNativeBackend;
}

/** @brief Returns the bounded LEDC timer and channel tables. */
void* pwm_native_context() {
    return &g_native_state;
}

} // namespace newmota::platform::detail
