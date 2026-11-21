/*
  widget_module.h - Berry widget module for the dashboard

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widget_module.h
 * @brief VM lifecycle and action delivery for `import widget`.
 */
#pragma once
struct bvm;

namespace newmota {
/** @brief Releases the callback root and every widget before the VM is destroyed. */
void remove_widget_module();
/** @brief Reports whether a control action waits for delivery. */
[[nodiscard]] bool widget_module_pending();
/** @brief Native protected entry that delivers one action as `callback(part, value)`. */
int dispatch_widget_action(bvm* vm);
} // namespace newmota
