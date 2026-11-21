/*
  pwm_outputs.cpp - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file pwm_outputs.cpp
 * @brief Portable PWM and Light behavior for Newmota.
 */

#include "drivers/pwm/pwm_outputs.h"

#include "newmota/log.h"

#include <memory>
#include <new>
#include <utility>

namespace newmota {
namespace {
std::uint8_t resolution(const std::uint16_t range) {
    return range == 255U ? 8U : range == 511U ? 9U : 10U;
}

// Serialized application operations keep their scratch off the small native
// stacks. Duty writes run on every fade step and stay in BSS.
std::array<platform::PwmDutyUpdate, 16> writes;
std::array<platform::PwmOperationResult, 16> operations;
std::array<std::size_t, 16> indices;

/** @brief Startup and retuning scratch, on the heap only while one runs. */
struct Plan final {
    std::array<platform::PwmRequest, 16> requests{};
    std::array<platform::PwmRequestResult, 16> results{};
    std::array<platform::PwmReconfiguration, 16> changes{};
    std::array<std::size_t, 16> indices{};
};

// Held from reconfigure() until activate_reconfiguration().
std::unique_ptr<Plan> retune;

[[nodiscard]] std::unique_ptr<Plan> allocate_plan() {
    std::unique_ptr<Plan> plan{new (std::nothrow) Plan{}};
    if (!plan)
        AddLog(LogLevel::error, "PWM: Out of memory");
    return plan;
}
} // namespace

bool PwmOutputs::requires_restart() const {
    for (std::size_t i = 0U; i < outputs_.size(); ++i)
        if (configured(i) && status(i) != platform::PwmResult::success)
            return true;
    return false;
}

void PwmOutputs::initialize(const Hardware& hardware, const LightSettings& settings) {
    std::unique_ptr<Plan> plan;
    bool out_of_memory = false;
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < outputs_.size(); ++i) {
        const auto* binding =
            hardware.find_function(PinRole::pwm, static_cast<std::uint8_t>(i + 1U));
        if (binding == nullptr)
            continue;
        auto& output = outputs_[i];
        output.configured = true;
        mask_ |= static_cast<std::uint16_t>(1U << i);
        output.request = {
            {platform::PwmOutputOwner::pwm_outputs, static_cast<std::uint16_t>(i + 1U)},
            binding->pin, settings.carrier(i), 50000U, resolution(settings.range),
            (binding->modifiers & static_cast<std::uint8_t>(PinModifier::inverted)) != 0U, 0U};
        if (!plan && !out_of_memory) {
            plan = allocate_plan();
            out_of_memory = !plan;
        }
        if (!plan) {
            output.result = output.last_operation = platform::PwmResult::no_memory;
            continue;
        }
        plan->requests[count] = output.request;
        plan->indices[count++] = i;
    }
    if (count == 0U)
        return;
    auto& results = plan->results;
    platform::PwmPlanToken token;
    const auto prepared =
        platform::pwm_prepare(plan->requests.data(), count, token, results.data());
    if (prepared == platform::PwmResult::success) {
        (void)platform::pwm_activate(token, results.data(), count);
    }
    for (std::size_t j = 0U; j < count; ++j) {
        auto& output = outputs_[plan->indices[j]];
        // An early prepare failure leaves results untouched.
        output.result = prepared == platform::PwmResult::success ? results[j].result : prepared;
        output.last_operation = output.result;
        output.handle = results[j].handle;
    }
}

platform::PwmResult PwmOutputs::status(const std::size_t index) const {
    if (!configured(index))
        return platform::PwmResult::invalid_request;
    const auto& output = outputs_[index];
    if (!output.handle.valid())
        return output.result;
    platform::PwmOutputStatus status{};
    const auto result = platform::pwm_get_status(output.handle, status);
    if (result != platform::PwmResult::success)
        return result;
    return status.state == platform::PwmOutputState::active ? platform::PwmResult::success
           : status.state == platform::PwmOutputState::quarantined
               ? platform::PwmResult::quarantined
               : platform::PwmResult::faulted;
}

std::uint16_t PwmOutputs::applied(const std::size_t index) const {
    platform::PwmOutputStatus status{};
    if (index < outputs_.size() &&
        platform::pwm_get_status(outputs_[index].handle, status) == platform::PwmResult::success)
        return status.applied_duty;
    return 0U;
}

platform::PwmResult PwmOutputs::write(
    const std::array<std::uint16_t, 16>& duties, const std::uint16_t selected) {
    confirmed_mask_ = 0U;
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < outputs_.size(); ++i) {
        if ((selected & (1U << i)) == 0U)
            continue;
        const auto result = status(i);
        if (result != platform::PwmResult::success) {
            outputs_[i].last_operation = result;
            return result;
        }
        outputs_[i].last_operation = platform::PwmResult::success;
        if (duties[i] != applied(i)) {
            indices[count] = i;
            writes[count++] = {outputs_[i].handle, duties[i]};
        } else
            confirmed_mask_ |= static_cast<std::uint16_t>(1U << i);
    }
    if (count == 0U)
        return platform::PwmResult::success;
    auto result = platform::pwm_write_batch(writes.data(), count, operations.data());
    for (std::size_t j = 0U; j < count; ++j) {
        outputs_[indices[j]].last_operation = operations[j].result;
        if (operations[j].result == platform::PwmResult::success &&
            operations[j].state == platform::PwmApplyState::applied)
            confirmed_mask_ |= static_cast<std::uint16_t>(1U << indices[j]);
        else if (result == platform::PwmResult::success)
            result = operations[j].result == platform::PwmResult::success
                         ? platform::PwmResult::native_failure
                         : operations[j].result;
    }
    // Shared faults can invalidate an unchanged sibling after a write.
    for (std::size_t i = 0U; i < outputs_.size(); ++i) {
        if ((selected & (1U << i)) != 0U && status(i) != platform::PwmResult::success)
            confirmed_mask_ &= static_cast<std::uint16_t>(~(1U << i));
    }
    return result;
}

platform::PwmResult PwmOutputs::reconfigure(
    const LightSettings& settings, const bool activate, const std::uint16_t selected) {
    staged_count_ = 0U;
    retune.reset();
    retune = allocate_plan();
    if (!retune)
        return platform::PwmResult::no_memory;
    auto& changes = retune->changes;
    auto& indices = retune->indices;
    std::size_t count = 0U;
    for (std::size_t i = 0U; i < outputs_.size(); ++i) {
        if (!configured(i) || (selected & (1U << i)) == 0U)
            continue;
        const auto state = status(i);
        if (state != platform::PwmResult::success) {
            retune.reset();
            return state;
        }
        auto request = outputs_[i].request;
        request.frequency_hz = settings.carrier(i);
        request.min_resolution_bits = resolution(settings.range);
        request.initial_duty = applied(i);
        changes[count] = {outputs_[i].handle, request};
        indices[count++] = i;
    }
    if (count == 0U) {
        retune.reset();
        return platform::PwmResult::success;
    }
    auto& results = retune->results;
    platform::PwmPlanToken token;
    auto result = platform::pwm_prepare_reconfigure(changes.data(), count, token, results.data());
    // A tolerated prepare rejection must not turn a command into a partial
    // retune.
    for (std::size_t j = 0U; result == platform::PwmResult::success && j < count; ++j)
        result = results[j].result;
    if (result != platform::PwmResult::success) {
        retune.reset();
        return result;
    }
    staged_ = token;
    staged_count_ = count;
    return activate ? activate_reconfiguration() : platform::PwmResult::success;
}

platform::PwmResult PwmOutputs::activate_reconfiguration() {
    const auto count = staged_count_;
    if (count == 0U)
        return platform::PwmResult::success;
    if (!retune)
        return platform::PwmResult::stale_plan;
    const auto plan = std::move(retune);
    auto& results = plan->results;
    const auto& indices = plan->indices;
    const auto& changes = plan->changes;
    auto result = platform::pwm_activate(staged_, results.data(), count);
    for (std::size_t j = 0U; j < count; ++j) {
        if (result == platform::PwmResult::success &&
            results[j].result != platform::PwmResult::success)
            result = results[j].result;
        auto& output = outputs_[indices[j]];
        if (results[j].result == platform::PwmResult::success) {
            output.request = changes[j].request;
            output.handle = results[j].handle;
        }
        output.result = results[j].result;
        output.last_operation = results[j].result;
    }
    return result;
}
} // namespace newmota
