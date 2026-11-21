/*
  pwm.cpp - Bouffalo BL602 PWM v1 adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file pwm.cpp
 * @brief BL602 PWM-v1 adapter; BL616 uses pwm_v2.cpp and other targets reject PWM.
 */

#include "newmota/platform/gpio.h"
#include "newmota/platform/pwm_detail.h"

#if defined(BL602)
extern "C" {
#include "bflb_clock.h"
#include "bflb_gpio.h"
#include "bflb_pwm_v1.h"
#include "hardware/pwm_v1_reg.h"
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

constexpr std::size_t kChannelCount = 5U;
constexpr std::uint32_t kGpioConfigCtl0Offset = 0x100U;
constexpr std::uint32_t kGpioFunctionMask = 0x0fU;

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

[[nodiscard]] bflb_device_s* pwm_device() {
    static auto* const device = bflb_device_get_by_name("pwm_v1");
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

void write_pwm_register(
    bflb_device_s* const device, const std::uint32_t offset, const std::uint32_t value) {
    putreg32(value, device->reg_base + offset);
}

[[nodiscard]] std::uint32_t channel_offset(const std::uint8_t channel, const std::uint32_t offset) {
    return offset + static_cast<std::uint32_t>(channel) * 0x20U;
}

[[nodiscard]] std::uint16_t failure_domain(const std::uint8_t channel) {
    return static_cast<std::uint16_t>(channel + 1U);
}

[[nodiscard]] bool route_for_pin(const PinId pin, std::uint8_t& channel) {
    const auto value = pin.value();
    if (value > 22U) {
        return false;
    }
    channel = static_cast<std::uint8_t>(value % 5U);
    // BL602 has no PWM channel 4 route on GPIO 20..22 beyond the repeated map.
    return (value <= 19U) || value == 20U || value == 21U || value == 22U;
}

[[nodiscard]] int gpio_function(const std::uint8_t pin) {
    auto* const device = gpio_device();
    if (device == nullptr || pin > 22U) {
        return -1;
    }
    // BL602 stores the even pin function in bits 11:8 and the odd pin
    // function in bits 27:24. The pinned SDK getter swaps those selections.
    const auto address =
        device->reg_base + kGpioConfigCtl0Offset + (static_cast<std::uint32_t>(pin) / 2U) * 4U;
    const auto shift = (pin & 1U) == 0U ? 8U : 24U;
    return static_cast<int>((getreg32(address) >> shift) & kGpioFunctionMask);
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
    return gpio_function(pin) == 11;
}

[[nodiscard]] bool route_gpio_to_pwm(const PwmRequest& request) {
    auto* const device = gpio_device();
    if (device == nullptr) {
        return false;
    }
    const auto pin = static_cast<std::uint8_t>(request.pin.value());
    bflb_gpio_init(
        device, pin, GPIO_FUNC_PWM0 | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_0);
    return gpio_function(pin) == 8;
}

[[nodiscard]] bool channel_stopped(bflb_device_s* const device, const std::uint8_t channel) {
    const auto config = pwm_register(device, channel_offset(channel, PWM0_CONFIG_OFFSET));
    return (config & PWM_STS_TOP) != 0U;
}

[[nodiscard]] bool channel_running(bflb_device_s* const device, const std::uint8_t channel) {
    const auto config = pwm_register(device, channel_offset(channel, PWM0_CONFIG_OFFSET));
    return (config & (PWM_STS_TOP | PWM_STOP_EN)) == 0U;
}

void reset_channel_registers(bflb_device_s* const device, const std::uint8_t channel) {
    write_pwm_register(device, channel_offset(channel, PWM0_CLKDIV_OFFSET), 0U);
    write_pwm_register(device, channel_offset(channel, PWM0_THRE1_OFFSET), 0U);
    write_pwm_register(device, channel_offset(channel, PWM0_THRE2_OFFSET), 0U);
    write_pwm_register(device, channel_offset(channel, PWM0_PERIOD_OFFSET), 0U);
    write_pwm_register(device, channel_offset(channel, PWM0_INTERRUPT_OFFSET), 0U);
    write_pwm_register(device, channel_offset(channel, PWM0_CONFIG_OFFSET), PWM_STOP_MODE);
}

[[nodiscard]] bool channel_reset_safe(bflb_device_s* const device, const std::uint8_t channel) {
    const auto config = pwm_register(device, channel_offset(channel, PWM0_CONFIG_OFFSET));
    return (config == 0U || config == PWM_STOP_MODE) &&
           pwm_register(device, channel_offset(channel, PWM0_CLKDIV_OFFSET)) == 0U &&
           pwm_register(device, channel_offset(channel, PWM0_THRE1_OFFSET)) == 0U &&
           pwm_register(device, channel_offset(channel, PWM0_THRE2_OFFSET)) == 0U &&
           pwm_register(device, channel_offset(channel, PWM0_PERIOD_OFFSET)) == 0U;
}

[[nodiscard]] bool write_thresholds(bflb_device_s* const device, const std::uint8_t channel,
    const std::uint16_t period, const std::uint16_t duty) {
    const auto high =
        static_cast<std::uint16_t>(pwm_duty_interval(duty, static_cast<std::uint32_t>(period)));
    bflb_pwm_v1_channel_set_threshold(device, channel, 0U, high);
    return pwm_register(device, channel_offset(channel, PWM0_THRE1_OFFSET)) == 0U &&
           (pwm_register(device, channel_offset(channel, PWM0_THRE2_OFFSET)) & PWM_THRE2_MASK) ==
               high;
}

[[nodiscard]] bool configure_channel(bflb_device_s* const device, PlanEntry& entry) {
    if (!configure_safe_gpio(entry.request)) {
        return false;
    }
    const auto config =
        bflb_pwm_v1_channel_config_s{BFLB_SYSTEM_PBCLK, entry.timing.divider, entry.timing.period};
    bflb_pwm_v1_channel_init(device, entry.channel, &config);
    const auto divider =
        pwm_register(device, channel_offset(entry.channel, PWM0_CLKDIV_OFFSET)) & PWM_CLK_DIV_MASK;
    const auto period =
        pwm_register(device, channel_offset(entry.channel, PWM0_PERIOD_OFFSET)) & PWM_PERIOD_MASK;
    if (divider != entry.timing.divider || period != entry.timing.period ||
        !channel_stopped(device, entry.channel)) {
        return false;
    }
    const auto duty = entry.reconfiguration ? entry.retained_duty : entry.request.initial_duty;
    if (!write_thresholds(device, entry.channel, entry.timing.period, duty)) {
        return false;
    }
    if (bflb_pwm_v1_feature_control(
            device, entry.channel, PWM_CMD_SET_OUT_INVERT, entry.request.inverted ? 1U : 0U) != 0) {
        return false;
    }
    const auto channel_config =
        pwm_register(device, channel_offset(entry.channel, PWM0_CONFIG_OFFSET));
    if (((channel_config & PWM_OUT_INV) != 0U) != entry.request.inverted) {
        return false;
    }
    if (!route_gpio_to_pwm(entry.request)) {
        return false;
    }
    bflb_pwm_v1_start(device, entry.channel);
    return channel_running(device, entry.channel);
}

[[nodiscard]] bool cleanup_channel(bflb_device_s* const device, const PlanEntry& entry) {
    bflb_pwm_v1_stop(device, entry.channel);
    if (!channel_stopped(device, entry.channel) || !configure_safe_gpio(entry.request)) {
        return false;
    }
    reset_channel_registers(device, entry.channel);
    return channel_reset_safe(device, entry.channel);
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
    auto* const device = pwm_device();
    if (device == nullptr || gpio_device() == nullptr) {
        for (std::size_t i = 0U; i < count; ++i) {
            results[i].result = PwmResult::native_failure;
        }
        return PwmResult::success;
    }
    const auto source_hz = bflb_clk_get_system_clock(BFLB_SYSTEM_PBCLK);
    bool planned_channel[kChannelCount]{};
    for (std::size_t i = 0U; i < count; ++i) {
        results[i] = {};
        std::uint8_t channel = 0U;
        if (!route_for_pin(requests[i].pin, channel)) {
            results[i].result = PwmResult::unsupported_route;
            continue;
        }
        if (planned_channel[channel]) {
            results[i].result = PwmResult::resource_unavailable;
            continue;
        }
        bool matching_active = false;
        const PwmBackendActiveOutput* active_output = nullptr;
        for (std::size_t active_index = 0U; active_index < active_count; ++active_index) {
            const auto& candidate = active[active_index];
            std::uint8_t active_channel = 0U;
            if (route_for_pin(candidate.request.pin, active_channel) && active_channel == channel) {
                const bool is_target = reconfiguration &&
                                       candidate.handle.slot == handles[i].slot &&
                                       candidate.handle.generation == handles[i].generation;
                if (!is_target) {
                    matching_active = true;
                } else {
                    active_output = &candidate;
                }
            }
        }
        auto& native = g_native.outputs[channel];
        if ((!reconfiguration && (matching_active || native.used)) ||
            (reconfiguration && (matching_active || active_output == nullptr || !native.used ||
                                    native.handle.slot != handles[i].slot ||
                                    native.handle.generation != handles[i].generation))) {
            results[i].result = PwmResult::ownership_conflict;
            continue;
        }
        if (!native.used && !channel_reset_safe(device, channel)) {
            results[i].result = PwmResult::resource_unavailable;
            continue;
        }
        Timing timing{};
        if (!plan_timing(requests[i], source_hz, timing)) {
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
        entry.retained_duty = reconfiguration ? active_output->applied_duty : 0U;
        planned_channel[channel] = true;
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
    auto* const device = pwm_device();
    if (device == nullptr) {
        return PwmResult::native_failure;
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
            if (existing == nullptr || existing->quarantined) {
                results[i].applied = PwmApplyState::unknown;
                results[i].state = PwmOutputState::quarantined;
                results[i].result = PwmResult::quarantined;
                continue;
            }
            entry.retained_duty = existing->duty;
            bflb_pwm_v1_stop(device, entry.channel);
            if (!channel_stopped(device, entry.channel)) {
                existing->quarantined = true;
                results[i] = {PwmApplyState::unknown, PwmResult::quarantined,
                    PwmOutputState::quarantined, failure_domain(entry.channel)};
                continue;
            }
        }
        const bool configured = configure_channel(device, entry);
        if (!configured) {
            const bool cleaned = cleanup_channel(device, entry);
            if (!cleaned || entry.reconfiguration) {
                if (entry.reconfiguration) {
                    if (auto* const existing = output_for_handle(entry.handle)) {
                        existing->quarantined = true;
                    }
                } else {
                    auto& output = g_native.outputs[entry.channel];
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

[[nodiscard]] PwmResult write_batch_native(void*, const PwmDutyUpdate* const updates,
    const std::size_t count, PwmBackendOperationResult* const results) {
    auto* const device = pwm_device();
    if (device == nullptr) {
        return PwmResult::native_failure;
    }
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
        results[i].failure_domain = failure_domain(channel);
        if (!write_thresholds(device, channel,
                static_cast<std::uint16_t>(
                    output->effective.resolution_bits == 0U
                        ? 0U
                        : (pwm_register(device, channel_offset(channel, PWM0_PERIOD_OFFSET)) &
                              PWM_PERIOD_MASK)),
                updates[i].duty)) {
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
    result = {};
    auto* const native = output_for_handle(output.handle);
    auto* const device = pwm_device();
    if (native == nullptr || device == nullptr) {
        result = {PwmApplyState::failed, PwmResult::stale_handle, PwmOutputState::active, 0U};
        return PwmResult::success;
    }
    std::uint8_t channel = 0U;
    if (!route_for_pin(native->request.pin, channel)) {
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined, 0U};
        native->quarantined = true;
        return PwmResult::success;
    }
    bflb_pwm_v1_stop(device, channel);
    if (!channel_stopped(device, channel) || !configure_safe_gpio(native->request)) {
        native->quarantined = true;
        result = {PwmApplyState::unknown, PwmResult::quarantined, PwmOutputState::quarantined,
            failure_domain(channel)};
        return PwmResult::success;
    }
    reset_channel_registers(device, channel);
    if (!channel_reset_safe(device, channel)) {
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

/** @brief Returns the BL602 PWM v1 backend callbacks. */
const PwmBackendOps& pwm_native_backend() {
    return kBackend;
}

/** @brief Provides the retained BL602 channel state. */
void* pwm_native_context() {
    return &g_native;
}

} // namespace newmota::platform::detail

#elif !defined(BL616)
namespace newmota::platform::detail {
namespace {
/** @brief Rejects unqualified PWM-v2 routes before changing any registers. */
PwmResult prepare_unsupported(void*, PwmPlanToken, bool, const PwmBackendActiveOutput*, std::size_t,
    const PwmRequest*, const PwmHandle*, std::size_t count, PwmBackendPlanResult* results) {
    for (std::size_t i = 0; i < count; ++i) {
        results[i].result = PwmResult::unsupported_route;
    }
    return PwmResult::success;
}

const PwmBackendOps kUnsupportedBackend{prepare_unsupported, nullptr, nullptr, nullptr};
} // namespace

/** @brief Returns a planner that rejects PWM-v2 until its resource contract is implemented. */
const PwmBackendOps& pwm_native_backend() {
    return kUnsupportedBackend;
}

void* pwm_native_context() {
    return nullptr;
}
} // namespace newmota::platform::detail
#endif
