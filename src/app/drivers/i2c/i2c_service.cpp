/*
  i2c_service.cpp - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "drivers/i2c/i2c_service.h"
#include "newmota_features.h"
#include "newmota/platform/i2c.h"
#include "newmota/log.h"
#if USE_WEBCAM
#include "drivers/camera/camera_service.h"
#endif

namespace newmota {
/** @brief Activates complete SCL/SDA pairs from the resolved hardware owner. */
void initialize_i2c(const Hardware& hardware) {
#if USE_WEBCAM
    PinId sccb_scl{}, sccb_sda{};
    bool sccb_pending = camera::sccb_pins(hardware, sccb_scl, sccb_sda);
#endif
    for (std::uint8_t bus = 0; bus < platform::i2c::kBusCount; ++bus) {
        const auto* scl = hardware.find_function(PinRole::i2c_scl, bus + 1);
        const auto* sda = hardware.find_function(PinRole::i2c_sda, bus + 1);
        if (scl == nullptr && sda == nullptr) {
#if USE_WEBCAM
            // Tasmota wires the camera's SCCB as WEBCAM_SIOC/SIOD; it takes the first free bus.
            if (sccb_pending) {
                sccb_pending = false;
                if (platform::i2c::configure(bus, sccb_scl, sccb_sda, true) ==
                    platform::i2c::Result::success)
                    camera::set_sccb_bus(bus);
                else
                    AddLog(
                        LogLevel::error, "I2C: webcam SCCB bus %u unavailable", unsigned(bus + 1));
            }
#endif
            continue;
        }
        if (scl == nullptr || sda == nullptr ||
            platform::i2c::configure(bus, scl->pin, sda->pin) != platform::i2c::Result::success)
            AddLog(LogLevel::error, "I2C: bus %u unavailable", unsigned(bus + 1));
    }
#if USE_WEBCAM
    if (sccb_pending)
        AddLog(LogLevel::error, "I2C: no free bus for webcam SCCB");
#endif
}
} // namespace newmota
