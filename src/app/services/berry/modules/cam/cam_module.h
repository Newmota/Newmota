/*
  cam_module.h - Berry cam module and Wc commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file cam_module.h
 * @brief Registers the `cam` module and runs Tasmota's `Wc*` commands through the sensor.
 */
#pragma once
#include "core/command/command.h"

#include <string_view>

struct bvm;

namespace newmota {
/** @brief Powers the camera down before VM teardown; the sensor object belongs to the VM. */
void remove_cam_module();

/**
 * @brief Native body of the camera commands, run as a protected Berry call.
 *
 * Stack argument 1 is a pointer to CameraCommand.
 */
int dispatch_camera_command(bvm* vm);

/** @brief One command handed to dispatch_camera_command(). */
struct CameraCommand final {
    const CommandRequest* request;
    CommandOutput* output;
    CommandStatus status{CommandStatus::unknown};
};

/** @brief Reports whether a command name belongs to the camera. */
[[nodiscard]] bool camera_command_name(std::string_view name);
} // namespace newmota
