/*
  autoexec.cpp - protected Berry startup script service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/autoexec.h"

#include "core/storage/filesystem.h"
#include "core/system/boot_guard.h"
#include "newmota/log.h"
#include "services/berry/berry_service.h"

namespace newmota {
namespace {

constexpr char kAutoexecPath[] = "/autoexec.be";

} // namespace

/** @brief Arms autoexec.be for one protected normal-application startup. */
void AutoexecService::initialize(const bool disabled) {
    const auto restart = boot_guard().restart();
    fast_restart_count_ = restart.count;
    state_ = AutoexecState::waiting;
    disabled_ = disabled;
    tracking_available_ = restart.available;
}

/** @brief Runs autoexec.be once unless disabled or the boot guard reports a loop. */
void AutoexecService::poll() {
    if (state_ == AutoexecState::waiting) {
        if (disabled_) {
            state_ = AutoexecState::disabled;
            AddLog(LogLevel::info, "BRY: autoexec.be disabled by SetOption153");
        } else if (!filesystem::is_mounted()) {
            state_ = AutoexecState::storage_unavailable;
        } else if (!tracking_available_) {
            state_ = AutoexecState::protection_unavailable;
            AddLog(
                LogLevel::error, "BRY: autoexec.be skipped; retained boot protection unavailable");
        } else if (boot_guard().reduced()) {
            state_ = AutoexecState::boot_loop;
            AddLog(LogLevel::error, "BRY: autoexec.be skipped after %u fast boots",
                static_cast<unsigned int>(fast_restart_count_));
        } else {
            switch (berry_.execute_file(kAutoexecPath)) {
            case BerryScriptResult::executed:
                state_ = AutoexecState::executed;
                break;
            case BerryScriptResult::not_found:
                state_ = AutoexecState::not_found;
                break;
            case BerryScriptResult::unavailable:
                state_ = AutoexecState::storage_unavailable;
                break;
            case BerryScriptResult::failed:
                state_ = AutoexecState::failed;
                break;
            }
        }
    }
}

} // namespace newmota
