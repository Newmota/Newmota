/*
  entry.cpp - Beken application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/app.h"
#include "newmota/log.h"

#include <cstdint>

extern "C" {
#include "include.h"
#include "rtos_pub.h"
}

extern "C" void extended_app_waiting_for_launch();
extern "C" {
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
}

namespace {

// Beken's rtos_create_thread() accepts bytes; reserve margin for deep web commands.
constexpr std::uint32_t kNewmotaStackSize = 12288U;

/** @brief Runs Newmota in its own Beken FreeRTOS task. */
void NewmotaTask(void*) {
    // user_main_entry() is wrapped before BDK launches extended_app.  Wait for
    // that launch barrier so filesystem, radio, and other BDK services are
    // initialized before Newmota calls into their adapters.
    extended_app_waiting_for_launch();
    // The stock non-Matter BDK startup does not run C++ constructors.
    // Initialize the shared application singletons once before using them.
#if !CFG_SUPPORT_MATTER
    for (auto constructor = __init_array_start; constructor != __init_array_end; ++constructor) {
        (*constructor)();
    }
#endif
    newmota::run();
}

} // namespace

/** @brief Replaces BDK's unused user task with the post-BDK Newmota task. */
extern "C" void __wrap_user_main_entry() {
    const auto status = rtos_create_thread(nullptr, THD_APPLICATION_PRIORITY, "newmota",
        reinterpret_cast<beken_thread_function_t>(NewmotaTask), kNewmotaStackSize, nullptr);
    if (status != kNoErr) {
        newmota::AddLog(newmota::LogLevel::error, "RTM: Newmota task creation failed (%d)", status);
    }
}
