/*
  ble_sensors.h - Native BLE sensor decoding commands

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_sensors.h
 * @brief MI32Key, MI32Name, MI32Cfg and MI32Option for the native BLE sensor driver.
 */
#pragma once

#include "core/command/command.h"

#if USE_TIME
namespace newmota {
class TimeService;
/** @brief Supplies the clock behind the sensors' Time member; without it the member is omitted. */
void ble_sensors_attach_time(const TimeService* time);
} // namespace newmota
#endif

namespace newmota::commands {
/** @brief Handles Tasmota's MI32 configuration commands.
 * @return unknown for another command, invalid for bad arguments, or the operation result.
 */
[[nodiscard]] CommandStatus dispatch_ble_sensors(
    const CommandRequest& request, CommandOutput& output);
} // namespace newmota::commands
