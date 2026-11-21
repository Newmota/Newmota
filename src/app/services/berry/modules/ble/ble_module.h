/*
  ble_module.h - Berry BLE module interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once
struct bvm;

namespace newmota {
/** @brief Remembers the VM that owns the script buffer's GC root. */
void install_ble_module(bvm* vm);
/** @brief Releases the buffer root and closes the client before VM destruction. */
void remove_ble_module();
/** @brief Reports whether a registered script has an event waiting. */
[[nodiscard]] bool ble_module_pending();
/** @brief Native protected entry that delivers waiting events to the script callback. */
int dispatch_ble_events(bvm* vm);
} // namespace newmota
