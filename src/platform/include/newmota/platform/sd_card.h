/*
  sd_card.h - portable SD card sector-device interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform {

/** @brief Bytes in one SD card sector; FatFs runs with fixed 512-byte sectors. */
inline constexpr std::uint32_t kSdSectorSize = 512;

/** @brief Card transport resolved from the Tasmota-compatible SD GPIO functions. */
struct SdCardConfig final {
    enum class Mode : std::uint8_t {
        spi,  /**< `GPIO_SDCARD_CS` with the SPI bus pins of @ref spi_bus. */
        sdio, /**< `GPIO_SDIO_*`; four data lines when d1 to d3 are all assigned. */
    };

    Mode mode{Mode::spi};
    std::uint8_t spi_bus{}; /**< Zero-based SPI bus, the `GPIO_SDCARD_CS` instance. */
    int cs{-1}, clock{-1}, mosi{-1}, miso{-1};
    int command{-1}, d0{-1}, d1{-1}, d2{-1}, d3{-1};
};

enum class SdCardResult : std::uint8_t {
    opened,
    unsupported,  /**< This build or chip has no transport for @ref SdCardConfig::Mode. */
    invalid_pins, /**< The pins cannot be routed to the transport or are already owned. */
    no_card,      /**< The card did not answer initialization. */
    failed,
};

/**
 * @brief An opened card that reads and writes whole sectors.
 *
 * Buffers may be unaligned and outside DMA-capable memory; the platform bounces them when
 * its transport needs to. Calls belong to the application task that owns the volume.
 */
class SectorDevice final {
  public:
    using Read = bool (*)(void* context, std::uint32_t sector, void* buffer, std::size_t count);
    using Write = bool (*)(
        void* context, std::uint32_t sector, const void* buffer, std::size_t count);
    using Sync = bool (*)(void* context);
    using Close = void (*)(void* context);

    /** @brief Creates a closed device handle. */
    constexpr SectorDevice() = default;

    /** @brief Binds an opened card's capacity and operations. */
    constexpr SectorDevice(void* context, const std::uint32_t sector_count, Read read, Write write,
        Sync sync, Close close)
        : context_(context), sector_count_(sector_count), read_(read), write_(write), sync_(sync),
          close_(close) {}

    /** @brief Reports whether the handle refers to an opened card. */
    [[nodiscard]] constexpr bool valid() const {
        return context_ != nullptr && sector_count_ != 0 && read_ != nullptr && write_ != nullptr &&
               sync_ != nullptr && close_ != nullptr;
    }

    /** @brief Reads @p count sectors starting at @p sector. */
    [[nodiscard]] bool read(std::uint32_t sector, void* buffer, std::size_t count) const {
        return valid() && in_range(sector, count) && buffer != nullptr &&
               read_(context_, sector, buffer, count);
    }

    /** @brief Writes @p count sectors starting at @p sector. */
    [[nodiscard]] bool write(std::uint32_t sector, const void* buffer, std::size_t count) const {
        return valid() && in_range(sector, count) && buffer != nullptr &&
               write_(context_, sector, buffer, count);
    }

    /** @brief Waits until the card has committed every written sector. */
    [[nodiscard]] bool sync() const { return valid() && sync_(context_); }

    /** @brief Releases the card, its bus attachment and its pins, then clears the handle. */
    void close() {
        if (close_ != nullptr && context_ != nullptr)
            close_(context_);
        *this = {};
    }

    /** @brief Returns the card capacity in sectors. */
    [[nodiscard]] constexpr std::uint32_t sector_count() const { return sector_count_; }

  private:
    [[nodiscard]] constexpr bool in_range(
        const std::uint32_t sector, const std::size_t count) const {
        return count != 0 && sector < sector_count_ && count <= sector_count_ - sector;
    }

    void* context_{nullptr};
    std::uint32_t sector_count_{0};
    Read read_{nullptr};
    Write write_{nullptr};
    Sync sync_{nullptr};
    Close close_{nullptr};
};

/**
 * @brief Initializes the card on the configured transport.
 * @return opened when @p device was populated; @p device stays closed otherwise.
 */
[[nodiscard]] SdCardResult open_sd_card(const SdCardConfig& config, SectorDevice& device);

namespace detail {
/**
 * @brief Opens an SDIO card on a native SD host, for families whose SPI-mode protocol is
 * `sd_card_spi.cpp`; `sd_card_sdio_unavailable.cpp` reports unsupported elsewhere.
 */
[[nodiscard]] SdCardResult open_sdio_card(const SdCardConfig& config, SectorDevice& device);
} // namespace detail

} // namespace newmota::platform
