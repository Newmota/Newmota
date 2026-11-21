/*
  pwm.cpp - public PWM API dispatch to the resolved native adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/pwm.h"
#include "newmota/platform/pwm_detail.h"

#include <memory>
#include <new>

namespace newmota::platform {
namespace {

// Created by the first prepare, so a device without PWM outputs never holds it.
std::unique_ptr<detail::PwmController> g_controller;

/** @brief Returns the single PWM owner, creating it on first use; nullptr when out of memory. */
[[nodiscard]] detail::PwmController* create_controller() {
    if (!g_controller) {
        g_controller.reset(new (std::nothrow)
                detail::PwmController{detail::pwm_native_backend(), detail::pwm_native_context()});
    }
    return g_controller.get();
}

} // namespace

/** @brief Stages new PWM outputs without changing native hardware. */
PwmResult pwm_prepare(const PwmRequest* const requests, const std::size_t count, PwmPlanToken& plan,
    PwmRequestResult* const results) {
    auto* const controller = create_controller();
    if (controller == nullptr) {
        plan = {};
        return PwmResult::no_memory;
    }
    return controller->prepare(requests, count, plan, results);
}

/** @brief Stages a complete shared-timing reconfiguration. */
PwmResult pwm_prepare_reconfigure(const PwmReconfiguration* const updates, const std::size_t count,
    PwmPlanToken& plan, PwmRequestResult* const results) {
    if (!g_controller) {
        plan = {};
        return PwmResult::stale_handle;
    }
    return g_controller->prepare_reconfigure(updates, count, plan, results);
}

/** @brief Activates the retained native plan for an output batch. */
PwmResult pwm_activate(
    const PwmPlanToken plan, PwmRequestResult* const results, const std::size_t result_capacity) {
    return g_controller ? g_controller->activate(plan, results, result_capacity)
                        : PwmResult::stale_plan;
}

/** @brief Applies a checked batch of normalized duties. */
PwmResult pwm_write_batch(const PwmDutyUpdate* const updates, const std::size_t count,
    PwmOperationResult* const results) {
    return g_controller ? g_controller->write_batch(updates, count, results)
                        : PwmResult::stale_handle;
}

/** @brief Stops and releases one generation-checked output handle. */
PwmResult pwm_release(const PwmHandle handle) {
    return g_controller ? g_controller->release(handle) : PwmResult::stale_handle;
}

/** @brief Reports effective retained settings and shared-resource state. */
PwmResult pwm_get_status(const PwmHandle handle, PwmOutputStatus& status) {
    return g_controller ? g_controller->get_status(handle, status) : PwmResult::stale_handle;
}

} // namespace newmota::platform
