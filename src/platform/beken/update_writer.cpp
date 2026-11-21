/*
  update_writer.cpp - bounded Beken native RBL OTA staging writer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "update_writer.h"

#include "flash_access.h"
#include "newmota/platform/beken/update_config.h"
#include "support/crc32.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "BkDriverFlash.h"
#include "rtos_pub.h"
}

namespace newmota::platform::beken {
namespace {

constexpr std::size_t kRblHeaderSize = 96U;
constexpr std::size_t kRblHeaderBodySize = 92U;
constexpr std::size_t kPayloadVerifyChunk = 1024U;
constexpr std::size_t kCompatibilityTagSize = 24U;
constexpr std::size_t kNameOffset = 12U;
constexpr std::size_t kNameFieldEnd = kNameOffset + 16U;
constexpr std::size_t kSerialOffset = 52U;
constexpr std::size_t kPayloadCrcOffset = 76U;
constexpr std::size_t kRawSizeOffset = 84U;
constexpr std::size_t kPayloadSizeOffset = 88U;

static_assert(sizeof(update_config::kRblCompatibilityTag) - 1U == kCompatibilityTagSize);
static_assert(update_config::kOta.offset % update_config::kEraseSize == 0U);
static_assert(update_config::kOta.size % update_config::kEraseSize == 0U);
// LittleFS lies outside staging, so an update never erases files.
static_assert(
    update_config::kFilesystem.offset >= update_config::kOta.offset + update_config::kOta.size ||
    update_config::kFilesystem.offset + update_config::kFilesystem.size <=
        update_config::kOta.offset);

bool g_writer_lease = false;

/** @brief Reads the shared OTA lease without a Beken task switch. */
[[nodiscard]] bool writer_lease_active() {
    rtos_lock_scheduling();
    const bool active = g_writer_lease;
    rtos_unlock_scheduling();
    return active;
}

/** @brief Atomically claims the sole Beken OTA staging target. */
[[nodiscard]] bool acquire_writer_lease() {
    rtos_lock_scheduling();
    const bool acquired = !g_writer_lease;
    if (acquired)
        g_writer_lease = true;
    rtos_unlock_scheduling();
    return acquired;
}

/** @brief Releases the Beken OTA lease after a failed or completed session. */
void release_writer_lease() {
    rtos_lock_scheduling();
    g_writer_lease = false;
    rtos_unlock_scheduling();
}

struct RblHeader final {
    std::uint32_t payload_crc{};
    std::uint32_t raw_size{};
    std::uint32_t payload_size{};
};

struct NativePartition final {
    bk_partition_t id;
    update_config::Region expected;
};

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool ranges_overlap(
    const update_config::Region first, const update_config::Region second) {
    const auto first_end = static_cast<std::uint64_t>(first.offset) + first.size;
    const auto second_end = static_cast<std::uint64_t>(second.offset) + second.size;
    return first.offset < second_end && second.offset < first_end;
}

[[nodiscard]] bool native_layout_matches() {
    constexpr std::array<NativePartition, 6> partitions{{
        {BK_PARTITION_BOOTLOADER, update_config::kBootloader},
        {BK_PARTITION_APPLICATION, update_config::kApplication},
        {BK_PARTITION_OTA, update_config::kOta},
        {BK_PARTITION_RF_FIRMWARE, update_config::kRf},
        {BK_PARTITION_NET_PARAM, update_config::kNet},
        {BK_PARTITION_BLE_BONDING_FLASH, update_config::kBleBonding},
    }};

    for (std::size_t index = 0U; index < partitions.size(); ++index) {
        const auto& partition = partitions[index];
        const auto* const actual = bk_flash_get_info(partition.id);
        if (actual == nullptr || actual->partition_owner != BK_FLASH_EMBEDDED ||
            actual->partition_start_addr != partition.expected.offset ||
            actual->partition_length != partition.expected.size ||
            static_cast<std::uint64_t>(partition.expected.offset) + partition.expected.size >
                update_config::kFlashBytes) {
            return false;
        }
        for (std::size_t other = index + 1U; other < partitions.size(); ++other) {
            if (ranges_overlap(partition.expected, partitions[other].expected)) {
                return false;
            }
        }
    }
    return !ranges_overlap(update_config::kOta, update_config::kBootloader) &&
           !ranges_overlap(update_config::kOta, update_config::kApplication) &&
           !ranges_overlap(update_config::kOta, update_config::kRf) &&
           !ranges_overlap(update_config::kOta, update_config::kNet) &&
           !ranges_overlap(update_config::kOta, update_config::kBleBonding) &&
           !ranges_overlap(update_config::kOta, update_config::kFilesystem);
}

[[nodiscard]] bool parse_rbl_header(
    const std::uint8_t* const bytes, const std::uint32_t artifact_size, RblHeader& parsed) {
    if (bytes == nullptr || artifact_size < kRblHeaderSize ||
        std::memcmp(bytes, "RBL\0", 4U) != 0) {
        return false;
    }
    const auto header_crc = support::crc32_finalize(
        support::crc32_update(support::kCrc32InitialState, bytes, kRblHeaderBodySize));
    if (read_u32(bytes + kRblHeaderBodySize) != header_crc)
        return false;
    if (bytes[6U] != 0U || bytes[7U] != 0U)
        return false;

    if (bytes[kNameOffset] != 'a' || bytes[kNameOffset + 1U] != 'p' ||
        bytes[kNameOffset + 2U] != 'p') {
        return false;
    }
    for (std::size_t index = kNameOffset + 3U; index < kNameFieldEnd; ++index) {
        if (bytes[index] != 0U)
            return false;
    }
    if (std::memcmp(bytes + kSerialOffset, update_config::kRblCompatibilityTag,
            kCompatibilityTagSize) != 0 ||
        bytes[4U] != update_config::kRblEncryption || bytes[5U] != update_config::kRblCompression) {
        return false;
    }

    parsed.payload_crc = read_u32(bytes + kPayloadCrcOffset);
    parsed.raw_size = read_u32(bytes + kRawSizeOffset);
    parsed.payload_size = read_u32(bytes + kPayloadSizeOffset);
    const auto expected_size = static_cast<std::uint64_t>(kRblHeaderSize) + parsed.payload_size;
    return parsed.payload_size > 0U && parsed.raw_size > 0U &&
           parsed.raw_size <= update_config::kApplicationRawLimit &&
           expected_size == artifact_size &&
           (update_config::kRblEncryption != 2U || parsed.payload_size % 16U == 0U);
}

class BekenNativeRblUpdateWriter final : public UpdateWriter {
  public:
    [[nodiscard]] bool available() const override {
        return !writer_lease_active() && ota_staging_available() && native_layout_matches();
    }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        return update_config::kOta.size;
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return kPayloadVerifyChunk; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override { return kRblHeaderSize; }

    [[nodiscard]] UpdateWriterResult begin(const std::uint32_t image_size) override {
        if (active_)
            return UpdateWriterResult::busy;
        if (!acquire_writer_lease()) {
            return UpdateWriterResult::busy;
        }
        if (image_size < kRblHeaderSize || image_size > maximum_image_size()) {
            release_writer_lease();
            return UpdateWriterResult::rejected;
        }
        if (!native_layout_matches() || !ota_staging_available()) {
            release_writer_lease();
            return UpdateWriterResult::unavailable;
        }
        image_size_ = image_size;
        received_bytes_ = 0U;
        payload_received_ = 0U;
        header_ = {};
        active_ = true;
        staging_started_ = false;
        activation_started_ = false;
        verified_ = false;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult write(
        const std::uint32_t offset, const std::string_view bytes) override {
        if (!active_ || verified_ || bytes.empty() || bytes.size() > maximum_chunk_size() ||
            offset != received_bytes_ || bytes.size() > image_size_ - received_bytes_) {
            return UpdateWriterResult::rejected;
        }

        const auto* const input = reinterpret_cast<const std::uint8_t*>(bytes.data());
        std::size_t payload_offset = 0U;
        if (received_bytes_ == 0U) {
            if (bytes.size() < kRblHeaderSize || !parse_rbl_header(input, image_size_, header_) ||
                !native_layout_matches()) {
                return UpdateWriterResult::rejected;
            }
            std::memcpy(header_bytes_.data(), input, header_bytes_.size());
            if (!begin_ota_staging())
                return UpdateWriterResult::busy;
            staging_started_ = true;
            if (!erase_staging_image())
                return UpdateWriterResult::failed;
            payload_offset = kRblHeaderSize;
        }

        const auto payload_size = bytes.size() - payload_offset;
        if (payload_size > 0U) {
            const auto result = program_payload(input + payload_offset,
                static_cast<std::uint32_t>(received_bytes_ + payload_offset - kRblHeaderSize),
                payload_size);
            if (result != UpdateWriterResult::success)
                return result;
            payload_received_ += static_cast<std::uint32_t>(payload_size);
        }
        received_bytes_ += static_cast<std::uint32_t>(bytes.size());
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult finish() override {
        if (!active_ || staging_started_ == false || received_bytes_ != image_size_ ||
            payload_received_ != header_.payload_size) {
            return UpdateWriterResult::rejected;
        }
        std::array<std::uint8_t, kRblHeaderSize> uncommitted_header{};
        if (!read_ota_staging(
                update_config::kOta.offset, uncommitted_header.data(), uncommitted_header.size()) ||
            std::any_of(uncommitted_header.begin(), uncommitted_header.end(),
                [](const std::uint8_t value) { return value != 0xffU; }) ||
            !verify_staged_payload()) {
            return UpdateWriterResult::failed;
        }
        verified_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult activate() override {
        if (!active_ || !staging_started_ || !verified_) {
            return UpdateWriterResult::rejected;
        }
        const auto address = update_config::kOta.offset;
        activation_started_ = true;
        std::array<std::uint8_t, 4U> erased_magic{};
        if (!read_ota_staging(address, erased_magic.data(), erased_magic.size()) ||
            std::any_of(erased_magic.begin(), erased_magic.end(),
                [](const std::uint8_t value) { return value != 0xffU; })) {
            return UpdateWriterResult::failed;
        }
        if (!program_ota_staging(address + 4U, header_bytes_.data() + 4U, kRblHeaderSize - 4U)) {
            return UpdateWriterResult::failed;
        }
        std::array<std::uint8_t, kRblHeaderSize> readback{};
        if (!read_ota_staging(address + 4U, readback.data() + 4U, kRblHeaderSize - 4U) ||
            std::memcmp(readback.data() + 4U, header_bytes_.data() + 4U, kRblHeaderSize - 4U) !=
                0) {
            return UpdateWriterResult::failed;
        }
        if (!program_ota_staging(address, header_bytes_.data(), 4U)) {
            return UpdateWriterResult::failed;
        }
        if (!read_ota_staging(address, readback.data(), readback.size()) ||
            std::memcmp(readback.data(), header_bytes_.data(), header_bytes_.size()) != 0) {
            (void)erase_ota_staging_sector(address);
            return UpdateWriterResult::failed;
        }
        if (!release_ota_staging())
            return UpdateWriterResult::failed;
        active_ = false;
        staging_started_ = false;
        verified_ = false;
        release_writer_lease();
        return UpdateWriterResult::success;
    }

    void abort() override {
        if (staging_started_) {
            if (activation_started_) {
                (void)erase_ota_staging_sector(update_config::kOta.offset);
            }
            (void)release_ota_staging();
        }
        staging_started_ = false;
        activation_started_ = false;
        active_ = false;
        verified_ = false;
        image_size_ = 0U;
        received_bytes_ = 0U;
        payload_received_ = 0U;
        header_ = {};
        release_writer_lease();
    }

  private:
    [[nodiscard]] bool erase_staging_image() {
        const auto rounded_size =
            (static_cast<std::uint64_t>(image_size_) + update_config::kEraseSize - 1U) /
            update_config::kEraseSize * update_config::kEraseSize;
        if (rounded_size > update_config::kOta.size)
            return false;
        for (std::uint32_t offset = 0U; offset < rounded_size;
            offset += update_config::kEraseSize) {
            if (!erase_ota_staging_sector(update_config::kOta.offset + offset)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] UpdateWriterResult program_payload(
        const std::uint8_t* const bytes, const std::uint32_t offset, const std::size_t size) {
        if (bytes == nullptr || size == 0U ||
            static_cast<std::uint64_t>(offset) + size > header_.payload_size ||
            !program_ota_staging(
                update_config::kOta.offset + static_cast<std::uint32_t>(kRblHeaderSize) + offset,
                bytes, size)) {
            return UpdateWriterResult::failed;
        }
        return UpdateWriterResult::success;
    }

    [[nodiscard]] bool verify_staged_payload() const {
        std::array<std::uint8_t, kPayloadVerifyChunk> buffer{};
        std::uint32_t crc_state = support::kCrc32InitialState;
        std::uint32_t offset = 0U;
        while (offset < header_.payload_size) {
            const auto remaining = header_.payload_size - offset;
            const auto count =
                static_cast<std::size_t>(remaining < buffer.size() ? remaining : buffer.size());
            if (!read_ota_staging(update_config::kOta.offset +
                                      static_cast<std::uint32_t>(kRblHeaderSize) + offset,
                    buffer.data(), count)) {
                return false;
            }
            crc_state = support::crc32_update(crc_state, buffer.data(), count);
            offset += static_cast<std::uint32_t>(count);
        }
        return support::crc32_finalize(crc_state) == header_.payload_crc;
    }

    std::array<std::uint8_t, kRblHeaderSize> header_bytes_{};
    RblHeader header_{};
    std::uint32_t image_size_{};
    std::uint32_t received_bytes_{};
    std::uint32_t payload_received_{};
    bool active_{};
    bool staging_started_{};
    bool activation_started_{};
    bool verified_{};
};

} // namespace

UpdateWriter& native_rbl_update_writer() {
    static BekenNativeRblUpdateWriter writer;
    return writer;
}

} // namespace newmota::platform::beken

namespace newmota::platform {

/** @brief Returns the Beken native RBL writer used by the browser updater. */
UpdateWriter& browser_update_writer() {
    return beken::native_rbl_update_writer();
}

} // namespace newmota::platform
