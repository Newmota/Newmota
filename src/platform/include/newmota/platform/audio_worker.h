/*
  audio_worker.h - Native audio worker hosting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_worker.h
 * @brief Hosts the same bounded capture step in an RTOS task or on Pico core 0.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace newmota::platform::audio_worker {
struct Worker {
    void* native{};
};

using Step = void (*)(void*);
/** @brief Starts periodic capture/codec work. The callback must return promptly.
 * @param stack_bytes Task stack reservation; Pico uses its core-0 application stack.
 * @return False if worker allocation or native task creation fails.
 */
bool start(Worker&, Step, void* context, std::size_t stack_bytes, std::uint32_t period_ms);
/** @brief Joins before returning; no callback can retain context after this call. */
void stop(Worker&);
/** @brief Services a due core-0 step on Pico; RTOS hosts need no polling. */
void poll(Worker&);
/** @brief Returns minimum free stack bytes (shared core-0 stack on Pico), or zero if unavailable.
 */
std::uint32_t stack_free_bytes(const Worker&);
} // namespace newmota::platform::audio_worker
