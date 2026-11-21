/*
  pwm.cpp - Beken PWM adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/pwm_detail.h"

extern "C" {
#include "include.h"
#include "arm_arch.h"
#include "gpio_pub.h"
#include "gpio.h"
#include "pwm_pub.h"
// BK7231T (SOC_BK7231U) and BK7252 (SOC_BK7221U) keep the older pwm.c driver:
// one independent unit per channel, reached through sddev_control.
#if (CFG_SOC_NAME == SOC_BK7231U) || (CFG_SOC_NAME == SOC_BK7221U)
#define NEWMOTA_BEKEN_PWM_UNITS 1
#else
#define NEWMOTA_BEKEN_PWM_UNITS 0
#endif
#if NEWMOTA_BEKEN_PWM_UNITS
#include "pwm.h"
#else
#include "pwm_bk7231n.h"
#endif
}

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace newmota::platform::detail {
namespace {

constexpr std::size_t kChannelCount = PWM_COUNT;
constexpr std::size_t kGroupCount =
#if NEWMOTA_BEKEN_PWM_UNITS
    PWM_COUNT;
#else
    PWM_GROUP_NUM;
#endif
constexpr std::uint32_t kPwmClockHz = 26'000'000U;
constexpr std::uint32_t kMaximumPeriod = kPwmClockHz;
#if NEWMOTA_BEKEN_PWM_UNITS
char kPwmDeviceName[] = "pwm";
#endif

struct NativeOutput {
    bool used{};
    bool quarantined{};
    PwmHandle handle{};
    PwmRequest request{};
    PwmEffectiveSettings effective{};
    std::uint16_t duty{};
};

struct Timing {
    bool valid{};
    std::uint32_t period{};
    std::uint8_t resolution{};
    PwmFrequency frequency{};
    std::uint64_t error_numerator{};
    std::uint64_t error_denominator{1U};
};

struct PlanEntry {
    bool valid{};
    bool reconfiguration{};
    std::uint8_t channel{};
    PwmRequest request{};
    PwmHandle handle{};
    Timing timing{};
    std::uint16_t retained_duty{};
};

struct NativePlan {
    bool valid{};
    bool reconfiguration{};
    PwmPlanToken token{};
    std::size_t count{};
    PlanEntry entries[kMaximumPwmOutputs]{};
};

struct NativeState {
    NativeOutput outputs[kChannelCount]{};
};

NativeState g_native{};
// Exists from prepare until activation or discard.
std::unique_ptr<NativePlan> g_plan;

[[nodiscard]] std::uint8_t group_for_channel(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return channel;
#else
    return static_cast<std::uint8_t>(channel / PWM_CHAN_IN_GROUP);
#endif
}

#if !NEWMOTA_BEKEN_PWM_UNITS
[[nodiscard]] std::uint8_t sibling_for_channel(const std::uint8_t channel) {
    return static_cast<std::uint8_t>(channel ^ 1U);
}
#endif

[[nodiscard]] std::uint16_t failure_domain(const std::uint8_t channel) {
    return static_cast<std::uint16_t>(group_for_channel(channel) + 1U);
}

[[nodiscard]] bool route_for_pin(const PinId pin, std::uint8_t& channel) {
    switch (pin.value()) {
    case 6U:
        channel = PWM0;
        return true;
    case 7U:
        channel = PWM1;
        return true;
    case 8U:
        channel = PWM2;
        return true;
    case 9U:
        channel = PWM3;
        return true;
    case 24U:
        channel = PWM4;
        return true;
    case 26U:
        channel = PWM5;
        return true;
    default:
        return false;
    }
}

void consider_period(const PwmRequest& request, const std::uint32_t period, Timing& selected) {
    if (period < 2U || period > kMaximumPeriod) {
        return;
    }
    const PwmFrequency frequency{kPwmClockHz, period};
    if (!pwm_frequency_meets_request(frequency, request)) {
        return;
    }
    const auto target = static_cast<std::uint64_t>(request.frequency_hz) * period;
    const auto error = kPwmClockHz > target ? kPwmClockHz - target : target - kPwmClockHz;
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
        selected = {true, period, resolution, frequency, error, target};
    }
}

/** @brief Selects the most accurate accepted 26 MHz period. */
[[nodiscard]] bool plan_timing(const PwmRequest& request, Timing& selected) {
    const auto ideal = static_cast<std::uint64_t>(kPwmClockHz) / request.frequency_hz;
    const auto resolution_floor = pwm_minimum_intervals(request.min_resolution_bits);
    const auto minimum_period = resolution_floor < 2U ? 2U : resolution_floor;
    if (ideal < 2U) {
        consider_period(request, minimum_period, selected);
        return selected.valid;
    }
    if (ideal > kMaximumPeriod) {
        consider_period(request, kMaximumPeriod, selected);
        return selected.valid;
    }
    if (ideal < minimum_period) {
        consider_period(request, minimum_period, selected);
        return selected.valid;
    }
    consider_period(request, static_cast<std::uint32_t>(ideal), selected);
    if (ideal < kMaximumPeriod) {
        consider_period(request, static_cast<std::uint32_t>(ideal + 1U), selected);
    }
    return selected.valid;
}

[[nodiscard]] std::uint32_t group_control(const std::uint8_t group) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return REG_READ(PWM_CTL) >> (group * 4U);
#else
    return REG_READ(REG_PWM_GROUP_CTRL_ADDR(group));
#endif
}

[[nodiscard]] std::uint32_t channel_t1(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return REG_READ(REG_APB_BK_PWMn_DC_ADDR(channel));
#else
    const auto group = group_for_channel(channel);
    return (channel & 1U) == 0U ? REG_READ(REG_GROUP_PWM0_T1_ADDR(group))
                                : REG_READ(REG_GROUP_PWM1_T1_ADDR(group));
#endif
}

[[nodiscard]] std::uint32_t channel_t4(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return REG_READ(REG_APB_BK_PWMn_END_ADDR(channel));
#else
    const auto group = group_for_channel(channel);
    return (channel & 1U) == 0U ? REG_READ(REG_GROUP_PWM0_T4_ADDR(group))
                                : REG_READ(REG_GROUP_PWM1_T4_ADDR(group));
#endif
}

[[nodiscard]] std::uint32_t pin_mux_mode(const std::uint8_t pin) {
    // Every BDK target writes two mux bits per pad; all PWM pads are below GPIO32.
    const auto address = pin < 16U ? REG_GPIO_FUNC_CFG : REG_GPIO_FUNC_CFG_2;
    const auto bit = pin < 16U ? pin : pin - 16U;
    return (REG_READ(address) >> (bit * 2U)) & 0x3U;
}

[[nodiscard]] std::uint32_t gpio_config_register(const std::uint8_t pin) {
    return REG_READ(REG_GPIO_X_CONGFIG_ADDR(pin));
}

[[nodiscard]] std::uint8_t second_function_mode(const std::uint8_t channel) {
    // The modes gpio_enable_second_function selects for GFUNC_MODE_PWM0..5.
#if (CFG_SOC_NAME == SOC_BK7238)
    return (channel == PWM0 || channel == PWM4) ? PERIAL_MODE_2 : PERIAL_MODE_1;
#else
    (void)channel;
    return PERIAL_MODE_2;
#endif
}

[[nodiscard]] bool set_safe_gpio(const PwmRequest& request) {
    const auto pin = static_cast<std::uint8_t>(request.pin.value());
    // GPIO mode and output latch share one register. gpio_config(GMODE_OUTPUT)
    // writes zero to the whole register, so compose the safe level and GPIO
    // output mode in one write instead of briefly clearing an inverted latch.
    REG_WRITE(REG_GPIO_X_CONGFIG_ADDR(pin), request.inverted ? GCFG_OUTPUT_BIT : 0U);
    const auto config = gpio_config_register(pin);
    UINT32 output = 0U;
    gpio_get_output(pin, &output);
    return (config & GCFG_FUNCTION_ENABLE_BIT) == 0U && (config & 0x0cU) == 0U &&
           (output & 1U) == static_cast<std::uint32_t>(request.inverted);
}

[[nodiscard]] bool pin_is_routed_to_pwm(const PlanEntry& entry) {
    const auto pin = static_cast<std::uint8_t>(entry.request.pin.value());
    const auto config = gpio_config_register(pin);
    return pin_mux_mode(pin) == second_function_mode(entry.channel) &&
           (config & GCFG_FUNCTION_ENABLE_BIT) != 0U;
}

[[nodiscard]] bool sibling_pair_idle(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    (void)channel;
    return true;
#else
    const auto group = group_for_channel(channel);
    const auto sibling = sibling_for_channel(channel);
    const auto control = group_control(group);
    const auto sibling_index = static_cast<std::uint8_t>(sibling & 1U);
    const auto sibling_enabled = PWM_GROUP_PWM_ENABLE_MASK(sibling_index);
    return pwm_check_is_used(sibling) == 0U && (control & sibling_enabled) == 0U &&
           (control & PWM_GROUP_PWM_GROUP_MODE_ENABLE_MASK) == 0U &&
           (control & PWM_GROUP_PWM_PRE_DIV_MASK) == 0U;
#endif
}

[[nodiscard]] bool channel_running(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return (group_control(channel) & 1U) != 0U;
#else
    const auto group = group_for_channel(channel);
    const auto post = static_cast<std::uint8_t>(channel & 1U);
    const auto control = group_control(group);
    return (control & PWM_GROUP_PWM_ENABLE_MASK(post)) != 0U &&
           ((control & PWM_GROUP_MODE_SET_MASK(post)) ==
               (static_cast<std::uint32_t>(PWM_PWM_MODE) << PWM_GROUP_MODE_SET_BIT(post)));
#endif
}

[[nodiscard]] bool channel_disabled(const std::uint8_t channel) {
#if NEWMOTA_BEKEN_PWM_UNITS
    return !channel_running(channel);
#else
    const auto group = group_for_channel(channel);
    const auto post = static_cast<std::uint8_t>(channel & 1U);
    return (group_control(group) & PWM_GROUP_PWM_ENABLE_MASK(post)) == 0U;
#endif
}

[[nodiscard]] bool initialize_channel(const PlanEntry& entry) {
    if (!set_safe_gpio(entry.request)) {
        return false;
    }
    const auto intervals = entry.timing.period;
    const auto active_ticks = pwm_duty_interval(
        entry.reconfiguration ? entry.retained_duty : entry.request.initial_duty, intervals);
    const auto high_ticks = entry.request.inverted ? intervals - active_ticks : active_ticks;
#if NEWMOTA_BEKEN_PWM_UNITS
    pwm_param_t parameter{};
    parameter.channel = entry.channel;
    parameter.cfg.bits.mode = PWM_PWM_MODE;
    parameter.cfg.bits.clk = PWM_CLK_26M;
    parameter.end_value = entry.timing.period;
    parameter.duty_cycle = high_ticks;
    if (sddev_control(kPwmDeviceName, CMD_PWM_INIT_PARAM, &parameter) != PWM_SUCCESS ||
        channel_t1(entry.channel) != high_ticks ||
        channel_t4(entry.channel) != entry.timing.period || !pin_is_routed_to_pwm(entry)) {
        return false;
    }
    UINT32 channel = entry.channel;
    return sddev_control(kPwmDeviceName, CMD_PWM_UNIT_ENABLE, &channel) == PWM_SUCCESS &&
           channel_running(entry.channel);
#else
    pwm_param_st parameter{};
    parameter.init_level = high_ticks == 0U ? 0U : 1U;
    parameter.status = 0U;
    parameter.chan = entry.channel;
    parameter.is_active = 0U;
    parameter.t1 = high_ticks;
    parameter.t4 = entry.timing.period;
    if (pwm_init_param(&parameter) != PWM_SUCCESS || channel_t1(entry.channel) != high_ticks ||
        channel_t4(entry.channel) != entry.timing.period || !pin_is_routed_to_pwm(entry)) {
        return false;
    }
    if (pwm_start(entry.channel) != PWM_SUCCESS || !channel_running(entry.channel)) {
        return false;
    }
    return true;
#endif
}

[[nodiscard]] bool cleanup_channel(const PlanEntry& entry) {
#if NEWMOTA_BEKEN_PWM_UNITS
    UINT32 channel = entry.channel;
    const bool stopped =
        sddev_control(kPwmDeviceName, CMD_PWM_UNIT_DISABLE, &channel) == PWM_SUCCESS;
    pwm_param_t parameter{};
    parameter.channel = entry.channel;
    parameter.cfg.bits.mode = PWM_PWM_MODE;
    parameter.cfg.bits.clk = PWM_CLK_26M;
    const bool deinitialized =
        sddev_control(kPwmDeviceName, CMD_PWM_DEINIT_PARAM, &parameter) == PWM_SUCCESS;
    return stopped && deinitialized && channel_disabled(entry.channel) &&
           set_safe_gpio(entry.request);
#else
    const bool safe_pad = set_safe_gpio(entry.request);
    const bool stopped = pwm_stop(entry.channel) == PWM_SUCCESS;
    const auto group = group_for_channel(entry.channel);
    const auto post = static_cast<std::uint8_t>(entry.channel & 1U);
    const auto control = group_control(group);
    const bool stopped_readback =
        (control & PWM_GROUP_PWM_ENABLE_MASK(post)) == 0U && pwm_check_is_used(entry.channel) == 0U;
    return safe_pad && stopped && stopped_readback &&
           (gpio_config_register(static_cast<std::uint8_t>(entry.request.pin.value())) &
               GCFG_FUNCTION_ENABLE_BIT) == 0U;
#endif
}

[[nodiscard]] NativeOutput* output_for_handle(const PwmHandle handle) {
    for (auto& output : g_native.outputs) {
        if (output.used && output.handle.slot == handle.slot &&
            output.handle.generation == handle.generation) {
            return &output;
        }
    }
    return nullptr;
}

[[nodiscard]] PwmResult prepare_native(void*, const PwmPlanToken token, const bool reconfiguration,
    const PwmBackendActiveOutput* const active, const std::size_t active_count,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendPlanResult* const results) {
    g_plan.reset();
    g_plan.reset(new (std::nothrow) NativePlan{});
    if (!g_plan) {
        return PwmResult::no_memory;
    }
    auto& plan = *g_plan;
    plan.valid = true;
    plan.reconfiguration = reconfiguration;
    plan.token = token;
    bool planned_group[kGroupCount]{};
    for (std::size_t i = 0U; i < count; ++i) {
        results[i] = {};
        std::uint8_t channel = 0U;
        if (!route_for_pin(requests[i].pin, channel)) {
            results[i].result = PwmResult::unsupported_route;
            continue;
        }
        const auto group = group_for_channel(channel);
        if (planned_group[group]) {
            results[i].result = PwmResult::resource_unavailable;
            continue;
        }
        bool active_pair_conflict = false;
        const PwmBackendActiveOutput* active_target = nullptr;
        for (std::size_t j = 0U; j < active_count; ++j) {
            const auto& candidate = active[j];
            std::uint8_t other_channel = 0U;
            if (!route_for_pin(candidate.request.pin, other_channel) ||
                group_for_channel(other_channel) != group) {
                continue;
            }
            const bool is_target = reconfiguration && candidate.handle.slot == handles[i].slot &&
                                   candidate.handle.generation == handles[i].generation;
            if (is_target && other_channel == channel) {
                active_target = &candidate;
            } else {
                active_pair_conflict = true;
            }
        }
        auto& native = g_native.outputs[channel];
        if (active_pair_conflict ||
            (reconfiguration && (active_target == nullptr || !native.used ||
                                    native.handle.slot != handles[i].slot ||
                                    native.handle.generation != handles[i].generation)) ||
            (!reconfiguration && native.used)) {
            results[i].result = PwmResult::ownership_conflict;
            continue;
        }
        if (!sibling_pair_idle(channel) ||
#if NEWMOTA_BEKEN_PWM_UNITS
            (!reconfiguration && !channel_disabled(channel))) {
#else
            (!reconfiguration &&
                (pwm_check_is_used(channel) != 0U || !channel_disabled(channel)))) {
#endif
            results[i].result = PwmResult::resource_unavailable;
            continue;
        }
        Timing timing{};
        if (!plan_timing(requests[i], timing)) {
            results[i].result = PwmResult::requirements_unsatisfied;
            continue;
        }
        auto& entry = plan.entries[plan.count++];
        entry.valid = true;
        entry.reconfiguration = reconfiguration;
        entry.channel = channel;
        entry.request = requests[i];
        entry.handle = handles[i];
        entry.timing = timing;
        entry.retained_duty = reconfiguration ? active_target->applied_duty : 0U;
        planned_group[group] = true;
        results[i].result = PwmResult::success;
        results[i].effective = {timing.frequency, timing.resolution};
        results[i].failure_domain = failure_domain(channel);
    }
    return PwmResult::success;
}

[[nodiscard]] PwmResult activate_native(void*, const PwmPlanToken token, const bool reconfiguration,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendOperationResult* const results) {
    if (!g_plan) {
        return PwmResult::stale_plan;
    }
    auto& plan = *g_plan;
    if (!plan.valid || plan.token.slot != token.slot || plan.token.generation != token.generation ||
        plan.reconfiguration != reconfiguration || plan.count != count) {
        return PwmResult::stale_plan;
    }
    for (std::size_t i = 0U; i < count; ++i) {
        auto& entry = plan.entries[i];
        results[i] = {};
        results[i].failure_domain = entry.valid ? failure_domain(entry.channel) : 0U;
        if (!entry.valid || entry.request.pin.value() != requests[i].pin.value() ||
            entry.handle.slot != handles[i].slot ||
            entry.handle.generation != handles[i].generation) {
            results[i].applied = PwmApplyState::failed;
            results[i].result = PwmResult::stale_plan;
            continue;
        }
        if (entry.reconfiguration) {
            auto* const existing = output_for_handle(entry.handle);
            const bool ready =
                existing != nullptr && !existing->quarantined && set_safe_gpio(entry.request);
#if NEWMOTA_BEKEN_PWM_UNITS
            UINT32 channel = entry.channel;
            const bool stopped = ready && sddev_control(kPwmDeviceName, CMD_PWM_UNIT_DISABLE,
                                              &channel) == PWM_SUCCESS;
#else
            const bool stopped = ready && pwm_stop(entry.channel) == PWM_SUCCESS;
#endif
            if (!stopped) {
                if (existing != nullptr) {
                    existing->quarantined = true;
                }
                results[i] = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, failure_domain(entry.channel)};
                continue;
            }
            entry.retained_duty = existing->duty;
        }
        if (!initialize_channel(entry)) {
            const bool cleaned = cleanup_channel(entry);
            if (!cleaned || entry.reconfiguration) {
                auto& output = g_native.outputs[entry.channel];
                if (entry.reconfiguration) {
                    output.quarantined = true;
                } else {
                    output = {true, true, entry.handle, entry.request,
                        {entry.timing.frequency, entry.timing.resolution},
                        entry.request.initial_duty};
                }
                results[i] = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, failure_domain(entry.channel)};
            } else {
                results[i] = {
                    PwmApplyState::failed, PwmResult::native_failure, PwmOutputState::active, 0U};
            }
            continue;
        }
        auto& output = g_native.outputs[entry.channel];
        output = {true, false, entry.handle, entry.request,
            {entry.timing.frequency, entry.timing.resolution},
            entry.reconfiguration ? entry.retained_duty : entry.request.initial_duty};
        results[i] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            failure_domain(entry.channel)};
    }
    g_plan.reset();
    return PwmResult::success;
}

[[nodiscard]] bool update_native_duty(const NativeOutput& output, const std::uint16_t duty) {
    std::uint8_t channel = 0U;
    if (!route_for_pin(output.request.pin, channel)) {
        return false;
    }
    const auto period = channel_t4(channel);
    if (period < 2U || period > kMaximumPeriod) {
        return false;
    }
    const auto active_ticks = pwm_duty_interval(duty, period);
    const auto high_ticks = output.request.inverted ? period - active_ticks : active_ticks;
#if NEWMOTA_BEKEN_PWM_UNITS
    pwm_param_t parameter{};
    parameter.channel = channel;
    parameter.duty_cycle = high_ticks;
    return sddev_control(kPwmDeviceName, CMD_PWM_SET_DUTY_CYCLE, &parameter) == PWM_SUCCESS &&
           channel_t1(channel) == high_ticks;
#else
    // The vendor update API retains the duty in its ISR-owned channel state;
    // the waveform changes at the following PWM update boundary.
    pwm_param_st parameter{};
    parameter.init_level = high_ticks == 0U ? 0U : 1U;
    parameter.chan = channel;
    parameter.t1 = high_ticks;
    parameter.t4 = period;
    if (pwm_update_param(&parameter) != PWM_SUCCESS) {
        return false;
    }
    return true;
#endif
}

[[nodiscard]] PwmResult write_batch_native(void*, const PwmDutyUpdate* const updates,
    const std::size_t count, PwmBackendOperationResult* const results) {
    for (std::size_t i = 0U; i < count; ++i) {
        results[i] = {};
        auto* const output = output_for_handle(updates[i].handle);
        if (output == nullptr || output->quarantined) {
            results[i] = {
                PwmApplyState::failed, PwmResult::quarantined, PwmOutputState::quarantined, 0U};
            continue;
        }
        std::uint8_t channel = 0U;
        (void)route_for_pin(output->request.pin, channel);
        if (!update_native_duty(*output, updates[i].duty)) {
            results[i] = {PwmApplyState::failed, PwmResult::native_failure, PwmOutputState::active,
                failure_domain(channel)};
            continue;
        }
        output->duty = updates[i].duty;
        results[i] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            failure_domain(channel)};
    }
    return PwmResult::success;
}

[[nodiscard]] PwmResult release_native(
    void*, const PwmBackendActiveOutput& output, PwmBackendOperationResult& result) {
    result = {};
    auto* const native = output_for_handle(output.handle);
    if (native == nullptr) {
        result = {PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        return PwmResult::success;
    }
    std::uint8_t channel = 0U;
    if (!route_for_pin(native->request.pin, channel)) {
        native->quarantined = true;
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined, 0U};
        return PwmResult::success;
    }
    PlanEntry entry{};
    entry.valid = true;
    entry.channel = channel;
    entry.request = native->request;
    entry.handle = native->handle;
    if (!cleanup_channel(entry)) {
        native->quarantined = true;
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
            failure_domain(channel)};
        return PwmResult::success;
    }
    *native = NativeOutput{};
    result = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
        failure_domain(channel)};
    return PwmResult::success;
}

void discard_native(void*) {
    g_plan.reset();
}

const PwmBackendOps kBackend{
    prepare_native, activate_native, write_batch_native, release_native, discard_native};

} // namespace

/** @brief Returns the Beken six-channel PWM backend callbacks. */
const PwmBackendOps& pwm_native_backend() {
    return kBackend;
}

/** @brief Provides the retained Beken channel state. */
void* pwm_native_context() {
    return &g_native;
}

} // namespace newmota::platform::detail
