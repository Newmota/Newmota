/*
  pwm.cpp - AmebaD RTL8720DN TIM5 PWM adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/gpio.h"
#include "newmota/platform/pwm_detail.h"

extern "C" {
#include "ameba_soc.h"
}

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>

namespace newmota::platform::detail {
namespace {

constexpr std::uint64_t kTimerClockHz = 40'000'000U;
constexpr std::uint64_t kPpm = 1'000'000U;
constexpr std::uint32_t kMinimumIntervals = 2U;
constexpr std::uint32_t kMaximumIntervals = 65'535U;
constexpr std::uint16_t kTimerFailureDomain = 1U;
constexpr std::size_t kTimerChannelCount = 18U;
constexpr std::uint32_t kChannelEnableMask = TIM_CCER_CCxE;
constexpr std::uint32_t kChannelConfigurationMask =
    TIM_OCPreload_Enable | TIM_CCPolarity_Low | TIM_CCMode_Inputcapture;
constexpr std::uint32_t kTimerRunningMask = TIM_CR_CNT_START | TIM_CR_CNT_RUN | TIM_CR_CNT_STS;

struct Route {
    std::uint16_t pin{};
    std::uint8_t channel{};
};

// BW16 exposes these five PA pads. Use the KM4/TIM5 route consistently so
// GPIO13 and GPIO30 do not alias through the KM0 channel map.
constexpr Route kRoutes[] = {{12U, 0U}, {13U, 1U}, {25U, 4U}, {26U, 5U}, {30U, 7U}};

struct NativeOutput {
    bool used{};
    PwmHandle handle{};
    PwmRequest request{};
    PwmEffectiveSettings effective{};
    std::uint16_t applied_duty{};
    std::uint8_t channel{};
};

struct PlanEntry {
    PwmRequest request{};
    PwmHandle handle{};
    PwmEffectiveSettings effective{};
    std::uint16_t old_duty{};
    std::uint8_t channel{};
    bool existing{};
};

struct NativePlan {
    bool valid{};
    bool reconfiguration{};
    PwmPlanToken token{};
    std::uint32_t intervals{};
    std::size_t count{};
    PlanEntry entries[kMaximumPwmOutputs]{};
};

struct NativeState {
    bool timer_owned{};
    bool timer_configured{};
    bool quarantined{};
    std::uint32_t intervals{};
    NativeOutput outputs[kTimerChannelCount]{};
};

NativeState g_state{};
// Exists from prepare until activation or discard.
std::unique_ptr<NativePlan> g_plan;

/** @brief Finds the selected BW16 KM4 route for a resolved physical pin. */
[[nodiscard]] bool route_for_pin(const PinId pin, std::uint8_t& channel) {
    for (const auto& route : kRoutes) {
        if (route.pin == pin.value()) {
            channel = route.channel;
            return true;
        }
    }
    return false;
}

/** @brief Finds one retained AmebaD channel allocation by stable handle. */
[[nodiscard]] NativeOutput* find_output(const PwmHandle handle) {
    for (auto& output : g_state.outputs) {
        if (output.used && output.handle.slot == handle.slot &&
            output.handle.generation == handle.generation) {
            return &output;
        }
    }
    return nullptr;
}

/** @brief Counts channel claims retained by this adapter. */
[[nodiscard]] std::size_t active_count() {
    std::size_t count = 0U;
    for (const auto& output : g_state.outputs) {
        if (output.used) {
            ++count;
        }
    }
    return count;
}

/** @brief Returns TIM5's exact effective carrier and usable resolution. */
[[nodiscard]] PwmEffectiveSettings effective_settings(const std::uint32_t intervals) {
    return {{kTimerClockHz, intervals}, pwm_resolution_bits(intervals)};
}

/** @brief Checks the exact interval range accepted by one frequency request. */
[[nodiscard]] bool allowed_interval_range(
    const PwmRequest& request, std::uint64_t& minimum, std::uint64_t& maximum) {
    if (request.frequency_hz == 0U || request.max_error_ppm >= kPpm) {
        return false;
    }
    const auto scaled_clock = kTimerClockHz * kPpm;
    const auto lower_frequency =
        static_cast<std::uint64_t>(request.frequency_hz) * (kPpm + request.max_error_ppm);
    const auto upper_frequency =
        static_cast<std::uint64_t>(request.frequency_hz) * (kPpm - request.max_error_ppm);
    if (lower_frequency == 0U || upper_frequency == 0U) {
        return false;
    }
    minimum = scaled_clock / lower_frequency;
    if (scaled_clock % lower_frequency != 0U) {
        ++minimum;
    }
    maximum = scaled_clock / upper_frequency;
    return true;
}

/** @brief Picks the most accurate fixed TIM5 period satisfying a set. */
[[nodiscard]] bool plan_intervals(
    const PwmRequest* requests, const std::size_t count, std::uint32_t& intervals) {
    if (requests == nullptr || count == 0U) {
        return false;
    }
    std::uint64_t lower = kMinimumIntervals;
    std::uint64_t upper = kMaximumIntervals;
    std::uint32_t minimum_target = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_target = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const auto& request = requests[index];
        if (request.frequency_hz < minimum_target) {
            minimum_target = request.frequency_hz;
        }
        if (request.frequency_hz > maximum_target) {
            maximum_target = request.frequency_hz;
        }
        if (request.min_resolution_bits > 16U) {
            return false;
        }
        const auto resolution_floor =
            request.min_resolution_bits == 0U
                ? 1U
                : ((std::uint32_t{1U} << request.min_resolution_bits) - 1U);
        if (resolution_floor > lower) {
            lower = resolution_floor;
        }
        std::uint64_t request_lower = 0U;
        std::uint64_t request_upper = 0U;
        if (!allowed_interval_range(request, request_lower, request_upper)) {
            return false;
        }
        if (request_lower > lower) {
            lower = request_lower;
        }
        if (request_upper < upper) {
            upper = request_upper;
        }
    }
    if (lower > upper || lower > kMaximumIntervals) {
        return false;
    }
    bool found = false;
    std::uint64_t best_error_numerator = 0U;
    std::uint64_t best_error_denominator = 1U;
    std::uint8_t best_resolution = 0U;
    std::uint32_t selected = 0U;
    const auto center_numerator = static_cast<std::uint64_t>(kTimerClockHz) *
                                  (static_cast<std::uint64_t>(minimum_target) + maximum_target);
    const auto center_denominator =
        static_cast<std::uint64_t>(2U) * minimum_target * maximum_target;
    if (center_denominator == 0U) {
        return false;
    }
    const auto center = center_numerator / center_denominator;
    const auto consider = [&](const std::uint64_t candidate) {
        if (candidate < lower || candidate > upper || candidate > kMaximumIntervals) {
            return;
        }
        const auto effective = effective_settings(static_cast<std::uint32_t>(candidate));
        bool accepted = true;
        std::uint64_t worst_error_numerator = 0U;
        std::uint64_t worst_error_denominator = 1U;
        for (std::size_t index = 0U; index < count; ++index) {
            if (!pwm_frequency_meets_request(effective.frequency, requests[index]) ||
                effective.resolution_bits < requests[index].min_resolution_bits) {
                accepted = false;
                break;
            }
            const auto target = static_cast<std::uint64_t>(requests[index].frequency_hz) *
                                effective.frequency.denominator;
            const auto error = effective.frequency.numerator_hz > target
                                   ? effective.frequency.numerator_hz - target
                                   : target - effective.frequency.numerator_hz;
            if (compare_fractions(error, target, worst_error_numerator, worst_error_denominator) >
                0) {
                worst_error_numerator = error;
                worst_error_denominator = target;
            }
        }
        if (!accepted) {
            return;
        }
        const auto error_order = compare_fractions(worst_error_numerator, worst_error_denominator,
            best_error_numerator, best_error_denominator);
        const bool better =
            !found || error_order < 0 ||
            (error_order == 0 &&
                (effective.resolution_bits > best_resolution ||
                    (effective.resolution_bits == best_resolution && candidate > selected)));
        if (better) {
            found = true;
            selected = static_cast<std::uint32_t>(candidate);
            best_resolution = effective.resolution_bits;
            best_error_numerator = worst_error_numerator;
            best_error_denominator = worst_error_denominator;
        }
    };
    consider(lower);
    consider(upper);
    for (std::int8_t adjustment = -1; adjustment <= 2; ++adjustment) {
        if (adjustment < 0 && center < static_cast<std::uint64_t>(-adjustment)) {
            continue;
        }
        consider(static_cast<std::uint64_t>(static_cast<std::int64_t>(center) + adjustment));
    }
    if (!found) {
        return false;
    }
    intervals = selected;
    return true;
}

/** @brief Confirms whether TIM5 is currently enabled or still running. */
[[nodiscard]] bool timer_running() {
    return (TIM5->EN & kTimerRunningMask) != 0U;
}

/** @brief Confirms a pad is electrically safe GPIO at the requested level. */
[[nodiscard]] bool set_safe_gpio(const PwmRequest& request) {
    const auto pin = request.pin.value();
    if (gpio_configure_output(request.pin, request.inverted) != GpioResult::success) {
        return false;
    }
    return Pinmux_ConfigGet(static_cast<u8>(pin)) == PINMUX_FUNCTION_GPIO &&
           (GPIO_ReadDataBit(pin) != 0U) == request.inverted;
}

/** @brief Checks that an unused request pad is still owned by GPIO. */
[[nodiscard]] bool pad_available(const PinId pin) {
    return Pinmux_ConfigGet(static_cast<u8>(pin.value())) == PINMUX_FUNCTION_GPIO;
}

/** @brief Confirms there is no native TIM5 owner before first acquisition. */
[[nodiscard]] bool timer_available() {
    if (timer_running()) {
        return false;
    }
    for (std::size_t channel = 0U; channel < kTimerChannelCount; ++channel) {
        if ((TIM5->CCMRx[channel] & kChannelEnableMask) != 0U) {
            return false;
        }
    }
    return true;
}

/** @brief Validates all retained outputs against live TIM5 and pin-mux state. */
[[nodiscard]] bool retained_hardware_matches() {
    if (!g_state.timer_configured || g_state.intervals < kMinimumIntervals ||
        g_state.intervals > kMaximumIntervals || !timer_running() || TIM5->PSC != 0U ||
        TIM5->ARR != g_state.intervals - 1U) {
        return false;
    }
    bool claimed[kTimerChannelCount]{};
    for (const auto& output : g_state.outputs) {
        if (!output.used) {
            continue;
        }
        if (output.channel >= kTimerChannelCount) {
            return false;
        }
        claimed[output.channel] = true;
        const auto mode = output.request.inverted ? TIM_CCPolarity_Low : TIM_CCPolarity_High;
        const auto register_value = TIM5->CCMRx[output.channel];
        if ((register_value & kChannelEnableMask) == 0U ||
            (register_value & kChannelConfigurationMask) != (TIM_OCPreload_Enable | mode) ||
            (register_value & 0xffffU) !=
                pwm_duty_interval(output.applied_duty, g_state.intervals) ||
            Pinmux_ConfigGet(static_cast<u8>(output.request.pin.value())) !=
                PINMUX_FUNCTION_PWM_HS) {
            return false;
        }
    }
    for (std::size_t channel = 0U; channel < kTimerChannelCount; ++channel) {
        if (!claimed[channel] && (TIM5->CCMRx[channel] & kChannelEnableMask) != 0U) {
            return false;
        }
    }
    return true;
}

/** @brief Stops TIM5 and confirms that its counter is no longer running. */
[[nodiscard]] bool stop_timer() {
    RTIM_Cmd(TIM5, DISABLE);
    for (std::uint32_t wait = 0U; wait < 100'000U; ++wait) {
        if (!timer_running()) {
            return true;
        }
    }
    return false;
}

/** @brief Starts TIM5 and confirms that the counter entered its running state. */
[[nodiscard]] bool start_timer() {
    RTIM_Cmd(TIM5, ENABLE);
    for (std::uint32_t wait = 0U; wait < 100'000U; ++wait) {
        if (timer_running()) {
            return true;
        }
    }
    return false;
}

/** @brief Sets a channel's inactive compare, polarity, and preload mode. */
[[nodiscard]] bool configure_channel(
    const std::uint8_t channel, const bool inverted, const std::uint32_t compare) {
    if (channel >= kTimerChannelCount || compare > 0xffffU) {
        return false;
    }
    auto& value = TIM5->CCMRx[channel];
    value &= ~kChannelEnableMask;
    value = (value & ~(0x00ffffffU | kChannelConfigurationMask)) | compare | TIM_OCPreload_Enable |
            (inverted ? TIM_CCPolarity_Low : TIM_CCPolarity_High);
    const auto expected =
        compare | TIM_OCPreload_Enable | (inverted ? TIM_CCPolarity_Low : TIM_CCPolarity_High);
    return (TIM5->CCMRx[channel] & (0x00ffffffU | kChannelConfigurationMask)) == expected;
}

/** @brief Enables or disables one output compare channel with readback. */
[[nodiscard]] bool set_channel_enabled(const std::uint8_t channel, const bool enabled) {
    if (channel >= kTimerChannelCount) {
        return false;
    }
    RTIM_CCxCmd(TIM5, channel, enabled ? TIM_CCx_Enable : TIM_CCx_Disable);
    return ((TIM5->CCMRx[channel] & kChannelEnableMask) != 0U) == enabled;
}

/** @brief Applies one checked high-speed PWM pin mux selection. */
[[nodiscard]] bool set_pwm_mux(const PinId pin) {
    Pinmux_Config(static_cast<u8>(pin.value()), PINMUX_FUNCTION_PWM_HS);
    return Pinmux_ConfigGet(static_cast<u8>(pin.value())) == PINMUX_FUNCTION_PWM_HS;
}

/** @brief Configures the shared 40 MHz TIM5 timebase and verifies registers. */
[[nodiscard]] bool configure_timebase(const std::uint32_t intervals) {
    if (intervals < kMinimumIntervals || intervals > kMaximumIntervals) {
        return false;
    }
    RTIM_TimeBaseInitTypeDef configuration{};
    RTIM_TimeBaseStructInit(&configuration);
    configuration.TIM_Idx = 5U;
    configuration.TIM_Prescaler = 0U;
    configuration.TIM_Period = intervals - 1U;
    configuration.TIM_UpdateEvent = ENABLE;
    configuration.TIM_UpdateSource = TIM_UpdateSource_Global;
    configuration.TIM_ARRProtection = ENABLE;
    RTIM_TimeBaseInit(TIM5, &configuration, TIMER5_IRQ, nullptr, 0U);
    return TIM5->PSC == 0U && TIM5->ARR == intervals - 1U;
}

/** @brief Builds a normalized duty compare value including exact endpoints. */
[[nodiscard]] std::uint32_t compare_for_duty(
    const std::uint16_t duty, const std::uint32_t intervals) {
    return pwm_duty_interval(duty, intervals);
}

/** @brief Records one output after a fully confirmed native activation. */
void retain_output(const PlanEntry& entry, const std::uint16_t duty) {
    auto& output = g_state.outputs[entry.channel];
    output.used = true;
    output.handle = entry.handle;
    output.request = entry.request;
    output.effective = entry.effective;
    output.applied_duty = duty;
    output.channel = entry.channel;
}

/** @brief Retains an uncertain request so it can still be safely released. */
void retain_quarantined_plan() {
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        const auto& entry = g_plan->entries[index];
        auto& output = g_state.outputs[entry.channel];
        if (!output.used) {
            output.used = true;
            output.handle = entry.handle;
            output.request = entry.request;
            output.effective = entry.effective;
            output.channel = entry.channel;
        }
    }
    g_state.quarantined = true;
}

/** @brief Best-effort moves every claimed output to its inactive GPIO level. */
[[nodiscard]] bool make_domain_safe() {
    // Duty writes and releases call this without a plan.
    const std::size_t planned = g_plan ? g_plan->count : 0U;
    bool safe = true;
    for (auto& output : g_state.outputs) {
        if (!output.used) {
            continue;
        }
        const PwmRequest* request = &output.request;
        for (std::size_t index = 0U; index < planned; ++index) {
            if (g_plan->entries[index].channel == output.channel) {
                request = &g_plan->entries[index].request;
                break;
            }
        }
        safe = set_safe_gpio(*request) && safe;
        safe = set_channel_enabled(output.channel, false) && safe;
    }
    for (std::size_t index = 0U; index < planned; ++index) {
        const auto& entry = g_plan->entries[index];
        bool already_retained = false;
        for (const auto& output : g_state.outputs) {
            already_retained = already_retained || (output.used && output.channel == entry.channel);
        }
        if (!already_retained) {
            safe = set_safe_gpio(entry.request) && safe;
            safe = set_channel_enabled(entry.channel, false) && safe;
        }
    }
    return stop_timer() && safe;
}

/** @brief Initializes or reconfigures the complete TIM5 output group. */
[[nodiscard]] bool activate_complete_group() {
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        if (!set_safe_gpio(g_plan->entries[index].request)) {
            return false;
        }
    }
    if (!stop_timer()) {
        return false;
    }
    for (const auto& output : g_state.outputs) {
        if (output.used && !set_channel_enabled(output.channel, false)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        if (!set_channel_enabled(g_plan->entries[index].channel, false)) {
            return false;
        }
    }
    if (!configure_timebase(g_plan->intervals)) {
        return false;
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        const auto& entry = g_plan->entries[index];
        const auto duty = entry.existing ? entry.old_duty : entry.request.initial_duty;
        if (!configure_channel(
                entry.channel, entry.request.inverted, compare_for_duty(duty, g_plan->intervals))) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        if (!set_pwm_mux(g_plan->entries[index].request.pin)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        if (!set_channel_enabled(g_plan->entries[index].channel, true)) {
            return false;
        }
    }
    if (!start_timer() || TIM5->PSC != 0U || TIM5->ARR != g_plan->intervals - 1U) {
        return false;
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        const auto& entry = g_plan->entries[index];
        const auto duty = entry.existing ? entry.old_duty : entry.request.initial_duty;
        const auto expected = compare_for_duty(duty, g_plan->intervals) | TIM_OCPreload_Enable |
                              (entry.request.inverted ? TIM_CCPolarity_Low : TIM_CCPolarity_High);
        if ((TIM5->CCMRx[entry.channel] &
                (0x00ffffffU | kChannelConfigurationMask | kChannelEnableMask)) !=
                (expected | kChannelEnableMask) ||
            Pinmux_ConfigGet(static_cast<u8>(entry.request.pin.value())) !=
                PINMUX_FUNCTION_PWM_HS) {
            return false;
        }
    }
    g_state.timer_owned = true;
    g_state.timer_configured = true;
    g_state.intervals = g_plan->intervals;
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        const auto& entry = g_plan->entries[index];
        retain_output(entry, entry.existing ? entry.old_duty : entry.request.initial_duty);
    }
    return true;
}

/** @brief Adds outputs without changing the live shared timer period. */
[[nodiscard]] bool activate_additions() {
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        const auto& entry = g_plan->entries[index];
        if (!set_safe_gpio(entry.request)) {
            return false;
        }
        if (!configure_channel(entry.channel, entry.request.inverted,
                compare_for_duty(entry.request.initial_duty, g_plan->intervals))) {
            return false;
        }
        if (!set_pwm_mux(entry.request.pin) || !set_channel_enabled(entry.channel, true)) {
            return false;
        }
        const auto expected = compare_for_duty(entry.request.initial_duty, g_plan->intervals) |
                              TIM_OCPreload_Enable |
                              (entry.request.inverted ? TIM_CCPolarity_Low : TIM_CCPolarity_High) |
                              kChannelEnableMask;
        if ((TIM5->CCMRx[entry.channel] &
                (0x00ffffffU | kChannelConfigurationMask | kChannelEnableMask)) != expected ||
            Pinmux_ConfigGet(static_cast<u8>(entry.request.pin.value())) !=
                PINMUX_FUNCTION_PWM_HS) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < g_plan->count; ++index) {
        retain_output(g_plan->entries[index], g_plan->entries[index].request.initial_duty);
    }
    return true;
}

/** @brief Returns the PWM callbacks for the AmebaD adapter. */
[[nodiscard]] const PwmBackendOps& backend_ops();

/** @brief Plans a bounded request set against TIM5's shared timebase. */
PwmResult prepare(void*, const PwmPlanToken token, const bool reconfiguration,
    const PwmBackendActiveOutput* const active, const std::size_t active_outputs,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendPlanResult* const results) {
    g_plan.reset();
    if (!token.valid() || requests == nullptr || handles == nullptr || results == nullptr ||
        count == 0U || count > kMaximumPwmOutputs) {
        return PwmResult::invalid_request;
    }
    g_plan.reset(new (std::nothrow) NativePlan{});
    if (!g_plan) {
        return PwmResult::no_memory;
    }
    if (g_state.quarantined) {
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmResult::quarantined, {}, kTimerFailureDomain};
        }
        return PwmResult::quarantined;
    }
    const auto retained_count = active_count();
    if (retained_count != active_outputs) {
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmResult::ownership_conflict, {}, kTimerFailureDomain};
        }
        return PwmResult::ownership_conflict;
    }
    if (retained_count == 0U && !timer_available()) {
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmResult::resource_unavailable, {}, kTimerFailureDomain};
        }
        return PwmResult::resource_unavailable;
    }

    std::uint8_t channels[kMaximumPwmOutputs]{};
    bool valid[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {PwmResult::success, {}, kTimerFailureDomain};
        if (!route_for_pin(requests[index].pin, channels[index])) {
            results[index].result = PwmResult::unsupported_route;
            continue;
        }
        if (!reconfiguration && g_state.outputs[channels[index]].used) {
            results[index].result = PwmResult::ownership_conflict;
            continue;
        }
        if (!reconfiguration && !pad_available(requests[index].pin)) {
            results[index].result = PwmResult::ownership_conflict;
            continue;
        }
        valid[index] = true;
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (valid[prior] && channels[prior] == channels[index]) {
                results[index].result = PwmResult::ownership_conflict;
                results[prior].result = PwmResult::ownership_conflict;
                valid[index] = false;
                valid[prior] = false;
            }
        }
    }

    if (reconfiguration) {
        bool group_valid = active != nullptr && retained_count != 0U && count == retained_count;
        for (std::size_t index = 0U; index < count; ++index) {
            group_valid = group_valid && valid[index];
        }
        if (!group_valid) {
            for (std::size_t index = 0U; index < count; ++index) {
                if (results[index].result == PwmResult::success) {
                    results[index].result = PwmResult::incompatible;
                }
            }
            return PwmResult::success;
        }
        for (std::size_t output_index = 0U; output_index < kTimerChannelCount; ++output_index) {
            const auto& output = g_state.outputs[output_index];
            if (!output.used) {
                continue;
            }
            bool participant = false;
            for (std::size_t index = 0U; index < count; ++index) {
                if (handles[index].slot == output.handle.slot &&
                    handles[index].generation == output.handle.generation) {
                    participant = requests[index].pin.value() == output.request.pin.value() &&
                                  channels[index] == output.channel;
                    break;
                }
            }
            if (!participant) {
                for (std::size_t index = 0U; index < count; ++index) {
                    results[index].result = PwmResult::incompatible;
                }
                return PwmResult::success;
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            bool found = false;
            for (std::size_t active_index = 0U; active_index < active_outputs; ++active_index) {
                const auto& current = active[active_index];
                if (current.handle.slot == handles[index].slot &&
                    current.handle.generation == handles[index].generation) {
                    found = current.state == PwmOutputState::active &&
                            current.failure_domain == kTimerFailureDomain;
                    break;
                }
            }
            if (!found || find_output(handles[index]) == nullptr) {
                for (std::size_t result_index = 0U; result_index < count; ++result_index) {
                    results[result_index].result = PwmResult::ownership_conflict;
                }
                return PwmResult::success;
            }
        }
        if (!plan_intervals(requests, count, g_plan->intervals)) {
            for (std::size_t index = 0U; index < count; ++index) {
                results[index].result = PwmResult::requirements_unsatisfied;
            }
            g_plan.reset();
            return PwmResult::success;
        }
    } else if (retained_count == 0U) {
        PwmRequest accepted[kMaximumPwmOutputs]{};
        std::size_t accepted_count = 0U;
        for (std::size_t index = 0U; index < count; ++index) {
            if (!valid[index]) {
                continue;
            }
            accepted[accepted_count] = requests[index];
            std::uint32_t trial_intervals = 0U;
            if (!plan_intervals(accepted, accepted_count + 1U, trial_intervals)) {
                results[index].result = PwmResult::requirements_unsatisfied;
                valid[index] = false;
                continue;
            }
            ++accepted_count;
            g_plan->intervals = trial_intervals;
        }
        if (accepted_count == 0U) {
            return PwmResult::success;
        }
    } else {
        g_plan->intervals = g_state.intervals;
        const auto effective = effective_settings(g_state.intervals);
        for (std::size_t index = 0U; index < count; ++index) {
            if (valid[index] &&
                (!pwm_frequency_meets_request(effective.frequency, requests[index]) ||
                    effective.resolution_bits < requests[index].min_resolution_bits)) {
                results[index].result = PwmResult::requirements_unsatisfied;
                valid[index] = false;
            }
        }
    }

    const auto effective = effective_settings(g_plan->intervals);
    g_plan->valid = true;
    g_plan->reconfiguration = reconfiguration;
    g_plan->token = token;
    g_plan->count = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        if (!valid[index]) {
            continue;
        }
        const auto* const existing = reconfiguration ? find_output(handles[index]) : nullptr;
        g_plan->entries[g_plan->count++] = {requests[index], handles[index], effective,
            existing == nullptr ? std::uint16_t{0U} : existing->applied_duty, channels[index],
            existing != nullptr};
        results[index].effective = effective;
    }
    g_plan->valid = g_plan->count > 0U;
    return PwmResult::success;
}

/** @brief Applies a prepared complete-group retune or additions-only batch. */
PwmResult activate(void*, const PwmPlanToken token, const bool reconfiguration,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendOperationResult* const results) {
    if (results == nullptr || requests == nullptr || handles == nullptr || !g_plan ||
        !g_plan->valid || g_plan->token.slot != token.slot ||
        g_plan->token.generation != token.generation ||
        g_plan->reconfiguration != reconfiguration || g_plan->count != count) {
        return PwmResult::stale_plan;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        results[index] = {PwmApplyState::not_applied, PwmResult::native_failure,
            PwmOutputState::active, kTimerFailureDomain};
        const auto& entry = g_plan->entries[index];
        if (entry.handle.slot != handles[index].slot ||
            entry.handle.generation != handles[index].generation ||
            entry.request.pin.value() != requests[index].pin.value() ||
            entry.request.frequency_hz != requests[index].frequency_hz ||
            entry.request.max_error_ppm != requests[index].max_error_ppm ||
            entry.request.min_resolution_bits != requests[index].min_resolution_bits ||
            entry.request.inverted != requests[index].inverted ||
            entry.request.initial_duty != requests[index].initial_duty) {
            return PwmResult::stale_plan;
        }
    }

    const auto was_empty = active_count() == 0U;
    bool success = false;
    if (reconfiguration) {
        g_state.timer_owned = true;
        success = retained_hardware_matches() && activate_complete_group();
    } else if (was_empty) {
        g_state.timer_owned = true;
        success = activate_complete_group();
    } else if (retained_hardware_matches()) {
        success = activate_additions();
    }
    if (success) {
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
                kTimerFailureDomain};
        }
        g_plan.reset();
        return PwmResult::success;
    }

    // Once a pad or timer register may have changed, shut down and quarantine
    // the one shared TIM5 failure domain. The common manager retains all
    // affected handles so callers can release them safely.
    if (g_state.timer_owned) {
        (void)make_domain_safe();
        retain_quarantined_plan();
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmApplyState::unknown, PwmResult::quarantined,
                PwmOutputState::quarantined, kTimerFailureDomain};
        }
        g_plan.reset();
        return PwmResult::success;
    }
    g_plan.reset();
    return PwmResult::native_failure;
}

/** @brief Writes duty compares that latch together at the next TIM5 period. */
PwmResult write_batch(void*, const PwmDutyUpdate* const updates, const std::size_t count,
    PwmBackendOperationResult* const results) {
    if (updates == nullptr || results == nullptr || count == 0U || count > kMaximumPwmOutputs) {
        return PwmResult::invalid_request;
    }
    if (g_state.quarantined || !retained_hardware_matches()) {
        g_state.quarantined = true;
        (void)make_domain_safe();
        g_plan.reset();
        for (std::size_t index = 0U; index < count; ++index) {
            results[index] = {PwmApplyState::unknown, PwmResult::quarantined,
                PwmOutputState::quarantined, kTimerFailureDomain};
        }
        return PwmResult::success;
    }
    if (g_plan && g_plan->reconfiguration) {
        g_plan.reset();
    }
    NativeOutput* batch_outputs[kMaximumPwmOutputs]{};
    for (std::size_t index = 0U; index < count; ++index) {
        batch_outputs[index] = find_output(updates[index].handle);
        if (batch_outputs[index] == nullptr) {
            return PwmResult::stale_handle;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (batch_outputs[prior]->channel == batch_outputs[index]->channel) {
                return PwmResult::duplicate_handle;
            }
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        auto* const output = batch_outputs[index];
        const auto compare = compare_for_duty(updates[index].duty, g_state.intervals);
        RTIM_CCRxSet(TIM5, compare, output->channel);
        if ((TIM5->CCMRx[output->channel] & 0xffffU) != compare) {
            g_state.quarantined = true;
            (void)make_domain_safe();
            g_plan.reset();
            for (std::size_t result_index = 0U; result_index < count; ++result_index) {
                results[result_index] = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, kTimerFailureDomain};
            }
            return PwmResult::success;
        }
    }
    for (std::size_t index = 0U; index < count; ++index) {
        auto* const output = find_output(updates[index].handle);
        output->applied_duty = updates[index].duty;
        output->request.initial_duty = updates[index].duty;
        results[index] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            kTimerFailureDomain};
    }
    return PwmResult::success;
}

/** @brief Safely returns one claimed pad to GPIO and disables its channel. */
PwmResult release(void*, const PwmBackendActiveOutput& output, PwmBackendOperationResult& result) {
    auto* const native = find_output(output.handle);
    if (native == nullptr) {
        result = {
            PwmApplyState::not_applied, PwmResult::stale_handle, output.state, kTimerFailureDomain};
        return PwmResult::stale_handle;
    }
    g_plan.reset();
    const auto safe_pad = set_safe_gpio(native->request);
    const auto disabled = set_channel_enabled(native->channel, false);
    if (!safe_pad || !disabled) {
        g_state.quarantined = true;
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
            kTimerFailureDomain};
        return PwmResult::success;
    }
    const auto last_output = active_count() == 1U;
    if (last_output) {
        if (!stop_timer()) {
            g_state.quarantined = true;
            result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
                kTimerFailureDomain};
            return PwmResult::success;
        }
    }
    native->used = false;
    if (last_output) {
        g_state.quarantined = false;
        g_state.timer_configured = false;
        g_state.intervals = 0U;
    }
    result = {
        PwmApplyState::applied, PwmResult::success, PwmOutputState::active, kTimerFailureDomain};
    return PwmResult::success;
}

/** @brief Frees a plan the controller will not activate. */
void discard(void*) {
    g_plan.reset();
}

const PwmBackendOps& backend_ops() {
    static const PwmBackendOps operations{prepare, activate, write_batch, release, discard};
    return operations;
}

} // namespace

/** @brief Returns the checked TIM5 PWM backend for AmebaD firmware. */
const PwmBackendOps& pwm_native_backend() {
    return backend_ops();
}

/** @brief Returns retained TIM5 adapter state for AmebaD firmware. */
void* pwm_native_context() {
    return &g_state;
}

} // namespace newmota::platform::detail
