/*
  audio_worker.cpp - Native audio worker hosting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_worker.cpp
 * @brief Runs capture on core 0 without an RTOS or interrupt-context codec calls.
 */
#include "newmota/platform/audio_worker.h"
#include "newmota/platform/platform.h"
#include "memory_info.h"
#include <new>

namespace newmota::platform::audio_worker {
namespace {
struct Native {
    Step step{};
    void* context{};
    std::uint32_t period{}, due{};
};
} // namespace

bool start(Worker& worker, Step step, void* context, std::size_t, std::uint32_t period_ms) {
    if (worker.native || !step || !period_ms)
        return false;
    worker.native = new (std::nothrow) Native{step, context, period_ms, monotonic_milliseconds()};
    return worker.native;
}

void stop(Worker& worker) {
    // All calls run on core 0; a callback has returned before stop can be entered.
    delete static_cast<Native*>(worker.native);
    worker = {};
}

void poll(Worker& worker) {
    auto* native = static_cast<Native*>(worker.native);
    if (!native)
        return;
    const auto now = monotonic_milliseconds();
    if (static_cast<std::int32_t>(now - native->due) < 0)
        return;
    native->due = now + native->period;
    native->step(native->context);
}

std::uint32_t stack_free_bytes(const Worker&) {
    return pico_detail::stack_watermark_bytes();
}
} // namespace newmota::platform::audio_worker
