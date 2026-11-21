/*
  sd_card_sdh.cpp - BL616/BL618DG SD host (SDH) cards

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sd_card_sdh.cpp
 * @brief SDIO-mode cards on the SDH controller through BouffaloSDK's `sdh_sd` driver.
 *
 * SDH owns six fixed pads: GPIO10–15 on BL616, GPIO43–48 on BL618DG. From the first pad they
 * carry D2, D3, CMD, CLK, D0, D1. The SDK marks the clock pad; the other positions follow the
 * BL616 pinout and need hardware confirmation, as does the whole BL618DG group. The SDK
 * driver switches to four data lines whenever the card supports them, so only four-bit
 * wiring works. It runs in polling mode and moves data by ADMA. The SD host specification
 * wants word-aligned ADMA addresses (the SDK skips that check), so unaligned sectors go through
 * an aligned bounce buffer, and long transfers are split to fit the SDK's descriptors.
 */
#include "newmota/platform/sd_card.h"

#include "newmota/log.h"
#include "newmota/platform/gpio.h"
extern "C" {
#include "bflb_gpio.h"
#include "sdh_sd.h"
}

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace newmota::platform::detail {
namespace {

#if defined(BL616)
constexpr int kFirstPad = 10;
#elif defined(BL618DG)
constexpr int kFirstPad = 43;
#else
#error "SDH cards exist on BL616 and BL618DG only"
#endif

/** @brief Upper card clock, as on ESP-IDF; the SDK would raise high-speed cards to 50 MHz. */
constexpr std::uint32_t kClockHz = 20000000U;

/**
 * @brief Sectors one SDK transfer may carry: its 16 ADMA descriptors of 64 KiB each.
 *
 * The SDK writes a descriptor before checking the count, so a longer transfer would write
 * past its descriptor array instead of failing.
 */
constexpr std::size_t kTransferSectors =
    SDH_HOST_ADMA2_HW_DESC_NUM * SDH_ADMA2_DESC_LEN_MAX / kSdSectorSize;
static_assert(kTransferSectors > 0U);

struct Card final {
    sd_card_s card{};
    sdh_host_s host{};
    std::uint8_t* bounce{}; // Word-aligned sector for unaligned FatFs buffers.
    bool pads{};
    bool open{};
};

Card card;

[[nodiscard]] bool aligned(const void* const buffer) {
    return (reinterpret_cast<std::uintptr_t>(buffer) & 3U) == 0U;
}

[[nodiscard]] bool pads_match(const SdCardConfig& config) {
    return config.d2 == kFirstPad && config.d3 == kFirstPad + 1 &&
           config.command == kFirstPad + 2 && config.clock == kFirstPad + 3 &&
           config.d0 == kFirstPad + 4 && config.d1 == kFirstPad + 5;
}

void configure_pads(const bool sdh) {
    auto* const gpio = bflb_device_get_by_name("gpio");
    for (int pad = kFirstPad; pad < kFirstPad + 6; ++pad) {
        if (sdh)
            bflb_gpio_init(
                gpio, pad, GPIO_FUNC_SDH | GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1);
        else
            (void)gpio_configure_input(PinId{static_cast<std::uint16_t>(pad)}, GpioPull::none);
    }
}

bool read_sectors(void*, const std::uint32_t sector, void* const buffer, const std::size_t count) {
    if (aligned(buffer)) {
        auto* bytes = static_cast<std::uint8_t*>(buffer);
        for (std::size_t done = 0; done < count;) {
            const auto chunk = count - done < kTransferSectors ? count - done : kTransferSectors;
            if (sdh_sd_read_blocks(&card.card, bytes + done * kSdSectorSize,
                    sector + static_cast<std::uint32_t>(done),
                    static_cast<std::uint32_t>(chunk)) < 0)
                return false;
            done += chunk;
        }
        return true;
    }
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    for (std::size_t index = 0; index < count; ++index, bytes += kSdSectorSize) {
        if (sdh_sd_read_blocks(&card.card, card.bounce, sector + index, 1) < 0)
            return false;
        std::memcpy(bytes, card.bounce, kSdSectorSize);
    }
    return true;
}

bool write_sectors(
    void*, const std::uint32_t sector, const void* const buffer, const std::size_t count) {
    if (aligned(buffer)) {
        auto* bytes = static_cast<std::uint8_t*>(const_cast<void*>(buffer));
        for (std::size_t done = 0; done < count;) {
            const auto chunk = count - done < kTransferSectors ? count - done : kTransferSectors;
            if (sdh_sd_write_blocks(&card.card, bytes + done * kSdSectorSize,
                    sector + static_cast<std::uint32_t>(done),
                    static_cast<std::uint32_t>(chunk)) < 0)
                return false;
            done += chunk;
        }
        return true;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    for (std::size_t index = 0; index < count; ++index, bytes += kSdSectorSize) {
        std::memcpy(card.bounce, bytes, kSdSectorSize);
        if (sdh_sd_write_blocks(&card.card, card.bounce, sector + index, 1) < 0)
            return false;
    }
    return true;
}

/** @brief The SDK write returns after the card reports its programming finished. */
bool sync_card(void*) {
    return true;
}

void release() {
    if (card.open)
        (void)sdh_sd_deinit(&card.card);
    if (card.pads)
        configure_pads(false);
    std::free(card.bounce);
    card = {};
}

void close_card(void*) {
    release();
}

} // namespace

SdCardResult open_sdio_card(const SdCardConfig& config, SectorDevice& device) {
    device = {};
    if (card.open)
        return SdCardResult::failed;
    if (config.d1 < 0 || config.d2 < 0 || config.d3 < 0) {
        AddLog(LogLevel::error, "UFS: SDH needs SDIO D1-D3; one-bit wiring is unsupported");
        return SdCardResult::invalid_pins;
    }
    if (!pads_match(config)) {
        AddLog(LogLevel::error, "UFS: SDH pins are GPIO%d D2, %d D3, %d CMD, %d CLK, %d D0, %d D1",
            kFirstPad, kFirstPad + 1, kFirstPad + 2, kFirstPad + 3, kFirstPad + 4, kFirstPad + 5);
        return SdCardResult::invalid_pins;
    }
    card.bounce = static_cast<std::uint8_t*>(std::malloc(kSdSectorSize)); // malloc aligns to 8.
    if (card.bounce == nullptr)
        return SdCardResult::failed;
    configure_pads(true);
    card.pads = true;
    if (sdh_sd_card_init(&card.card, &card.host) < 0) {
        (void)sdh_sd_deinit(&card.card);
        release();
        return SdCardResult::no_card;
    }
    card.open = true;
    if (card.card.bus_clock_hz > kClockHz)
        (void)sdh_set_bus_clock(&card.host, kClockHz, &card.card.bus_clock_hz);
    if (card.card.block_size != kSdSectorSize || card.card.block_count == 0U) {
        release();
        return SdCardResult::failed;
    }
    device = SectorDevice{
        &card, card.card.block_count, read_sectors, write_sectors, sync_card, close_card};
    return SdCardResult::opened;
}

} // namespace newmota::platform::detail
