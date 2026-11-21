/*
  hosted_service.h - ESP-Hosted companion pins and commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file hosted_service.h
 * @brief Ported from Tasmota's xdrv_84_esp32_hosted.ino: Hosted, HostedOta and HostedLoad.
 *
 * HostedLoad streams a filesystem image to the companion a few KiB per loop pass. HostedOta
 * downloads the image with wget first and then loads it, so it has wget's plain-HTTP rule.
 * After a successful update the device restarts, as in Tasmota: the companion reboots into
 * the new firmware and the host has to bring the link up again.
 */
#pragma once
#include "core/command/command.h"
#include "core/hardware/hardware.h"

#include <cstdint>

namespace newmota {
class RestartController;
class Settings;
class WgetService;
} // namespace newmota

namespace newmota::hosted {

/** @brief Hands the template's HSDIO pins to the platform before Wi-Fi starts. */
void initialize(const Hardware& hardware);

#if defined(NEWMOTA_IMAGE_APPLICATION)
/** @brief Binds the OtaUrl source, the downloader (may be null) and the restart. */
void bind(const Settings& settings, WgetService* wget, RestartController& restart);

/** @brief Handles Hosted, HostedOta and HostedLoad; unknown for anything else. */
[[nodiscard]] CommandStatus command(const CommandRequest& request, CommandOutput& output);

/** @brief Advances a download hand-off or one bounded slice of a companion update. */
void poll(std::uint32_t now);

/** @brief Whether an update owns the companion link. */
[[nodiscard]] bool busy();
#endif

} // namespace newmota::hosted
