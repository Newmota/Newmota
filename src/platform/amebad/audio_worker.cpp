/*
  audio_worker.cpp - Native audio worker hosting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_worker.cpp
 * @brief Uses delay-until pacing and joins capture before application-owned cleanup.
 */
#include "newmota/platform/audio_worker.h"
#include "FreeRTOS.h"
#include "task.h"
#include <atomic>
#include <new>

namespace newmota::platform::audio_worker {
namespace {
struct Native {
    Step step{};
    void* context{};
    TickType_t period{};
    std::atomic<bool> stopping{false}, exited{false};
    std::atomic<std::uint32_t> stack_free{0};
};

void run(void* context) {
    auto& worker = *static_cast<Native*>(context);
    auto wake = xTaskGetTickCount();
    while (!worker.stopping.load(std::memory_order_acquire)) {
        worker.step(worker.context);
        worker.stack_free.store(
            uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t), std::memory_order_relaxed);
        // A late codec pass starts a fresh period instead of spinning to catch up.
        const auto now = xTaskGetTickCount();
        if (now - wake >= worker.period)
            wake = now;
        vTaskDelayUntil(&wake, worker.period);
    }
    worker.exited.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
} // namespace

bool start(
    Worker& worker, Step step, void* context, std::size_t stack_bytes, std::uint32_t period_ms) {
    if (worker.native || !step || !period_ms)
        return false;
    auto* native = new (std::nothrow) Native();
    if (!native)
        return false;
    native->step = step;
    native->context = context;
    native->period = pdMS_TO_TICKS(period_ms);
    if (!native->period)
        native->period = 1;
    if (xTaskCreate(run, "audio-capture",
            (stack_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t), native,
            tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        delete native;
        return false;
    }
    worker.native = native;
    return true;
}

void stop(Worker& worker) {
    auto* native = static_cast<Native*>(worker.native);
    if (!native)
        return;
    native->stopping.store(true, std::memory_order_release);
    while (!native->exited.load(std::memory_order_acquire))
        vTaskDelay(1);
    delete native;
    worker = {};
}

void poll(Worker&) {}

std::uint32_t stack_free_bytes(const Worker& worker) {
    auto* native = static_cast<const Native*>(worker.native);
    return native ? native->stack_free.load(std::memory_order_relaxed) : 0;
}
} // namespace newmota::platform::audio_worker
