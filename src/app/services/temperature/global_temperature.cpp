/*
  global_temperature.cpp - Canonical shared temperature lifetime

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  UpdateGlobalTemperature/ResetGlobalValues reference: Tasmota support.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature.cpp
 * @brief Owns shared global Celsius and the source 300-second validity clock without allocation.
 */
#include "services/temperature/global_temperature.h"

namespace newmota {
GlobalTemperature& global_temperature() {
    static GlobalTemperature owner;
    return owner;
}

void GlobalTemperature::reset() {
    celsius_ = 0;
    updated_at_ = now_ = 0;
    present_ = uptime_seen_ = update_valid_ = clock_seen_ = false;
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    manual_ = false;
    source_ = 0;
    selection_bucket_ = 0;
#endif
}

std::uint32_t GlobalTemperature::normalize(std::uint32_t now) {
    if (clock_seen_ && static_cast<std::int32_t>(now - now_) < 0)
        return now_;
    clock_seen_ = true;
    now_ = now;
    return now;
}

void GlobalTemperature::expire(std::uint32_t now) {
    now = normalize(now);
    /** Quantize to source uptime seconds while retaining unsigned millisecond-wrap safety. */
    const auto age = (std::uint64_t(std::uint32_t(now - updated_at_)) + updated_at_ % 1000) / 1000;
    if (age > 300) {
        present_ = false;
        update_valid_ = false;
    }
}

void GlobalTemperature::refresh(std::uint32_t now) {
    now = normalize(now);
    expire(now);
    uptime_seen_ = uptime_seen_ || now >= 1000;
    update_valid_ = uptime_seen_;
    updated_at_ = now;
}

void GlobalTemperature::observe(std::int32_t celsius, std::uint32_t now) {
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    if (manual_ || source_)
        return;
#endif
    refresh(now);
    celsius_ = celsius;
    present_ = true;
}

bool GlobalTemperature::read(std::uint32_t now, std::int32_t& celsius) {
    expire(now);
    if (!update_valid_ || !present_)
        return false;
    celsius = celsius_;
    return true;
}
#if USE_GLOBAL_TEMPERATURE_COMMANDS
bool GlobalTemperature::selection_due(std::uint32_t now, bool publication) {
    now = normalize(now);
    const auto bucket = now / 10000;
    const bool due = publication || bucket != selection_bucket_;
    selection_bucket_ = bucket;
    return due && source_ && !manual_;
}

void GlobalTemperature::selected(std::int32_t celsius, std::uint32_t now) {
    if (!source_ || manual_)
        return;
    refresh(now);
    celsius_ = celsius;
    present_ = true;
}

bool GlobalTemperature::set_manual(std::int32_t celsius, std::uint32_t now) {
    if (celsius < -50000 || celsius > 100000)
        return false;
    refresh(now);
    celsius_ = celsius;
    present_ = true;
    manual_ = true;
    return true;
}

bool GlobalTemperature::value(std::uint32_t now, std::int32_t& celsius) {
    expire(now);
    if (!present_)
        return false;
    celsius = celsius_;
    return true;
}
#endif
} // namespace newmota
