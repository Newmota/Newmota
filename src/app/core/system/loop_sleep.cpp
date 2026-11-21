/*
  loop_sleep.cpp - main-loop sleep state and load average for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LoadAvg formula adapted from Tasmota tasmota.ino loop(),
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

#include "core/system/loop_sleep.h"

namespace newmota::loop_sleep {
namespace {

Status current{};
std::uint32_t load_milli{}; // LoadAvg in thousandths of a percent

} // namespace

void record_pass(const std::uint32_t activity_ms, const std::uint8_t sleep_ms, const bool normal) {
    // Tasmota: one loop period is Sleep milliseconds (at least 1); the average spans one second.
    // Tasmota averages whole percent, which truncates a short pass's share to zero; the same
    // average kept in thousandths lets those shares add up. The cap keeps 100000 * activity in
    // 32 bits.
    const std::uint32_t activity = activity_ms == 0U      ? 1U
                                   : activity_ms < 40000U ? activity_ms
                                                          : 40000U;
    const std::uint32_t loop_delay = sleep_ms != 0U ? sleep_ms : 1U;
    const std::uint32_t loops_per_second = 1000U / loop_delay;
    const std::uint32_t this_cycle_ratio = 100000U * activity / loop_delay;
    load_milli = load_milli - load_milli / loops_per_second + this_cycle_ratio / loops_per_second;
    current.load = (load_milli + 500U) / 1000U;
    current.sleep = sleep_ms;
    current.normal = normal;
}

Status status() {
    return current;
}

const char* mode_name(const bool normal) {
    return normal ? "Normal" : "Dynamic";
}

} // namespace newmota::loop_sleep
