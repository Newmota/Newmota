/*
  boot_trace.h - retained boot-progress trace for Pico SDK qualification

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota::platform::pico_detail {

/**
 * @brief Ordered boot points that a qualification build distinguishes.
 *
 * The values are retained in a watchdog scratch register and reported by the
 * following boot, so they are part of the diagnostic contract and must only be
 * appended. A value of zero means "no stage recorded".
 */
enum class BootStage : std::uint8_t {
    /** @brief Image entry, before the application starts its services. */
    entry = 1,
    /** @brief The console backend is initialized and logging. */
    console_ready,
    /** @brief The LittleFS block device geometry resolved and was opened. */
    filesystem_mapped,
    /** @brief Reading the LittleFS region out of physical flash. */
    filesystem_read,
    /** @brief Inside one application log line. */
    logging,
    /** @brief Entered a physical flash sector erase. */
    flash_erase_enter,
    /** @brief A physical flash sector erase returned. */
    flash_erase_done,
    /** @brief Entered a physical flash page program. */
    flash_program_enter,
    /** @brief A physical flash page program returned. */
    flash_program_done,
    /** @brief Reached the cooperative application loop. */
    application_loop,
};

/**
 * @brief Reports the previous boot, then starts this boot's trace.
 *
 * Call once, after the console backend is initialized. The Pico USB standard
 * I/O initialization is not idempotent, so the report is deliberately not
 * emitted from image entry: only the application's console may own it. Prints
 * and clears the retained record of the previous run, arms the watchdog that
 * turns a stall into a restart, and records BootStage::entry.
 */
void begin_boot_trace();

/**
 * @brief Records the latest boot point reached and refreshes the watchdog.
 *
 * Every qualification build marks its progress, so a stall is always
 * attributable to the step that did not complete.
 */
void mark_boot_stage(BootStage stage);

/**
 * @brief Services the trace from the cooperative application loop.
 *
 * Called once per loop iteration. Records BootStage::application_loop on the
 * first call and refreshes the watchdog thereafter.
 */
void service_boot_trace();

} // namespace newmota::platform::pico_detail
