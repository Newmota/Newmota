/*
  matter_module.h - Berry entry points of the Matter stack

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

struct bvm;

namespace newmota {

class NetworkService;
class PowerService;

/** @brief Gives Network Commissioning the owner of the Wi-Fi credentials. */
void matter_module_bind_network(NetworkService* network);
/** @brief Gives the clusters the change counter of relays and lights. */
void matter_module_bind_power(const PowerService* power);

/** @brief Selects whether the stack starts; set once at boot from SetOption151. */
void matter_module_enable(bool enabled);
/** @brief Reports whether the stack wants the VM on this scheduler tick. */
[[nodiscard]] bool matter_module_pending(bool timer_tick);
/** @brief Starts the stack once the station is connected, then delivers queued messages. */
int dispatch_matter_events(bvm* vm);

} // namespace newmota
