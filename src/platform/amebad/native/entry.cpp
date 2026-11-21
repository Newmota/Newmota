/*
  entry.cpp - Realtek AmebaD application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/app.h"
#include "newmota/log.h"

#include "FreeRTOS.h"
#include "task.h"

#include <cstdint>

namespace {

/** @brief Reserves 16 KiB for nested web, command, and settings calls (32-bit words). */
constexpr std::uint16_t kTaskStackDepth = 4096;

/** @brief Runs Newmota in the task created after AmebaD startup. */
void NewmotaTask(void*) {
    newmota::run();
}

} // namespace

/** @brief Creates the Newmota task from the AmebaD SDK entry hook. */
extern "C" void newmota_entry() {
    if (xTaskCreate(NewmotaTask, "newmota", kTaskStackDepth, nullptr, tskIDLE_PRIORITY + 1,
            nullptr) != pdPASS) {
        newmota::AddLog(newmota::LogLevel::error, "RTM: task creation failed");
    }
}
