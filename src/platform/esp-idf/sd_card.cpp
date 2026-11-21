/*
  sd_card.cpp - ESP-IDF SD card transports for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sd_card.cpp
 * @brief SD cards over a shared SPI host (`esp_driver_sdspi`) or the SDMMC host.
 *
 * Both transports produce an `sdmmc_card_t`, so sectors move through the same
 * `sdmmc_read_sectors()`/`sdmmc_write_sectors()` calls, which bounce buffers that are not
 * DMA-capable. No ESP-IDF VFS or FatFs is involved.
 */

#include "newmota/platform/sd_card.h"

#include "newmota/log.h"
#include "spi_host.h"

#include "driver/sdspi_host.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#include "soc/sdmmc_pins.h"
#endif
#if SOC_GP_LDO_SUPPORTED && SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include <initializer_list>

namespace newmota::platform {
namespace {

/** @brief Upper SD clock for both transports, as the plan fixes it. */
constexpr int kClockKhz = 20000;
/** @brief A shared-bus transfer that holds several sectors and a command frame. */
constexpr int kSpiTransferBytes = 4000;
#if SOC_GP_LDO_SUPPORTED && SOC_SDMMC_IO_POWER_EXTERNAL
/** @brief The P4 powers SDMMC slot 0's dedicated pins from on-chip LDO channel 4. */
constexpr int kSlot0PowerLdo = 4;
#endif

static_assert(kSpiTransferBytes <= spi_host::kSharedTransferBytes);

struct Card final {
    sdmmc_host_t host{};
    sdmmc_card_t card{};
    SdCardConfig::Mode mode{};
    spi_host::Lease lease{};
    sdspi_dev_handle_t spi{-1};
    int slot{-1};
#if SOC_GP_LDO_SUPPORTED && SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_handle_t power{};
#endif
    bool open{};
};

Card card;

bool read_sectors(
    void* const context, const std::uint32_t sector, void* const buffer, const std::size_t count) {
    auto& opened = *static_cast<Card*>(context);
    return sdmmc_read_sectors(&opened.card, buffer, sector, count) == ESP_OK;
}

bool write_sectors(void* const context, const std::uint32_t sector, const void* const buffer,
    const std::size_t count) {
    auto& opened = *static_cast<Card*>(context);
    return sdmmc_write_sectors(&opened.card, buffer, sector, count) == ESP_OK;
}

/** @brief sdmmc_write_sectors() returns after the card has left its busy state. */
bool sync_card(void*) {
    return true;
}

/** @brief Undoes whatever part of the transport an open reached. */
void release(Card& opened) {
    if (opened.mode == SdCardConfig::Mode::spi) {
        if (opened.spi >= 0)
            (void)sdspi_host_remove_device(opened.spi);
        if (opened.lease.valid())
            (void)spi_host::release(opened.lease);
    }
#if SOC_SDMMC_HOST_SUPPORTED
    if (opened.mode == SdCardConfig::Mode::sdio && opened.slot >= 0)
        (void)sdmmc_host_deinit_slot(opened.slot); // Leaves ESP-Hosted's slot running.
#endif
#if SOC_GP_LDO_SUPPORTED && SOC_SDMMC_IO_POWER_EXTERNAL
    if (opened.power != nullptr)
        (void)sd_pwr_ctrl_del_on_chip_ldo(opened.power);
#endif
    opened = {};
}

void close_card(void* const context) {
    release(*static_cast<Card*>(context));
}

/** @brief Maps a failed card identification to "nobody answered" or a hard failure. */
[[nodiscard]] SdCardResult card_result(const esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return SdCardResult::opened;
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_NOT_SUPPORTED:
        return SdCardResult::no_card;
    default:
        return SdCardResult::failed;
    }
}

[[nodiscard]] SdCardResult open_spi(const SdCardConfig& config) {
#if SOC_SPI_PERIPH_NUM > 2
    if (config.spi_bus > 1U)
        return SdCardResult::unsupported;
    const auto host = config.spi_bus == 1U ? SPI3_HOST : SPI2_HOST;
#else
    if (config.spi_bus != 0U)
        return SdCardResult::unsupported;
    const auto host = SPI2_HOST;
#endif
    if (config.cs < 0 || config.clock < 0 || config.mosi < 0 || config.miso < 0)
        return SdCardResult::invalid_pins;
    spi_bus_config_t bus{};
    bus.sclk_io_num = config.clock;
    bus.mosi_io_num = config.mosi;
    bus.miso_io_num = config.miso;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kSpiTransferBytes;
    // A display or Ethernet controller on the same wires keeps its own CS.
    switch (spi_host::acquire(host, bus, spi_host::Use::shared, card.lease)) {
    case spi_host::Result::success:
        break;
    case spi_host::Result::unsupported:
        return SdCardResult::unsupported;
    case spi_host::Result::busy:
    case spi_host::Result::conflict:
        return SdCardResult::invalid_pins;
    case spi_host::Result::no_memory:
    case spi_host::Result::failed:
        return SdCardResult::failed;
    }
    if (sdspi_host_init() != ESP_OK)
        return SdCardResult::failed;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id = host;
    device.gpio_cs = static_cast<gpio_num_t>(config.cs);
    if (sdspi_host_init_device(&device, &card.spi) != ESP_OK) {
        card.spi = -1;
        return SdCardResult::invalid_pins;
    }
    card.host = SDSPI_HOST_DEFAULT();
    card.host.slot = card.spi;
    card.host.max_freq_khz = kClockKhz;
    return card_result(sdmmc_card_init(&card.host, &card.card));
}

#if SOC_SDMMC_HOST_SUPPORTED
/** @brief Reports whether the pins equal one slot's dedicated IOMUX pins. */
[[nodiscard]] bool iomux_pins(const SdCardConfig& config, const bool four_bit, const int clock,
    const int command, const int d0, const int d1, const int d2, const int d3) {
    return config.clock == clock && config.command == command && config.d0 == d0 &&
           (!four_bit || (config.d1 == d1 && config.d2 == d2 && config.d3 == d3));
}

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
/** @brief ESP-Hosted's Wi-Fi companion owns this slot; ESP-IDF would re-initialize it. */
constexpr int kHostedSlot = CONFIG_ESP_HOSTED_SDIO_SLOT;
#else
constexpr int kHostedSlot = -1;
#endif

/** @brief A slot routes through the GPIO matrix when the chip has one and the slot has no
 * dedicated pins (ESP32-S3 both slots, ESP32-P4 slot 1). */
[[nodiscard]] constexpr bool matrix_slot(const int slot) {
#if SOC_SDMMC_USE_GPIO_MATRIX
#if defined(SDMMC_SLOT0_IOMUX_PIN_NUM_CLK)
    if (slot == 0)
        return false;
#endif
#if defined(SDMMC_SLOT1_IOMUX_PIN_NUM_CLK)
    if (slot == 1)
        return false;
#endif
    return slot == 0 || slot == 1;
#else
    (void)slot;
    return false;
#endif
}

/** @brief Slot choice for the card's pins. */
struct SlotChoice final {
    int slot{-1};       /**< 0 or 1 when usable. */
    bool hosted{false}; /**< Only ESP-Hosted's slot could route the pins. */
};

/**
 * @brief Chooses the slot that can route the pins without taking ESP-Hosted's slot.
 *
 * Dedicated IOMUX pins name their slot. Other pins take a GPIO-matrix slot, slot 1 first.
 */
[[nodiscard]] SlotChoice sdmmc_slot(const SdCardConfig& config, const bool four_bit) {
    int dedicated = -1;
#if defined(SDMMC_SLOT0_IOMUX_PIN_NUM_CLK)
    if (iomux_pins(config, four_bit, SDMMC_SLOT0_IOMUX_PIN_NUM_CLK, SDMMC_SLOT0_IOMUX_PIN_NUM_CMD,
            SDMMC_SLOT0_IOMUX_PIN_NUM_D0, SDMMC_SLOT0_IOMUX_PIN_NUM_D1,
            SDMMC_SLOT0_IOMUX_PIN_NUM_D2, SDMMC_SLOT0_IOMUX_PIN_NUM_D3))
        dedicated = 0;
#endif
#if defined(SDMMC_SLOT1_IOMUX_PIN_NUM_CLK)
    if (dedicated < 0 &&
        iomux_pins(config, four_bit, SDMMC_SLOT1_IOMUX_PIN_NUM_CLK, SDMMC_SLOT1_IOMUX_PIN_NUM_CMD,
            SDMMC_SLOT1_IOMUX_PIN_NUM_D0, SDMMC_SLOT1_IOMUX_PIN_NUM_D1,
            SDMMC_SLOT1_IOMUX_PIN_NUM_D2, SDMMC_SLOT1_IOMUX_PIN_NUM_D3))
        dedicated = 1;
#endif
    if (dedicated >= 0)
        return dedicated == kHostedSlot ? SlotChoice{-1, true} : SlotChoice{dedicated, false};
    SlotChoice choice{};
    for (const int slot : {1, 0}) {
        if (!matrix_slot(slot))
            continue;
        if (slot != kHostedSlot)
            return SlotChoice{slot, false};
        choice.hosted = true;
    }
    return choice;
}

[[nodiscard]] SdCardResult open_sdmmc(const SdCardConfig& config) {
    if (config.clock < 0 || config.command < 0 || config.d0 < 0)
        return SdCardResult::invalid_pins;
    const bool four_bit = config.d1 >= 0 && config.d2 >= 0 && config.d3 >= 0;
    const auto choice = sdmmc_slot(config, four_bit);
    if (choice.hosted) {
        AddLog(LogLevel::error, "UFS: SDIO pins need SDMMC slot %d, which ESP-Hosted uses",
            kHostedSlot);
        return SdCardResult::invalid_pins;
    }
    const int slot = choice.slot;
    if (slot < 0) {
        AddLog(LogLevel::error, "UFS: SDIO pins are not this chip's SDMMC pins");
        return SdCardResult::invalid_pins;
    }
#if CONFIG_IDF_TARGET_ESP32
    if (four_bit)
        AddLog(
            LogLevel::info, "UFS: SDIO D2 is strapping GPIO12; a card pull-up selects 1.8 V flash");
#endif
    card.host = SDMMC_HOST_DEFAULT();
    card.host.slot = slot;
    card.host.max_freq_khz = kClockKhz;
#if SOC_GP_LDO_SUPPORTED && SOC_SDMMC_IO_POWER_EXTERNAL
    if (slot == 0) {
        sd_pwr_ctrl_ldo_config_t ldo{};
        ldo.ldo_chan_id = kSlot0PowerLdo;
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &card.power) != ESP_OK) {
            card.power = nullptr;
            return SdCardResult::failed;
        }
        card.host.pwr_ctrl_handle = card.power;
    }
#endif
    // Idempotent, and sdmmc_host_init_slot() accepts any slot: ESP-Hosted's slot stays off
    // limits through sdmmc_slot() above.
    if (sdmmc_host_init() != ESP_OK)
        return SdCardResult::failed;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = four_bit ? 4 : 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = static_cast<gpio_num_t>(config.clock);
    slot_config.cmd = static_cast<gpio_num_t>(config.command);
    slot_config.d0 = static_cast<gpio_num_t>(config.d0);
    if (four_bit) {
        slot_config.d1 = static_cast<gpio_num_t>(config.d1);
        slot_config.d2 = static_cast<gpio_num_t>(config.d2);
        slot_config.d3 = static_cast<gpio_num_t>(config.d3);
    }
#endif
    const auto initialized = sdmmc_host_init_slot(slot, &slot_config);
    if (initialized != ESP_OK)
        return initialized == ESP_ERR_INVALID_ARG ? SdCardResult::invalid_pins
                                                  : SdCardResult::failed;
    card.slot = slot;
    return card_result(sdmmc_card_init(&card.host, &card.card));
}
#endif

} // namespace

SdCardResult open_sd_card(const SdCardConfig& config, SectorDevice& device) {
    device = {};
    if (card.open)
        return SdCardResult::failed;
    card = {};
    card.mode = config.mode;
    SdCardResult result = SdCardResult::unsupported;
    if (config.mode == SdCardConfig::Mode::spi) {
        result = open_spi(config);
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else {
        result = open_sdmmc(config);
    }
#endif
    if (result == SdCardResult::opened &&
        (card.card.csd.capacity <= 0 || card.card.csd.sector_size != kSdSectorSize))
        result = SdCardResult::failed;
    if (result != SdCardResult::opened) {
        release(card);
        return result;
    }
    card.open = true;
    device = SectorDevice{&card, static_cast<std::uint32_t>(card.card.csd.capacity), read_sectors,
        write_sectors, sync_card, close_card};
    return SdCardResult::opened;
}

} // namespace newmota::platform
