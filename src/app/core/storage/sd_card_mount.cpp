/*
  sd_card_mount.cpp - boot-time SD card mount from the GPIO configuration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/sd_card_mount.h"

#include "core/hardware/hardware.h"
#include "core/storage/filesystem.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/sd_card.h"
#include "support/number.h"

#include <cstddef>
#include <cstdint>

namespace newmota {
namespace {

/** @brief The SDK pin of one active function instance, or -1. */
int assigned(const Hardware& hardware, const std::uint16_t base, const std::uint8_t instance = 1U) {
    const auto function = generated::gpio::encode(base, instance);
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function && binding->active())
            return binding->pin.value();
    }
    return -1;
}

/**
 * @brief Resolves Tasmota's SD pins.
 * @return false when the configuration assigns no SD card.
 */
bool resolve(const Hardware& hardware, platform::SdCardConfig& config) {
    namespace gpio = generated::gpio;
    // Tasmota's UfsCheckSDCardInit: SDCard CS2 on SPI bus 2 replaces CS1 on bus 1.
    for (std::uint8_t instance = 1U; instance <= 2U; ++instance) {
        const int cs = assigned(hardware, gpio::kGpioSdcardCsBase, instance);
        if (cs < 0)
            continue;
        config.mode = platform::SdCardConfig::Mode::spi;
        config.spi_bus = static_cast<std::uint8_t>(instance - 1U);
        config.cs = cs;
        config.clock = assigned(hardware, gpio::kGpioSpiClkBase, instance);
        config.mosi = assigned(hardware, gpio::kGpioSpiMosiBase, instance);
        config.miso = assigned(hardware, gpio::kGpioSpiMisoBase, instance);
    }
    if (config.cs >= 0)
        return true;
    config.command = assigned(hardware, gpio::kGpioSdioCmdBase);
    config.clock = assigned(hardware, gpio::kGpioSdioClkBase);
    config.d0 = assigned(hardware, gpio::kGpioSdioD0Base);
    if (config.command < 0 || config.clock < 0 || config.d0 < 0)
        return false;
    config.mode = platform::SdCardConfig::Mode::sdio;
    config.d1 = assigned(hardware, gpio::kGpioSdioD1Base);
    config.d2 = assigned(hardware, gpio::kGpioSdioD2Base);
    config.d3 = assigned(hardware, gpio::kGpioSdioD3Base);
    if (config.d1 < 0 || config.d2 < 0 || config.d3 < 0)
        config.d1 = config.d2 = config.d3 = -1; // Four-bit needs all of D1 to D3.
    return true;
}

} // namespace

void mount_sd_card(const Hardware& hardware) {
    platform::SdCardConfig config{};
    if (!resolve(hardware, config))
        return;
    const bool spi = config.mode == platform::SdCardConfig::Mode::spi;
    const auto result = filesystem::mount_sd(config);
    if (result == filesystem::MountResult::mounted) {
        filesystem::SpaceInfo space{};
        const auto free_kib = filesystem::space(space, filesystem::Volume::sd)
                                  ? (space.total_bytes - space.used_bytes) / 1024U
                                  : 0U;
        if (spi)
            AddLog(LogLevel::info, "UFS: SDCard mounted (SPI bus%u) with %s kB free",
                static_cast<unsigned>(config.spi_bus) + 1U,
                newmota::support::decimal(std::uint64_t(free_kib)).text);
        else
            AddLog(LogLevel::info, "UFS: SDCard mounted (SDIO %u-bit) with %s kB free",
                config.d1 >= 0 ? 4U : 1U, newmota::support::decimal(std::uint64_t(free_kib)).text);
        return;
    }
    switch (result) {
    case filesystem::MountResult::unavailable:
        AddLog(LogLevel::error, "UFS: SDCard %s is unavailable on this chip", spi ? "SPI" : "SDIO");
        break;
    case filesystem::MountResult::invalid_pins:
        AddLog(LogLevel::error, "UFS: SDCard pins cannot be used (missing, unroutable or taken)");
        break;
    case filesystem::MountResult::no_card:
        AddLog(LogLevel::info, "UFS: no SDCard found");
        break;
    case filesystem::MountResult::no_filesystem:
        AddLog(LogLevel::error, "UFS: SDCard has no FAT volume; not formatting");
        break;
    default:
        AddLog(LogLevel::error, "UFS: SDCard mount failed (%d)", filesystem::last_error());
        break;
    }
}

} // namespace newmota
