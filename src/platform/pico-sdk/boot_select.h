/*
  boot_select.h - internal sticky Safeboot selection for the Pico SDK platform

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

namespace newmota::platform::pico_detail {

/**
 * @brief Chains into Safeboot when a sticky boot request is pending.
 *
 * Runs from main() before newmota::run(), while only clocks and XIP are
 * active. Safeboot occupies flash offset zero and chains into the application
 * unless its persistent recovery marker is armed. RP2350 Safeboot owns its
 * resident partition table; RP2040 Safeboot uses a compiled layout identity.
 * A failed application chain arms the marker before services start, keeping
 * the recovery writer available for repair.
 */
void honor_sticky_boot_request();

/** @brief Reports whether the persistent marker keeps the device in Safeboot. */
[[nodiscard]] bool recovery_mode_requested();

/** @brief Clears the recovery marker after a fully verified update. */
[[nodiscard]] bool clear_recovery_mode();

/** @brief Returns the resident RP2040 Safeboot's compiled layout identity. */
[[nodiscard]] const char* compiled_layout_identity();

} // namespace newmota::platform::pico_detail
