/*
  sd_card_spi.cpp - SD card SPI-mode protocol over the shared SPI controller

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sd_card_spi.cpp
 * @brief SD Physical Layer SPI mode: SDSC, SDHC and SDXC cards, single-block transfers.
 *
 * The card shares the controller with other devices through spi_controller; its CS is a
 * GPIO this file drives. Initialization runs at 400 kHz in mode 0, data at up to 20 MHz.
 * CRCs stay off, as SPI mode starts. SDIO cards go to the family's native host, if any.
 */

#include "newmota/platform/sd_card.h"

#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/spi_controller.h"

#include <array>

namespace newmota::platform {
namespace {

constexpr std::uint32_t kIdentifyHz = 400000U;
constexpr std::uint32_t kDataHz = 20000000U;
constexpr std::uint32_t kInitializeMs = 1000U; // ACMD41 may take up to 1 s.
constexpr std::uint32_t kReadMs = 200U;
constexpr std::uint32_t kWriteMs = 500U;

constexpr std::uint8_t kGoIdle = 0;
constexpr std::uint8_t kSendIfCond = 8;
constexpr std::uint8_t kSendCsd = 9;
constexpr std::uint8_t kSetBlockLength = 16;
constexpr std::uint8_t kReadBlock = 17;
constexpr std::uint8_t kWriteBlock = 24;
constexpr std::uint8_t kAppCommand = 55;
constexpr std::uint8_t kReadOcr = 58;
constexpr std::uint8_t kAppSendOpCond = 41;

constexpr std::uint8_t kIdle = 0x01;
constexpr std::uint8_t kIllegalCommand = 0x04;
constexpr std::uint8_t kStartBlock = 0xfe;

struct Card final {
    spi_controller::Lease lease{};
    spi_controller::Format format{kIdentifyHz, 0U, 8U};
    PinId cs{};
    bool block_addressing{}; /**< SDHC/SDXC address sectors; SDSC addresses bytes. */
    bool open{};
};

Card card;

[[nodiscard]] bool transfer(const std::uint8_t* tx, std::uint8_t* rx, std::size_t size) {
    return spi_controller::exchange(card.lease, card.format, tx, rx, size) ==
           spi_controller::Result::success;
}

[[nodiscard]] bool receive_byte(std::uint8_t& value) {
    return transfer(nullptr, &value, 1);
}

[[nodiscard]] bool select(const bool selected) {
    return gpio_write(card.cs, !selected) == GpioResult::success;
}

/** @brief Releases CS and clocks one more byte so the card frees MISO. */
void deselect() {
    (void)select(false);
    (void)transfer(nullptr, nullptr, 1);
}

[[nodiscard]] bool expired(const std::uint32_t start, const std::uint32_t limit) {
    return monotonic_milliseconds() - start >= limit;
}

/** @brief Waits while the card holds MISO low (busy after a write). */
[[nodiscard]] bool wait_ready(const std::uint32_t limit) {
    const auto start = monotonic_milliseconds();
    std::uint8_t value = 0;
    do {
        if (!receive_byte(value))
            return false;
        if (value == 0xff)
            return true;
    } while (!expired(start, limit));
    return false;
}

/**
 * @brief Selects the card and sends one command frame.
 * @return the R1 response, or 0xff when the card did not answer.
 */
[[nodiscard]] std::uint8_t command(const std::uint8_t index, const std::uint32_t argument) {
    if (!select(true))
        return 0xff;
    if (index != kGoIdle && !wait_ready(kWriteMs))
        return 0xff;
    // CRCs are off in SPI mode, but CMD0 and CMD8 are checked before that applies.
    const std::uint8_t crc = index == kGoIdle ? 0x95 : index == kSendIfCond ? 0x87 : 0x01;
    const std::array<std::uint8_t, 6> frame{static_cast<std::uint8_t>(0x40U | index),
        static_cast<std::uint8_t>(argument >> 24U), static_cast<std::uint8_t>(argument >> 16U),
        static_cast<std::uint8_t>(argument >> 8U), static_cast<std::uint8_t>(argument), crc};
    if (!transfer(frame.data(), nullptr, frame.size()))
        return 0xff;
    std::uint8_t response = 0xff;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (!receive_byte(response))
            return 0xff;
        if ((response & 0x80U) == 0U)
            break;
    }
    return response;
}

[[nodiscard]] std::uint8_t app_command(const std::uint8_t index, const std::uint32_t argument) {
    const auto response = command(kAppCommand, 0);
    deselect();
    return response > kIdle ? response : command(index, argument);
}

/** @brief Waits for the start token, then reads @p size data bytes and the CRC. */
[[nodiscard]] bool read_data(std::uint8_t* const data, const std::size_t size) {
    const auto start = monotonic_milliseconds();
    std::uint8_t token = 0xff;
    do {
        if (!receive_byte(token))
            return false;
    } while (token == 0xff && !expired(start, kReadMs));
    std::array<std::uint8_t, 2> crc{};
    return token == kStartBlock && transfer(nullptr, data, size) &&
           transfer(nullptr, crc.data(), crc.size());
}

/** @brief Derives the sector count from a CSD register (structure version 1 or 2). */
[[nodiscard]] std::uint32_t csd_sectors(const std::array<std::uint8_t, 16>& csd) {
    if ((csd[0] >> 6U) == 1U) { // CSD 2.0: C_SIZE in units of 512 KiB.
        const std::uint32_t size =
            (static_cast<std::uint32_t>(csd[7] & 0x3fU) << 16U) | (csd[8] << 8U) | csd[9];
        return (size + 1U) * 1024U;
    }
    const std::uint32_t block_length = csd[5] & 0x0fU;
    const std::uint32_t size =
        (static_cast<std::uint32_t>(csd[6] & 0x03U) << 10U) | (csd[7] << 2U) | (csd[8] >> 6U);
    const std::uint32_t multiplier = ((csd[9] & 0x03U) << 1U) | (csd[10] >> 7U);
    const std::uint64_t bytes = static_cast<std::uint64_t>(size + 1U)
                                << (multiplier + 2U + block_length);
    return static_cast<std::uint32_t>(bytes / kSdSectorSize);
}

/** @brief Runs the SPI-mode identification sequence; @p sectors receives the capacity. */
[[nodiscard]] SdCardResult identify(std::uint32_t& sectors) {
    // At least 74 clocks with CS high put the card into its native idle state.
    (void)select(false);
    if (!transfer(nullptr, nullptr, 10))
        return SdCardResult::failed;
    std::uint8_t response = 0xff;
    for (int attempt = 0; attempt < 10 && response != kIdle; ++attempt) {
        response = command(kGoIdle, 0);
        deselect();
    }
    if (response != kIdle)
        return SdCardResult::no_card;

    bool version_2 = false;
    response = command(kSendIfCond, 0x1aaU);
    if (response == kIdle) {
        std::array<std::uint8_t, 4> echo{};
        if (!transfer(nullptr, echo.data(), echo.size()))
            return SdCardResult::failed;
        deselect();
        if (echo[2] != 0x01U || echo[3] != 0xaaU)
            return SdCardResult::no_card; // 2.7–3.6 V not accepted.
        version_2 = true;
    } else {
        deselect();
        if ((response & kIllegalCommand) == 0U)
            return SdCardResult::no_card;
    }

    const auto start = monotonic_milliseconds();
    do {
        response = app_command(kAppSendOpCond, version_2 ? 0x40000000U : 0U);
        deselect();
    } while (response == kIdle && !expired(start, kInitializeMs));
    if (response != 0U)
        return SdCardResult::no_card; // MMC and cards that never leave idle.

    card.block_addressing = false;
    if (version_2) {
        std::array<std::uint8_t, 4> ocr{};
        response = command(kReadOcr, 0);
        const bool read = response == 0U && transfer(nullptr, ocr.data(), ocr.size());
        deselect();
        if (!read)
            return SdCardResult::failed;
        card.block_addressing = (ocr[0] & 0x40U) != 0U;
    }
    if (!card.block_addressing) {
        response = command(kSetBlockLength, kSdSectorSize);
        deselect();
        if (response != 0U)
            return SdCardResult::failed;
    }

    card.format.hz = kDataHz;
    std::array<std::uint8_t, 16> csd{};
    response = command(kSendCsd, 0);
    const bool read = response == 0U && read_data(csd.data(), csd.size());
    deselect();
    if (!read)
        return SdCardResult::failed;
    sectors = csd_sectors(csd);
    return sectors != 0U ? SdCardResult::opened : SdCardResult::failed;
}

[[nodiscard]] std::uint32_t address(const std::uint32_t sector) {
    return card.block_addressing ? sector : sector * kSdSectorSize;
}

bool read_sectors(void*, const std::uint32_t sector, void* const buffer, const std::size_t count) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    for (std::size_t index = 0; index < count; ++index) {
        const auto response = command(kReadBlock, address(sector + index));
        const bool read = response == 0U && read_data(bytes, kSdSectorSize);
        deselect();
        if (!read)
            return false;
        bytes += kSdSectorSize;
    }
    return true;
}

bool write_sectors(
    void*, const std::uint32_t sector, const void* const buffer, const std::size_t count) {
    const auto* bytes = static_cast<const std::uint8_t*>(buffer);
    for (std::size_t index = 0; index < count; ++index) {
        bool written = command(kWriteBlock, address(sector + index)) == 0U;
        if (written) {
            constexpr std::array<std::uint8_t, 2> kPrefix{0xff, kStartBlock};
            constexpr std::array<std::uint8_t, 2> kCrc{0xff, 0xff};
            std::uint8_t status = 0;
            written = transfer(kPrefix.data(), nullptr, kPrefix.size()) &&
                      transfer(bytes, nullptr, kSdSectorSize) &&
                      transfer(kCrc.data(), nullptr, kCrc.size()) && receive_byte(status) &&
                      (status & 0x1fU) == 0x05U && wait_ready(kWriteMs);
        }
        deselect();
        if (!written)
            return false;
        bytes += kSdSectorSize;
    }
    return true;
}

/** @brief Each write already waited for its busy state; this confirms the card is idle. */
bool sync_card(void*) {
    const bool ready = select(true) && wait_ready(kWriteMs); // Busy shows only while selected.
    deselect();
    return ready;
}

void release() {
    if (card.cs.valid())
        (void)gpio_configure_input(card.cs, GpioPull::up);
    if (card.lease.valid())
        (void)spi_controller::release(card.lease);
    card = {};
}

void close_card(void*) {
    release();
}

[[nodiscard]] SdCardResult open_spi(const SdCardConfig& config, std::uint32_t& sectors) {
    if (config.cs < 0 || config.clock < 0 || config.mosi < 0 || config.miso < 0)
        return SdCardResult::invalid_pins;
    const spi_controller::Wiring wiring{config.spi_bus, config.clock, config.mosi, config.miso};
    switch (spi_controller::acquire(wiring, card.lease)) {
    case spi_controller::Result::success:
        break;
    case spi_controller::Result::unsupported:
    case spi_controller::Result::busy:
    case spi_controller::Result::conflict:
        return SdCardResult::invalid_pins;
    case spi_controller::Result::timeout:
    case spi_controller::Result::failed:
        return SdCardResult::failed;
    }
    card.cs = PinId{static_cast<std::uint16_t>(config.cs)};
    if (gpio_configure_output(card.cs, true) != GpioResult::success)
        return SdCardResult::invalid_pins;
    return identify(sectors);
}

} // namespace

SdCardResult open_sd_card(const SdCardConfig& config, SectorDevice& device) {
    device = {};
    if (card.open)
        return SdCardResult::failed;
    if (config.mode != SdCardConfig::Mode::spi)
        return detail::open_sdio_card(config, device);
    std::uint32_t sectors = 0;
    const auto result = open_spi(config, sectors);
    if (result != SdCardResult::opened) {
        release();
        return result;
    }
    card.open = true;
    device = SectorDevice{&card, sectors, read_sectors, write_sectors, sync_card, close_card};
    return SdCardResult::opened;
}

} // namespace newmota::platform
