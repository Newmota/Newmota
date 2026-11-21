/*
  edge_capture.cpp - ESP-IDF interrupt edge capture and atomic snapshots

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.cpp
 * @brief ESP-IDF interrupt edge capture and atomic snapshots.
 */

#include "newmota/platform/edge_capture_state.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <array>
#include <limits>

namespace newmota::platform::edge_capture {
namespace {
using State = detail::State;

std::array<State, 16> states{};
portMUX_TYPE capture_lock = portMUX_INITIALIZER_UNLOCKED;
std::uint32_t generation{1};
bool service_installed{};

/** @brief Captures integer counters only; protocol work stays on the application task. */
void IRAM_ATTR interrupt(void* argument) {
    auto& state = *static_cast<State*>(argument);
    const auto now = static_cast<std::uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL_ISR(&capture_lock);
    detail::record(state, now);
    portEXIT_CRITICAL_ISR(&capture_lock);
}

/** @brief Looks up a slot only while capture_lock is held. */
State* find_locked(Handle handle) {
    if (handle.slot >= states.size())
        return nullptr;
    auto& entry = states[handle.slot];
    return (entry.active || entry.faulted) && entry.generation == handle.generation ? &entry
                                                                                    : nullptr;
}
} // namespace

std::uint64_t monotonic_microseconds() {
    return static_cast<std::uint64_t>(esp_timer_get_time());
}

Result open(const Configuration& configuration, Handle& handle) {
    handle = {};
    if (!configuration.pin.valid() || unsigned(configuration.edge) > unsigned(Edge::both) ||
        unsigned(configuration.pull) > unsigned(GpioPull::down) ||
        (configuration.maximum_interval_us &&
            configuration.maximum_interval_us < configuration.minimum_interval_us))
        return Result::invalid_argument;
    const auto requirements =
        capability_mask(PinCapability::input) | capability_mask(PinCapability::interrupt);
    if (!gpio_available(configuration.pin) ||
        (gpio_capabilities(configuration.pin) & requirements) != requirements)
        return Result::unsupported;
    portENTER_CRITICAL(&capture_lock);
    bool duplicate{};
    for (const auto& entry : states)
        duplicate |= (entry.active || entry.faulted) &&
                     entry.configuration.pin.value() == configuration.pin.value();
    std::size_t slot{};
    while (slot < states.size() && (states[slot].active || states[slot].faulted))
        ++slot;
    portEXIT_CRITICAL(&capture_lock);
    if (duplicate || slot == states.size() || generation == UINT32_MAX)
        return Result::busy;
    if (!service_installed) {
        const auto result = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        // Another owner may already have installed the shared GPIO service.
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
            return Result::failed;
        service_installed = true;
    }
    auto& entry = states[slot];
    const auto id = ++generation;
    const auto configured = gpio_configure_input(configuration.pin, configuration.pull);
    if (configured != GpioResult::success) {
        if (configured == GpioResult::failed) {
            // Native GPIO failure may have changed pad state. Retain both the native slot
            // and the application owner's reservation instead of advertising safe release.
            portENTER_CRITICAL(&capture_lock);
            entry = {configuration, {}, id, false, true};
            portEXIT_CRITICAL(&capture_lock);
            return Result::failed;
        }
        return Result::unsupported;
    }
    portENTER_CRITICAL(&capture_lock);
    entry = {configuration, {}, id, true, false};
    portEXIT_CRITICAL(&capture_lock);
    const auto pin = static_cast<gpio_num_t>(configuration.pin.value());
    const auto edge = configuration.edge == Edge::rising    ? GPIO_INTR_POSEDGE
                      : configuration.edge == Edge::falling ? GPIO_INTR_NEGEDGE
                                                            : GPIO_INTR_ANYEDGE;
    bool added{};
    const bool ready = gpio_intr_disable(pin) == ESP_OK &&
                       gpio_set_intr_type(pin, edge) == ESP_OK &&
                       (added = gpio_isr_handler_add(pin, interrupt, &entry) == ESP_OK) &&
                       gpio_intr_enable(pin) == ESP_OK;
    if (!ready) {
        portENTER_CRITICAL(&capture_lock);
        entry.active = false;
        portEXIT_CRITICAL(&capture_lock);
        const bool disabled = gpio_intr_disable(pin) == ESP_OK;
        const bool removed = !added || gpio_isr_handler_remove(pin) == ESP_OK;
        portENTER_CRITICAL(&capture_lock);
        if (!disabled || !removed)
            entry.faulted = true;
        else
            entry = {};
        portEXIT_CRITICAL(&capture_lock);
        if (!disabled || !removed)
            return Result::failed;
        return Result::unsupported;
    }
    handle = {id, static_cast<std::uint8_t>(slot)};
    return Result::success;
}

Result window(Handle handle, const Window& config) {
    portENTER_CRITICAL(&capture_lock);
    const auto result = detail::window(find_locked(handle), config);
    portEXIT_CRITICAL(&capture_lock);
    return result;
}

Result snapshot(Handle handle, Snapshot& snapshot) {
    portENTER_CRITICAL(&capture_lock);
    auto* entry = find_locked(handle);
    const bool valid = entry && entry->active && !entry->faulted;
    if (valid)
        snapshot = entry->captured;
    portEXIT_CRITICAL(&capture_lock);
    return valid ? Result::success : Result::invalid_argument;
}

Result close(Handle handle) {
    portENTER_CRITICAL(&capture_lock);
    auto* entry = find_locked(handle);
    const auto pin =
        entry ? static_cast<gpio_num_t>(entry->configuration.pin.value()) : GPIO_NUM_NC;
    portEXIT_CRITICAL(&capture_lock);
    if (!entry)
        return Result::success;
    if (gpio_intr_disable(pin) != ESP_OK) {
        portENTER_CRITICAL(&capture_lock);
        entry->faulted = true;
        portEXIT_CRITICAL(&capture_lock);
        return Result::failed;
    }
    portENTER_CRITICAL(&capture_lock);
    entry->active = false;
    portEXIT_CRITICAL(&capture_lock);
    const bool removed = gpio_isr_handler_remove(pin) == ESP_OK;
    portENTER_CRITICAL(&capture_lock);
    if (removed)
        *entry = {};
    else
        entry->faulted = true;
    portEXIT_CRITICAL(&capture_lock);
    return removed ? Result::success : Result::failed;
}
} // namespace newmota::platform::edge_capture
