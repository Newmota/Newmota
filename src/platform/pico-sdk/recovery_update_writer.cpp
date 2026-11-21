/*
  recovery_update_writer.cpp - bounded Pico Safeboot application writer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "recovery_update_writer.h"

#include "boot_select.h"
#include "flash_access.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "newmota/platform/update_envelope.h"
#include "support/crc32.h"

#if PICO_RP2350
#include "boot/picobin.h"
#include "picobin_image.h"
#include "pico/bootrom.h"
#elif PICO_RP2040
#include "rp2040_image.h"
#else
#error "The Pico recovery writer supports RP2040 and RP2350 targets only"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#ifndef NEWMOTA_IMAGE_SAFEBOOT
#error "The Pico recovery writer can only be compiled into Safeboot"
#endif

#ifndef NEWMOTA_UPDATE_CHIP_ID
#error "NEWMOTA_UPDATE_CHIP_ID is required"
#endif

#ifndef NEWMOTA_UPDATE_FLASH_BYTES
#error "NEWMOTA_UPDATE_FLASH_BYTES is required"
#endif

#ifndef NEWMOTA_UPDATE_LAYOUT_SHA256
#error "NEWMOTA_UPDATE_LAYOUT_SHA256 is required"
#endif

#ifndef NEWMOTA_APPLICATION_OFFSET
#error "NEWMOTA_APPLICATION_OFFSET is required"
#endif

#ifndef NEWMOTA_APPLICATION_SIZE
#error "NEWMOTA_APPLICATION_SIZE is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_OFFSET
#error "NEWMOTA_SAFEBOOT_OFFSET is required"
#endif

#ifndef NEWMOTA_SAFEBOOT_SIZE
#error "NEWMOTA_SAFEBOOT_SIZE is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_OFFSET
#error "NEWMOTA_FILESYSTEM_OFFSET is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_SIZE
#error "NEWMOTA_FILESYSTEM_SIZE is required"
#endif

#ifndef NEWMOTA_BOOT_MARKER_OFFSET
#error "NEWMOTA_BOOT_MARKER_OFFSET is required"
#endif

#ifndef NEWMOTA_DATA_OFFSET
#error "NEWMOTA_DATA_OFFSET is required"
#endif

#ifndef NEWMOTA_DATA_SIZE
#error "NEWMOTA_DATA_SIZE is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::size_t kChunkSize = 4096U;
constexpr std::size_t kMinimumApplicationPrefix = 512U;
constexpr char kLayoutSha256Text[] = NEWMOTA_UPDATE_LAYOUT_SHA256;
#if PICO_RP2040
constexpr char kLayoutIdentityPrefix[] = "NEWMOTA-RP2040-LAYOUT-V1:";
#endif
#if PICO_RP2350
constexpr std::uint32_t kPartitionFlags =
    PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION;
constexpr std::uint32_t kExpectedPermissions =
    PICOBIN_PARTITION_PERMISSION_S_R_BITS | PICOBIN_PARTITION_PERMISSION_S_W_BITS |
    PICOBIN_PARTITION_PERMISSION_NSBOOT_R_BITS | PICOBIN_PARTITION_PERMISSION_NSBOOT_W_BITS;
#endif

static_assert(NEWMOTA_SAFEBOOT_OFFSET == 0U, "Pico Safeboot must start at flash offset zero");
static_assert(NEWMOTA_DATA_OFFSET == NEWMOTA_BOOT_MARKER_OFFSET,
    "Pico data partition must start with the recovery marker sector");

std::atomic<bool> g_writer_lease{false};

#if PICO_RP2350
[[nodiscard]] bool partition_matches(const std::uint32_t index, const std::uint32_t expected_offset,
    const std::uint32_t expected_size) {
    std::array<std::uint32_t, 4> info{};
    const auto flags = kPartitionFlags | (index << 24U);
    const int words =
        rom_get_partition_table_info(info.data(), static_cast<std::uint32_t>(info.size()), flags);
    if (words != 3 || info[0] != kPartitionFlags)
        return false;
    const auto location = info[1];
    const auto permissions = info[2];
    const std::uint32_t first_sector = (location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                                       PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    const std::uint32_t last_sector = (location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                                      PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    const std::uint32_t start = first_sector * FLASH_SECTOR_SIZE;
    const std::uint32_t end = (last_sector + 1U) * FLASH_SECTOR_SIZE;
    return start == expected_offset && end >= start && end - start == expected_size &&
           (permissions & PICOBIN_PARTITION_PERMISSIONS_BITS) == kExpectedPermissions;
}
#endif

[[nodiscard]] bool runtime_layout_matches() {
#if PICO_RP2350
    return partition_matches(0U, NEWMOTA_SAFEBOOT_OFFSET, NEWMOTA_SAFEBOOT_SIZE) &&
           partition_matches(1U, NEWMOTA_APPLICATION_OFFSET, NEWMOTA_APPLICATION_SIZE) &&
           partition_matches(2U, NEWMOTA_DATA_OFFSET, NEWMOTA_DATA_SIZE);
#elif PICO_RP2040
    std::uint8_t digest[update_detail::kEnvelopeDigestSize]{};
    const char* const identity = pico_detail::compiled_layout_identity();
    return identity != nullptr &&
           std::strncmp(identity, kLayoutIdentityPrefix, sizeof(kLayoutIdentityPrefix) - 1U) == 0 &&
           update_detail::decode_layout_digest(
               identity + sizeof(kLayoutIdentityPrefix) - 1U, digest) &&
           NEWMOTA_SAFEBOOT_OFFSET == 0U && NEWMOTA_APPLICATION_OFFSET == NEWMOTA_SAFEBOOT_SIZE &&
           NEWMOTA_BOOT_MARKER_OFFSET == NEWMOTA_APPLICATION_OFFSET + NEWMOTA_APPLICATION_SIZE &&
           NEWMOTA_DATA_OFFSET == NEWMOTA_BOOT_MARKER_OFFSET &&
           NEWMOTA_DATA_SIZE >= FLASH_SECTOR_SIZE &&
           NEWMOTA_FILESYSTEM_OFFSET == NEWMOTA_BOOT_MARKER_OFFSET + FLASH_SECTOR_SIZE &&
           NEWMOTA_FILESYSTEM_OFFSET + NEWMOTA_FILESYSTEM_SIZE == NEWMOTA_UPDATE_FLASH_BYTES;
#else
    return false;
#endif
}

[[nodiscard]] bool read_application_image(
    void*, const std::uint32_t offset, std::uint8_t* const buffer, const std::size_t size) {
    if (size > UINT32_MAX || offset > NEWMOTA_APPLICATION_SIZE ||
        size > NEWMOTA_APPLICATION_SIZE - offset) {
        return false;
    }
    return pico_detail::flash_read(NEWMOTA_APPLICATION_OFFSET + offset, buffer, size);
}

class PicoRecoveryUpdateWriter final : public UpdateWriter {
  public:
    [[nodiscard]] bool available() const override {
        return !g_writer_lease.load() && runtime_layout_matches() &&
               pico_detail::recovery_mode_requested();
    }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        return static_cast<std::uint32_t>(update_detail::kEnvelopeSize) + NEWMOTA_APPLICATION_SIZE;
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return kChunkSize; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override {
        return update_detail::kEnvelopeSize + kMinimumApplicationPrefix;
    }

    [[nodiscard]] UpdateWriterResult begin(const std::uint32_t image_size) override {
        if (active_)
            return UpdateWriterResult::busy;
        bool expected = false;
        if (!g_writer_lease.compare_exchange_strong(expected, true)) {
            return UpdateWriterResult::busy;
        }
        if (image_size < minimum_initial_chunk_size() || image_size > maximum_image_size() ||
            !runtime_layout_matches() || !pico_detail::recovery_mode_requested()) {
            g_writer_lease.store(false);
            return UpdateWriterResult::rejected;
        }
        artifact_size_ = image_size;
        received_bytes_ = 0U;
        payload_size_ = 0U;
        payload_received_ = 0U;
        payload_programmed_ = 0U;
        payload_crc_state_ = support::kCrc32InitialState;
        page_size_ = 0U;
        initialized_ = false;
        verified_ = false;
        active_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult write(
        const std::uint32_t offset, const std::string_view bytes) override {
        if (!active_ || verified_ || offset != received_bytes_ || bytes.empty() ||
            bytes.size() > maximum_chunk_size() ||
            bytes.size() > artifact_size_ - received_bytes_) {
            return UpdateWriterResult::rejected;
        }
        if (received_bytes_ == 0U) {
            if (bytes.size() < minimum_initial_chunk_size() || !initialize_from_prefix(bytes)) {
                return UpdateWriterResult::rejected;
            }
        }
        const std::size_t envelope_size = update_detail::kEnvelopeSize;
        const std::size_t chunk_end = static_cast<std::size_t>(received_bytes_) + bytes.size();
        if (chunk_end > envelope_size) {
            const std::size_t local_start =
                received_bytes_ < envelope_size ? envelope_size - received_bytes_ : 0U;
            const auto payload = bytes.substr(local_start);
            payload_crc_state_ =
                support::crc32_update(payload_crc_state_, payload.data(), payload.size());
            if (!buffer_and_program(payload))
                return UpdateWriterResult::failed;
            payload_received_ += static_cast<std::uint32_t>(payload.size());
        }
        received_bytes_ += static_cast<std::uint32_t>(bytes.size());
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult finish() override {
        if (!active_ || !initialized_ || received_bytes_ != artifact_size_ ||
            payload_received_ != payload_size_ ||
            support::crc32_finalize(payload_crc_state_) != envelope_.payload_crc32 ||
            !flush_partial_page() || !verify_flash_payload()) {
            return UpdateWriterResult::failed;
        }
        verified_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult activate() override {
        if (!active_ || !verified_ || !runtime_layout_matches() ||
            !pico_detail::recovery_mode_requested()) {
            return UpdateWriterResult::rejected;
        }
        if (!pico_detail::clear_recovery_mode())
            return UpdateWriterResult::failed;
        release_lease();
        return UpdateWriterResult::success;
    }

    void abort() override { release_lease(); }

  private:
    [[nodiscard]] bool initialize_from_prefix(const std::string_view bytes) {
        const auto* const prefix = reinterpret_cast<const std::uint8_t*>(bytes.data());
        std::uint8_t digest[update_detail::kEnvelopeDigestSize]{};
        if (!update_detail::decode_envelope(bytes, artifact_size_, envelope_) ||
            !update_detail::decode_layout_digest(kLayoutSha256Text, digest) ||
            !update_detail::envelope_matches(envelope_, NEWMOTA_UPDATE_CHIP_ID,
                NEWMOTA_UPDATE_FLASH_BYTES, digest, NEWMOTA_APPLICATION_OFFSET,
                NEWMOTA_APPLICATION_SIZE, NEWMOTA_SAFEBOOT_OFFSET, NEWMOTA_SAFEBOOT_SIZE,
                NEWMOTA_FILESYSTEM_OFFSET, NEWMOTA_FILESYSTEM_SIZE) ||
            envelope_.payload_size == 0U || envelope_.payload_size > NEWMOTA_APPLICATION_SIZE ||
            bytes.size() < minimum_initial_chunk_size() || !runtime_layout_matches() ||
            !pico_detail::recovery_mode_requested()) {
            return false;
        }
        const std::uint32_t erase_size = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(envelope_.payload_size) + FLASH_SECTOR_SIZE - 1U) /
            FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE);
        if (erase_size == 0U || erase_size > NEWMOTA_APPLICATION_SIZE ||
            !pico_detail::flash_erase(NEWMOTA_APPLICATION_OFFSET, erase_size)) {
            return false;
        }
        std::fill(page_.begin(), page_.end(), 0xFFU);
        payload_size_ = envelope_.payload_size;
        initialized_ = true;
        return true;
    }

    [[nodiscard]] bool buffer_and_program(const std::string_view payload) {
        const auto* const bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
        for (std::size_t index = 0; index < payload.size(); ++index) {
            page_[page_size_++] = bytes[index];
            if (page_size_ == page_.size()) {
                if (!program_page())
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool program_page() {
        if (payload_programmed_ + page_.size() > NEWMOTA_APPLICATION_SIZE) {
            return false;
        }
        if (!pico_detail::flash_program(
                NEWMOTA_APPLICATION_OFFSET + payload_programmed_, page_.data(), page_.size())) {
            return false;
        }
        payload_programmed_ += static_cast<std::uint32_t>(page_.size());
        page_size_ = 0U;
        std::fill(page_.begin(), page_.end(), 0xFFU);
        return true;
    }

    [[nodiscard]] bool flush_partial_page() {
        if (page_size_ == 0U)
            return true;
        return program_page();
    }

    [[nodiscard]] bool verify_flash_payload() const {
        alignas(4) std::array<std::uint8_t, kChunkSize> buffer{};
        std::uint32_t state = support::kCrc32InitialState;
        std::uint32_t offset = 0U;
        while (offset < payload_size_) {
            const auto count = static_cast<std::size_t>(
                payload_size_ - offset < buffer.size() ? payload_size_ - offset : buffer.size());
            if (!pico_detail::flash_read(
                    NEWMOTA_APPLICATION_OFFSET + offset, buffer.data(), count)) {
                return false;
            }
            state = support::crc32_update(state, buffer.data(), count);
            offset += static_cast<std::uint32_t>(count);
        }
        if (support::crc32_finalize(state) != envelope_.payload_crc32)
            return false;
#if PICO_RP2350
        return pico_detail::valid_rp2350_application(payload_size_,
            static_cast<std::uint32_t>(XIP_BASE), SRAM_BASE, SRAM_END, read_application_image,
            nullptr);
#elif PICO_RP2040
        return pico_detail::valid_rp2040_application(payload_size_,
            static_cast<std::uint32_t>(XIP_BASE) + NEWMOTA_APPLICATION_OFFSET, SRAM_BASE, SRAM_END,
            read_application_image, nullptr);
#else
        return false;
#endif
    }

    void release_lease() {
        if (!active_)
            return;
        active_ = false;
        initialized_ = false;
        verified_ = false;
        artifact_size_ = 0U;
        received_bytes_ = 0U;
        payload_size_ = 0U;
        payload_received_ = 0U;
        payload_programmed_ = 0U;
        page_size_ = 0U;
        g_writer_lease.store(false);
    }

    bool active_{};
    bool initialized_{};
    bool verified_{};
    std::uint32_t artifact_size_{};
    std::uint32_t received_bytes_{};
    std::uint32_t payload_size_{};
    std::uint32_t payload_received_{};
    std::uint32_t payload_programmed_{};
    std::uint32_t payload_crc_state_{support::kCrc32InitialState};
    std::size_t page_size_{};
    alignas(4) std::array<std::uint8_t, FLASH_PAGE_SIZE> page_{};
    update_detail::UpdateEnvelopeV1 envelope_{};
};

} // namespace

UpdateWriter& recovery_update_writer() {
    static PicoRecoveryUpdateWriter writer;
    return writer;
}

/** @brief Returns the application-slot writer used by Safeboot browser updates. */
UpdateWriter& browser_update_writer() {
    return recovery_update_writer();
}

} // namespace newmota::platform
