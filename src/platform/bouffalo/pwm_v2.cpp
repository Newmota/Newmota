/*
  pwm_v2.cpp - Bouffalo BL616 PWM v2 adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file pwm_v2.cpp
 * @brief BL616 PWM-v2 adapter with single-ended positive outputs.
 *
 * BL616 has one PWM-v2 group with four channels. In single-ended mode, GPIO n
 * carries the positive output of channel n % 4. All channels share the
 * group's clock divider and period, so concurrent outputs share one carrier.
 */

#include "newmota/platform/gpio.h"
#include "newmota/platform/pwm_detail.h"

#if defined(BL616)
extern "C" {
#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_pwm_v2.h"
#include "hardware/gpio_reg.h"
#include "hardware/pwm_v2_reg.h"
}

#include "pwm_timing.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace newmota::platform::detail {
namespace {

using bouffalo_pwm::plan_timing;
using bouffalo_pwm::Timing;

constexpr std::size_t kChannelCount = PWM_V2_CH_MAX;
constexpr std::uint32_t kLastPin = 34U;
constexpr std::uint32_t kGpioFunctionSoftware = 11U;
constexpr std::uint32_t kGpioFunctionPwm0 = 16U;
// GLB_PWM_CFG0 bit 0 selects single-ended (0) or differential (1) PWM pads.
// The pinned SDK setter shifts the whole register before writing it back.
constexpr std::uint32_t kGlbPwmConfig0 = 0x2000'01C0U;
constexpr std::uint32_t kGlbPwmDifferential = 1U;

struct NativeOutput {
    bool used{};
    bool quarantined{};
    PwmHandle handle{};
    PwmRequest request{};
    PwmEffectiveSettings effective{};
    std::uint16_t duty{};
};

struct PlanEntry {
    bool valid{};
    bool reconfiguration{};
    std::uint8_t channel{};
    PwmRequest request{};
    PwmHandle handle{};
    std::uint16_t retained_duty{};
};

struct NativePlan {
    bool valid{};
    bool reconfiguration{};
    bool retime{};
    PwmPlanToken token{};
    Timing timing{};
    std::size_t count{};
    PlanEntry entries[kMaximumPwmOutputs]{};
};

struct NativeState {
    NativeOutput outputs[kChannelCount]{};
    Timing timing{};
};

NativeState g_native{};
// Exists from prepare until activation or discard.
std::unique_ptr<NativePlan> g_plan;

[[nodiscard]] bflb_device_s* pwm_device() {
    static auto* const device = bflb_device_get_by_name("pwm_v2_0");
    return device;
}

[[nodiscard]] bflb_device_s* gpio_device() {
    static auto* const device = bflb_device_get_by_name("gpio");
    return device;
}

[[nodiscard]] std::uint32_t pwm_register(
    const bflb_device_s* const device, const std::uint32_t offset) {
    return getreg32(device->reg_base + offset);
}

[[nodiscard]] std::uint32_t threshold_offset(const std::uint8_t channel) {
    return PWM_MC0_CH0_THRE_OFFSET + static_cast<std::uint32_t>(channel) * 4U;
}

[[nodiscard]] std::uint32_t enable_bit(const std::uint8_t channel) {
    return static_cast<std::uint32_t>(PWM_CH0_PEN) << (4U * channel);
}

[[nodiscard]] std::uint32_t polarity_bit(const std::uint8_t channel) {
    return static_cast<std::uint32_t>(PWM_CH0_PPL) << (2U * channel);
}

[[nodiscard]] std::uint16_t failure_domain(const std::uint8_t channel) {
    return static_cast<std::uint16_t>(channel + 1U);
}

[[nodiscard]] bool route_for_pin(const PinId pin, std::uint8_t& channel) {
    const auto value = pin.value();
    if (value > kLastPin) {
        return false;
    }
    channel = static_cast<std::uint8_t>(value % kChannelCount);
    return true;
}

[[nodiscard]] std::uint32_t gpio_function(const std::uint8_t pin) {
    auto* const device = gpio_device();
    if (device == nullptr || pin > kLastPin) {
        return 0xffU;
    }
    const auto config =
        getreg32(device->reg_base + GLB_GPIO_CFG0_OFFSET + (static_cast<std::uint32_t>(pin) << 2U));
    return (config & GLB_REG_GPIO_0_FUNC_SEL_MASK) >> GLB_REG_GPIO_0_FUNC_SEL_SHIFT;
}

[[nodiscard]] bool configure_safe_gpio(const PwmRequest& request) {
    auto* const device = gpio_device();
    if (device == nullptr) {
        return false;
    }
    const auto pin = static_cast<std::uint8_t>(request.pin.value());
    if (request.inverted) {
        bflb_gpio_set(device, pin);
    } else {
        bflb_gpio_reset(device, pin);
    }
    bflb_gpio_init(device, pin, GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_0);
    return gpio_function(pin) == kGpioFunctionSoftware;
}

[[nodiscard]] bool route_gpio_to_pwm(const PwmRequest& request) {
    auto* const device = gpio_device();
    if (device == nullptr) {
        return false;
    }
    const auto pin = static_cast<std::uint8_t>(request.pin.value());
    bflb_gpio_init(
        device, pin, GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_0);
    return gpio_function(pin) == kGpioFunctionPwm0;
}

[[nodiscard]] bool group_stopped(bflb_device_s* const device) {
    return (pwm_register(device, PWM_MC0_CONFIG0_OFFSET) & PWM_STS_STOP) != 0U;
}

[[nodiscard]] bool channel_enabled(bflb_device_s* const device, const std::uint8_t channel) {
    return (pwm_register(device, PWM_MC0_CONFIG1_OFFSET) & enable_bit(channel)) != 0U;
}

/** @brief Disables one positive output and clears its thresholds. */
[[nodiscard]] bool stop_channel(bflb_device_s* const device, const std::uint8_t channel) {
    bflb_pwm_v2_channel_positive_stop(device, channel);
    bflb_pwm_v2_channel_set_threshold(device, channel, 0U, 0U);
    return !channel_enabled(device, channel) &&
           pwm_register(device, threshold_offset(channel)) == 0U;
}

[[nodiscard]] bool select_single_ended() {
    putreg32(getreg32(kGlbPwmConfig0) & ~kGlbPwmDifferential, kGlbPwmConfig0);
    return (getreg32(kGlbPwmConfig0) & kGlbPwmDifferential) == 0U;
}

/** @brief Stops the group and programs the shared clock divider and period. */
[[nodiscard]] bool program_group(bflb_device_s* const device, const Timing& timing) {
    bflb_pwm_v2_stop(device);
    if (!group_stopped(device) || !select_single_ended()) {
        return false;
    }
    const bflb_pwm_v2_config_s config{BFLB_SYSTEM_PBCLK, timing.divider, timing.period};
    bflb_pwm_v2_init(device, &config);
    const auto divider =
        (pwm_register(device, PWM_MC0_CONFIG0_OFFSET) & PWM_CLK_DIV_MASK) >> PWM_CLK_DIV_SHIFT;
    const auto period =
        (pwm_register(device, PWM_MC0_PERIOD_OFFSET) & PWM_PERIOD_MASK) >> PWM_PERIOD_SHIFT;
    return divider == timing.divider && period == timing.period && group_stopped(device);
}

[[nodiscard]] bool start_group(bflb_device_s* const device) {
    bflb_pwm_v2_start(device);
    return !group_stopped(device);
}

[[nodiscard]] bool write_threshold(bflb_device_s* const device, const std::uint8_t channel,
    const std::uint16_t period, const std::uint16_t duty) {
    // The output is active from count 0 until the high threshold.
    const auto high = static_cast<std::uint16_t>(pwm_duty_interval(duty, period));
    bflb_pwm_v2_channel_set_threshold(device, channel, 0U, high);
    return pwm_register(device, threshold_offset(channel)) ==
           (static_cast<std::uint32_t>(high) << 16U);
}

[[nodiscard]] bool configure_channel(
    bflb_device_s* const device, const PlanEntry& entry, const std::uint16_t period) {
    if (!configure_safe_gpio(entry.request)) {
        return false;
    }
    bflb_pwm_v2_channel_config_s config{};
    config.positive_polarity =
        entry.request.inverted ? PWM_POLARITY_ACTIVE_LOW : PWM_POLARITY_ACTIVE_HIGH;
    config.negative_polarity = PWM_POLARITY_ACTIVE_HIGH;
    config.positive_stop_state = PWM_STATE_INACTIVE;
    config.negative_stop_state = PWM_STATE_INACTIVE;
    config.positive_brake_state = PWM_STATE_INACTIVE;
    config.negative_brake_state = PWM_STATE_INACTIVE;
    bflb_pwm_v2_channel_init(device, entry.channel, &config);
    const bool active_high =
        (pwm_register(device, PWM_MC0_CONFIG1_OFFSET) & polarity_bit(entry.channel)) != 0U;
    if (active_high == entry.request.inverted) {
        return false;
    }
    const auto duty = entry.reconfiguration ? entry.retained_duty : entry.request.initial_duty;
    if (!write_threshold(device, entry.channel, period, duty)) {
        return false;
    }
    bflb_pwm_v2_channel_positive_start(device, entry.channel);
    return channel_enabled(device, entry.channel) && route_gpio_to_pwm(entry.request);
}

[[nodiscard]] bool cleanup_channel(bflb_device_s* const device, const PlanEntry& entry) {
    return configure_safe_gpio(entry.request) && stop_channel(device, entry.channel);
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

[[nodiscard]] bool same_handle(const PwmHandle left, const PwmHandle right) {
    return left.slot == right.slot && left.generation == right.generation;
}

[[nodiscard]] bool targeted(
    const PwmHandle handle, const PwmHandle* const handles, const std::size_t count) {
    for (std::size_t i = 0U; i < count; ++i) {
        if (same_handle(handle, handles[i])) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool timing_meets(const Timing& timing, const PwmRequest& request) {
    return timing.valid && timing.resolution >= request.min_resolution_bits &&
           pwm_frequency_meets_request(timing.frequency, request);
}

/**
 * @brief Plans one batch against an optional locked shared carrier.
 * @return Number of rejected reconfiguration targets.
 */
std::size_t plan_batch(const Timing* const locked, const bool reconfiguration,
    const PwmBackendActiveOutput* const active, const std::size_t active_count,
    const PwmRequest* const requests, const PwmHandle* const handles, const std::size_t count,
    PwmBackendPlanResult* const results) {
    auto& plan = *g_plan;
    plan.count = 0U;
    plan.retime = locked == nullptr;
    plan.timing = locked != nullptr ? *locked : Timing{};
    auto* const device = pwm_device();
    const auto source_hz = bflb_clk_get_system_clock(BFLB_SYSTEM_PBCLK);
    bool planned_channel[kChannelCount]{};
    std::size_t rejected_targets = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        results[i] = {};
        std::uint8_t channel = 0U;
        if (!route_for_pin(requests[i].pin, channel)) {
            results[i].result = PwmResult::unsupported_route;
        } else if (planned_channel[channel]) {
            results[i].result = PwmResult::resource_unavailable;
        } else {
            bool other_owner = false;
            const PwmBackendActiveOutput* active_output = nullptr;
            for (std::size_t a = 0U; a < active_count; ++a) {
                std::uint8_t active_channel = 0U;
                if (route_for_pin(active[a].request.pin, active_channel) &&
                    active_channel == channel) {
                    if (reconfiguration && same_handle(active[a].handle, handles[i])) {
                        active_output = &active[a];
                    } else {
                        other_owner = true;
                    }
                }
            }
            const auto& native = g_native.outputs[channel];
            if (other_owner || (!reconfiguration && native.used) ||
                (reconfiguration && (active_output == nullptr || !native.used ||
                                        !same_handle(native.handle, handles[i])))) {
                results[i].result = PwmResult::ownership_conflict;
            } else if (!native.used && channel_enabled(device, channel)) {
                results[i].result = PwmResult::resource_unavailable;
            } else if (!plan.timing.valid && !plan_timing(requests[i], source_hz, plan.timing)) {
                results[i].result = PwmResult::requirements_unsatisfied;
            } else if (!timing_meets(plan.timing, requests[i])) {
                results[i].result = PwmResult::requirements_unsatisfied;
            } else {
                auto& entry = plan.entries[plan.count++];
                entry = {true, reconfiguration, channel, requests[i], handles[i],
                    reconfiguration ? active_output->applied_duty : std::uint16_t{0U}};
                planned_channel[channel] = true;
                results[i].result = PwmResult::success;
                results[i].failure_domain = failure_domain(channel);
            }
        }
        if (reconfiguration && results[i].result != PwmResult::success) {
            ++rejected_targets;
        }
    }
    for (std::size_t i = 0U; i < count; ++i) {
        if (results[i].result == PwmResult::success) {
            results[i].effective = {plan.timing.frequency, plan.timing.resolution};
        }
    }
    return rejected_targets;
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
    g_plan->valid = true;
    g_plan->reconfiguration = reconfiguration;
    g_plan->token = token;
    if (pwm_device() == nullptr || gpio_device() == nullptr) {
        for (std::size_t i = 0U; i < count; ++i) {
            results[i].result = PwmResult::native_failure;
        }
        return PwmResult::success;
    }
    // Outputs outside this batch keep running, so they pin the shared carrier.
    bool pinned = false;
    for (std::size_t a = 0U; a < active_count; ++a) {
        pinned = pinned || !reconfiguration || !targeted(active[a].handle, handles, count);
    }
    const auto current = g_native.timing;
    if (pinned && !current.valid) {
        // Running outputs without a known carrier: leave the group untouched.
        for (std::size_t i = 0U; i < count; ++i) {
            results[i].result = PwmResult::resource_unavailable;
        }
        return PwmResult::success;
    }
    const auto* const locked = pinned ? &current : nullptr;
    const auto rejected = plan_batch(
        locked, reconfiguration, active, active_count, requests, handles, count, results);
    // A rejected reconfiguration target keeps running on the current carrier.
    if (locked == nullptr && rejected != 0U && current.valid) {
        (void)plan_batch(
            &current, reconfiguration, active, active_count, requests, handles, count, results);
    }
    return PwmResult::success;
}

void quarantine(const PlanEntry& entry, PwmBackendOperationResult& result) {
    if (auto* const existing = output_for_handle(entry.handle)) {
        existing->quarantined = true;
    } else {
        g_native.outputs[entry.channel] = {true, true, entry.handle, entry.request,
            {g_native.timing.frequency, g_native.timing.resolution}, entry.request.initial_duty};
    }
    result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
        failure_domain(entry.channel)};
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
    auto* const device = pwm_device();
    if (device == nullptr) {
        return PwmResult::native_failure;
    }
    bool valid[kMaximumPwmOutputs]{};
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& entry = plan.entries[i];
        results[i] = {};
        valid[i] = entry.valid && entry.request.pin.value() == requests[i].pin.value() &&
                   same_handle(entry.handle, handles[i]);
        if (!valid[i]) {
            results[i] = {PwmApplyState::failed, PwmResult::stale_plan, PwmOutputState::active, 0U};
            continue;
        }
        if (entry.reconfiguration) {
            auto* const existing = output_for_handle(entry.handle);
            if (existing == nullptr || existing->quarantined) {
                results[i] = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, failure_domain(entry.channel)};
                valid[i] = false;
                continue;
            }
            plan.entries[i].retained_duty = existing->duty;
        }
    }
    // A new carrier is only planned when every running output is in this batch.
    if (plan.retime && !program_group(device, plan.timing)) {
        for (std::size_t i = 0U; i < count; ++i) {
            if (valid[i]) {
                (void)cleanup_channel(device, plan.entries[i]);
                quarantine(plan.entries[i], results[i]);
            }
        }
        g_native.timing = {};
        g_plan.reset();
        return PwmResult::success;
    }
    g_native.timing = plan.timing;
    for (std::size_t i = 0U; i < count; ++i) {
        if (!valid[i]) {
            continue;
        }
        const auto& entry = plan.entries[i];
        if (!configure_channel(device, entry, plan.timing.period)) {
            if (!cleanup_channel(device, entry) || entry.reconfiguration) {
                quarantine(entry, results[i]);
            } else {
                results[i] = {
                    PwmApplyState::failed, PwmResult::native_failure, PwmOutputState::active, 0U};
            }
            valid[i] = false;
            continue;
        }
        g_native.outputs[entry.channel] = {true, false, entry.handle, entry.request,
            {plan.timing.frequency, plan.timing.resolution},
            entry.reconfiguration ? entry.retained_duty : entry.request.initial_duty};
        results[i] = {PwmApplyState::applied, PwmResult::success, PwmOutputState::active,
            failure_domain(entry.channel)};
    }
    if (group_stopped(device) && !start_group(device)) {
        for (std::size_t i = 0U; i < count; ++i) {
            if (valid[i]) {
                quarantine(plan.entries[i], results[i]);
            }
        }
    }
    g_plan.reset();
    return PwmResult::success;
}

[[nodiscard]] PwmResult write_batch_native(void*, const PwmDutyUpdate* const updates,
    const std::size_t count, PwmBackendOperationResult* const results) {
    auto* const device = pwm_device();
    if (device == nullptr) {
        return PwmResult::native_failure;
    }
    for (std::size_t i = 0U; i < count; ++i) {
        auto* const output = output_for_handle(updates[i].handle);
        if (output == nullptr || output->quarantined) {
            results[i] = {
                PwmApplyState::failed, PwmResult::quarantined, PwmOutputState::quarantined, 0U};
            continue;
        }
        std::uint8_t channel = 0U;
        (void)route_for_pin(output->request.pin, channel);
        if (!write_threshold(device, channel, g_native.timing.period, updates[i].duty)) {
            output->quarantined = true;
            results[i] = {PwmApplyState::unknown, PwmResult::quarantined,
                PwmOutputState::quarantined, failure_domain(channel)};
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
    auto* const native = output_for_handle(output.handle);
    auto* const device = pwm_device();
    if (native == nullptr || device == nullptr) {
        result = {PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        return PwmResult::success;
    }
    std::uint8_t channel = 0U;
    (void)route_for_pin(native->request.pin, channel);
    if (!configure_safe_gpio(native->request) || !stop_channel(device, channel)) {
        native->quarantined = true;
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
            failure_domain(channel)};
        return PwmResult::success;
    }
    *native = NativeOutput{};
    bool any_used = false;
    for (const auto& remaining : g_native.outputs) {
        any_used = any_used || remaining.used;
    }
    if (!any_used) {
        bflb_pwm_v2_stop(device);
        g_native.timing = {};
    }
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

/** @brief Returns the BL616 PWM v2 backend callbacks. */
const PwmBackendOps& pwm_native_backend() {
    return kBackend;
}

/** @brief Provides the retained BL616 channel and carrier state. */
void* pwm_native_context() {
    return &g_native;
}

} // namespace newmota::platform::detail
#endif
