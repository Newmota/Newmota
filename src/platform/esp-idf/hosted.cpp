/*
  hosted.cpp - ESP-Hosted Wi-Fi companion of the ESP32-P4 for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file hosted.cpp
 * @brief Ported from Tasmota's xdrv_84_esp32_hosted.ino and Arduino's esp32-hal-hosted.c.
 *
 * Arduino's order: SDIO pins, esp_hosted_init(), esp_hosted_connect_to_slave(). The vendored
 * component has no constructor starting the transport early (see its README).
 */
#include "newmota/platform/hosted.h"

#include "newmota/log.h"

#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"

#include <cstring>

namespace newmota::platform::hosted {
namespace {
Pins configured{};
bool started{};
Version companion{};
bool companion_asked{};

void apply(gpio_pin_t& target, const PinId pin) {
    if (pin.valid())
        target.pin = pin.value();
}

Result result(const esp_err_t error) {
    return error == ESP_OK ? Result::success : Result::failed;
}
} // namespace

void set_pins(const Pins& pins) {
    configured = pins;
}

Result start() {
    if (started)
        return Result::success;
    struct esp_hosted_sdio_config config = INIT_DEFAULT_HOST_SDIO_CONFIG();
    apply(config.pin_clk, configured.clk);
    apply(config.pin_cmd, configured.cmd);
    apply(config.pin_d0, configured.d0);
    apply(config.pin_d1, configured.d1);
    apply(config.pin_d2, configured.d2);
    apply(config.pin_d3, configured.d3);
    apply(config.pin_reset, configured.reset);
    AddLog(LogLevel::debug,
        "HST: Hosted MCU GPIO%02d(CLK), GPIO%02d(CMD), GPIO%02d(D0), GPIO%02d(D1), GPIO%02d(D2), "
        "GPIO%02d(D3) and GPIO%02d(RST)",
        config.pin_clk.pin, config.pin_cmd.pin, config.pin_d0.pin, config.pin_d1.pin,
        config.pin_d2.pin, config.pin_d3.pin, config.pin_reset.pin);
    if (esp_hosted_sdio_set_config(&config) != ESP_TRANSPORT_OK || esp_hosted_init() != ESP_OK) {
        AddLog(LogLevel::error, "HST: transport set-up failed");
        return Result::failed;
    }
    if (esp_hosted_connect_to_slave() != ESP_OK) {
        AddLog(LogLevel::error, "HST: no answer from the Hosted MCU");
        (void)esp_hosted_deinit();
        return Result::failed;
    }
    started = true;
    return Result::success;
}

bool running() {
    return started && esp_hosted_is_config_valid();
}

Version host_version() {
    return Version(ESP_HOSTED_VERSION_MAJOR_1) << 16 | Version(ESP_HOSTED_VERSION_MINOR_1) << 8 |
           Version(ESP_HOSTED_VERSION_PATCH_1);
}

Version companion_version() {
    if (!running())
        return 0;
    if (!companion_asked) {
        companion_asked = true;
        // Firmware before 0.0.6 lacks the call; Tasmota then reports 0.0.6.
        companion = 6;
        esp_hosted_coprocessor_fwver_t info{};
        if (esp_hosted_get_coprocessor_fwversion(&info) == ESP_OK)
            companion = Version(info.major1) << 16 | Version(info.minor1 & 0xFF) << 8 |
                        Version(info.patch1 & 0xFF);
    }
    return companion;
}

void companion_chip(char* name, const std::size_t capacity) {
    if (capacity == 0U)
        return;
    std::strncpy(name, CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET, capacity - 1U);
    name[capacity - 1U] = '\0';
    if (!running() || companion_version() < 0x00020C02U) // asking needs 2.12.2
        return;
    char reported[30]{};
    std::uint32_t chip_id{};
    if (esp_hosted_get_cp_info(&chip_id, reported, sizeof(reported) - 1U) == ESP_OK &&
        reported[0] != '\0') {
        std::strncpy(name, reported, capacity - 1U);
        name[capacity - 1U] = '\0';
    }
}

Result update_begin() {
    return running() ? result(esp_hosted_slave_ota_begin()) : Result::unavailable;
}

Result update_write(const std::uint8_t* data, const std::size_t size) {
    return result(esp_hosted_slave_ota_write(const_cast<std::uint8_t*>(data), std::uint32_t(size)));
}

Result update_end() {
    return result(esp_hosted_slave_ota_end());
}

Result update_activate() {
    // Companions before 2.6.0 boot the new image on their own after the end.
    if (companion_version() < 0x00020600U)
        return Result::success;
    return result(esp_hosted_slave_ota_activate());
}

} // namespace newmota::platform::hosted
