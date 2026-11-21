/*
  lvgl_runtime.h - cooperative LVGL lifecycle for Berry

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file lvgl_runtime.h
 * @brief Declares the Berry-owned lazy LVGL lifecycle.
 */

#pragma once

struct bvm;

namespace newmota {

/**
 * @brief Installs the lazy lv.start() entry point and embedded font lookups.
 * @return true when the importable bindings are ready.
 *
 * The Berry service serializes all LVGL work on the application loop. When
 * USE_DISPLAY is enabled, the runtime attaches an RGB565 partial buffer to the
 * descriptor-selected panel on explicit start, or automatically unless SetOption135 is set or
 * the boot guard reports a boot loop.
 * Without a panel, only an explicit start initializes the core.
 */
[[nodiscard]] bool install_lvgl_runtime(bvm* vm);

/** @brief Runs LVGL timers inside the Berry service's protected call/deadline. */
int poll_lvgl_runtime(bvm* vm);

/** @brief Disables Berry callbacks and releases LVGL before VM destruction. */
void remove_lvgl_runtime(bvm* vm);

} // namespace newmota
