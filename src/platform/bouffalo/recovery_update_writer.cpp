/*
  recovery_update_writer.cpp - bounded BL602 Safeboot application writer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file recovery_update_writer.cpp
 * @brief Bounded target-aware Bouffalo Safeboot application updates.
 */

#include "recovery_update_writer.h"

extern "C" {
#include "bflb_core.h"
#include "bflb_flash.h"
#include "bflb_l1c.h"
#include "bflb_sec_mutex.h"
#include "bflb_sec_sha.h"
#include "bflb_boot2.h"
#include "partition.h"
}
#if !defined(BL602)
#include "mbedtls/sha256.h"
#endif
#include "newmota/platform/update_envelope.h"
#include "support/crc32.h"
#include "boot_header.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#ifndef NEWMOTA_IMAGE_SAFEBOOT
#error "The Bouffalo recovery writer can only be compiled into Safeboot"
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

namespace newmota::platform {
namespace {

constexpr std::size_t kChunkSize = 4096U;
constexpr std::size_t kBootHeaderSize = boot_header::size;
constexpr std::uint32_t kSectorSize = 4096U;
constexpr std::uint32_t kBootHeaderMagic = 0x504E4642U;
constexpr std::uint32_t kBootConfigOffset = boot_header::config_offset;
constexpr std::uint32_t kImageLengthOffset = boot_header::length_offset;
constexpr std::uint32_t kImageHashOffset = boot_header::hash_offset;
constexpr std::uint32_t kBootHeaderCrcOffset = boot_header::crc_offset;
constexpr std::uint32_t kBootConfigEncryptMask = 0x0000000CU;
constexpr std::uint32_t kBootConfigCrcIgnore = 1U << 16U;
constexpr std::uint32_t kBootConfigHashIgnore = 1U << 17U;
constexpr std::uint8_t kBoot2EntryType = 16U;
constexpr std::uint8_t kFilesystemEntryType = 9U;
constexpr char kBoot2EntryName[] = "Boot2";
constexpr char kFirmwareEntryName[] = "FW";
constexpr char kFilesystemEntryName[] = "lfs";
constexpr char kLayoutSha256[] = NEWMOTA_UPDATE_LAYOUT_SHA256;

static_assert(NEWMOTA_UPDATE_FLASH_BYTES == NEWMOTA_APPLICATION_OFFSET + NEWMOTA_APPLICATION_SIZE +
                                                NEWMOTA_SAFEBOOT_SIZE + NEWMOTA_FILESYSTEM_SIZE,
    "Bouffalo OTA writer only supports the resolved compact partition layout");

std::atomic<bool> g_writer_lease{false};
#if defined(BL602)
/* BL602's SHA link descriptor must live outside DTCM for DMA access. */
struct bflb_sha_link_s g_update_sha_link_cfg __attribute__((section(".wifi_ram"), aligned(4)));
#endif

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool current_layout_matches(
    pt_table_entry_config& entry, std::uint8_t& active_table, const bool require_safeboot) {
    if (bflb_flash_get_size() != NEWMOTA_UPDATE_FLASH_BYTES || bflb_boot2_init() != 0) {
        return false;
    }
    auto* const config = bflb_boot2_get_pt_config();
    pt_table_entry_config boot2{};
    pt_table_entry_config filesystem{};
    if (config == nullptr || config->pt_table.entryCnt != 3U ||
        pt_table_get_active_entries_by_name(config,
            reinterpret_cast<std::uint8_t*>(const_cast<char*>(kBoot2EntryName)),
            &boot2) != PT_ERROR_SUCCESS ||
        pt_table_get_active_entries_by_name(config,
            reinterpret_cast<std::uint8_t*>(const_cast<char*>(kFilesystemEntryName)),
            &filesystem) != PT_ERROR_SUCCESS ||
        pt_table_get_active_entries_by_name(config,
            reinterpret_cast<std::uint8_t*>(const_cast<char*>(kFirmwareEntryName)),
            &entry) != PT_ERROR_SUCCESS) {
        return false;
    }
    const auto table = bflb_boot2_get_active_partition();
    if (table > PT_TABLE_ID_1)
        return false;
    active_table = table;
    const auto selected = static_cast<std::uint8_t>(entry.active_index & 0x01U);
    return boot2.type == kBoot2EntryType && boot2.device == 0U && boot2.start_address[0] == 0U &&
           boot2.start_address[1] == 0U && boot2.max_len[0] == 0xE000U && boot2.max_len[1] == 0U &&
           entry.type == PT_ENTRY_FW_CPU0 && entry.start_address[0] == NEWMOTA_APPLICATION_OFFSET &&
           entry.start_address[1] == NEWMOTA_SAFEBOOT_OFFSET &&
           entry.max_len[0] == NEWMOTA_APPLICATION_SIZE &&
           entry.max_len[1] == NEWMOTA_SAFEBOOT_SIZE && filesystem.type == kFilesystemEntryType &&
           filesystem.device == 0U && filesystem.start_address[0] == NEWMOTA_FILESYSTEM_OFFSET &&
           filesystem.start_address[1] == 0U && filesystem.max_len[0] == NEWMOTA_FILESYSTEM_SIZE &&
           filesystem.max_len[1] == 0U && (entry.active_index & 0xFEU) == 0U &&
           (!require_safeboot || selected == 1U);
}

[[nodiscard]] bool validate_native_header(const std::uint8_t* const prefix,
    const std::uint32_t payload_size, std::uint8_t* const saved_header) {
    if (prefix == nullptr || saved_header == nullptr ||
        payload_size <= kBootHeaderSize + kSectorSize) {
        return false;
    }
    std::memcpy(saved_header, prefix, kBootHeaderSize);
    const auto magic = read_u32(saved_header);
    const auto boot_config = read_u32(saved_header + kBootConfigOffset);
    const auto image_length = read_u32(saved_header + kImageLengthOffset);
    const auto header_crc = read_u32(saved_header + kBootHeaderCrcOffset);
    const auto calculated_header_crc = support::crc32_finalize(
        support::crc32_update(support::kCrc32InitialState, saved_header, kBootHeaderCrcOffset));
    const auto sign_type = boot_config & 0x03U;
    const bool unsigned_sdk_format = sign_type == 0U || sign_type == 3U;
    return magic == kBootHeaderMagic && unsigned_sdk_format &&
           (boot_config & kBootConfigEncryptMask) == 0U &&
           (boot_config & (kBootConfigCrcIgnore | kBootConfigHashIgnore)) == 0U &&
           image_length != 0U &&
           static_cast<std::uint64_t>(kSectorSize) + image_length == payload_size &&
           calculated_header_crc == header_crc;
}

[[nodiscard]] bool verify_flash_payload_crc(
    const std::uint32_t address, const std::uint32_t size, const std::uint32_t expected_crc) {
    alignas(32) std::array<std::uint8_t, kChunkSize> buffer{};
    std::uint32_t state = support::kCrc32InitialState;
    std::uint32_t offset = 0U;
    while (offset < size) {
        const auto count =
            static_cast<std::size_t>(size - offset < buffer.size() ? size - offset : buffer.size());
        if (bflb_flash_read(address + offset, buffer.data(), static_cast<std::uint32_t>(count)) !=
            0) {
            return false;
        }
        state = support::crc32_update(state, buffer.data(), count);
        offset += static_cast<std::uint32_t>(count);
    }
    return support::crc32_finalize(state) == expected_crc;
}

[[nodiscard]] bool verify_native_hash(
    const std::uint32_t address, const std::uint8_t* const header) {
#if defined(BL602)
    auto* const sha = bflb_device_get_by_name(BFLB_NAME_SEC_SHA);
    if (sha == nullptr)
        return false;
    const auto length = read_u32(header + kImageLengthOffset);
    alignas(32) std::array<std::uint8_t, kChunkSize> buffer{};
    alignas(32) struct bflb_sha256_link_ctx_s context{};
    std::array<std::uint8_t, 32> digest{};

    if (bflb_sec_sha_mutex_take() != 0)
        return false;
    bflb_sha_link_init(sha);
    bflb_sha256_link_start(sha, &context, 0);
    std::memcpy(&g_update_sha_link_cfg, &context.link_cfg, sizeof(g_update_sha_link_cfg));
    context.link_addr =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_update_sha_link_cfg));
    bflb_sec_sha_mutex_give();

    std::uint32_t offset = 0U;
    while (offset < length) {
        const auto count = static_cast<std::size_t>(
            length - offset < buffer.size() ? length - offset : buffer.size());
        if (bflb_flash_read(address + kSectorSize + offset, buffer.data(),
                static_cast<std::uint32_t>(count)) != 0) {
            return false;
        }
        bflb_l1c_dcache_clean_range(buffer.data(), static_cast<std::uint32_t>(count));
        if (bflb_sec_sha_mutex_take() != 0)
            return false;
        bflb_sha_link_init(sha);
        std::memcpy(&g_update_sha_link_cfg, &context.link_cfg, sizeof(g_update_sha_link_cfg));
        context.link_addr =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_update_sha_link_cfg));
        const int result = bflb_sha256_link_update(
            sha, &context, buffer.data(), static_cast<std::uint32_t>(count));
        std::memcpy(&context.link_cfg, &g_update_sha_link_cfg, sizeof(g_update_sha_link_cfg));
        bflb_sec_sha_mutex_give();
        if (result != 0)
            return false;
        offset += static_cast<std::uint32_t>(count);
    }

    if (bflb_sec_sha_mutex_take() != 0)
        return false;
    bflb_sha_link_init(sha);
    std::memcpy(&g_update_sha_link_cfg, &context.link_cfg, sizeof(g_update_sha_link_cfg));
    context.link_addr =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_update_sha_link_cfg));
    bflb_sha256_link_finish(sha, &context, digest.data());
    std::memcpy(&context.link_cfg, &g_update_sha_link_cfg, sizeof(g_update_sha_link_cfg));
    bflb_sec_sha_mutex_give();
    return std::memcmp(digest.data(), header + kImageHashOffset, digest.size()) == 0;
#else
    // Wi-Fi6 runs from cached SRAM. Use the SDK software implementation so
    // SHA padding/context buffers do not cross the BL602-only DMA contract.
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
#if defined(CONFIG_MBEDTLS_V2)
    bool success = mbedtls_sha256_starts_ret(&context, 0) == 0;
#else
    bool success = mbedtls_sha256_starts(&context, 0) == 0;
#endif
    const auto length = read_u32(header + kImageLengthOffset);
    std::array<std::uint8_t, kChunkSize> buffer{};
    std::array<std::uint8_t, 32> digest{};
    for (std::uint32_t offset = 0; success && offset < length;) {
        const auto count = std::min<std::uint32_t>(length - offset, buffer.size());
        success = bflb_flash_read(address + kSectorSize + offset, buffer.data(), count) == 0;
        if (success) {
#if defined(CONFIG_MBEDTLS_V2)
            success = mbedtls_sha256_update_ret(&context, buffer.data(), count) == 0;
#else
            success = mbedtls_sha256_update(&context, buffer.data(), count) == 0;
#endif
        }
        offset += count;
    }
    if (success) {
#if defined(CONFIG_MBEDTLS_V2)
        success = mbedtls_sha256_finish_ret(&context, digest.data()) == 0;
#else
        success = mbedtls_sha256_finish(&context, digest.data()) == 0;
#endif
    }
    mbedtls_sha256_free(&context);
    return success && std::memcmp(digest.data(), header + kImageHashOffset, digest.size()) == 0;
#endif
}

class BouffaloRecoveryUpdateWriter final : public UpdateWriter {
  public:
    [[nodiscard]] bool available() const override {
        if (g_writer_lease.load())
            return false;
        pt_table_entry_config entry{};
        std::uint8_t active_table = 0U;
        return current_layout_matches(entry, active_table, true);
    }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        return static_cast<std::uint32_t>(update_detail::kEnvelopeSize) + NEWMOTA_APPLICATION_SIZE;
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return kChunkSize; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override {
        return update_detail::kEnvelopeSize + kBootHeaderSize;
    }

    [[nodiscard]] UpdateWriterResult begin(const std::uint32_t image_size) override {
        if (active_)
            return UpdateWriterResult::busy;
        bool expected = false;
        if (!g_writer_lease.compare_exchange_strong(expected, true)) {
            return UpdateWriterResult::busy;
        }
        pt_table_entry_config entry{};
        std::uint8_t active_table = 0U;
        if (image_size < minimum_initial_chunk_size() || image_size > maximum_image_size() ||
            !current_layout_matches(entry, active_table, true)) {
            g_writer_lease.store(false);
            return UpdateWriterResult::rejected;
        }
        artifact_size_ = image_size;
        received_bytes_ = 0U;
        payload_size_ = 0U;
        payload_received_ = 0U;
        payload_crc_state_ = support::kCrc32InitialState;
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
        const auto payload_start = update_detail::kEnvelopeSize;
        const auto chunk_end = static_cast<std::size_t>(received_bytes_) + bytes.size();
        if (chunk_end > payload_start) {
            const auto local_start =
                received_bytes_ < payload_start ? payload_start - received_bytes_ : 0U;
            const auto payload = bytes.substr(local_start);
            payload_crc_state_ =
                support::crc32_update(payload_crc_state_, payload.data(), payload.size());
            if (payload.empty() ||
                bflb_flash_write(NEWMOTA_APPLICATION_OFFSET + payload_received_,
                    reinterpret_cast<std::uint8_t*>(const_cast<char*>(payload.data())),
                    static_cast<std::uint32_t>(payload.size())) != 0) {
                return UpdateWriterResult::failed;
            }
            payload_received_ += static_cast<std::uint32_t>(payload.size());
        }
        received_bytes_ += static_cast<std::uint32_t>(bytes.size());
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult finish() override {
        if (!active_ || !initialized_ || received_bytes_ != artifact_size_ ||
            payload_received_ != payload_size_ ||
            support::crc32_finalize(payload_crc_state_) != envelope_.payload_crc32 ||
            !verify_flash_payload_crc(
                NEWMOTA_APPLICATION_OFFSET, payload_size_, envelope_.payload_crc32) ||
            !verify_native_hash(NEWMOTA_APPLICATION_OFFSET, native_header_.data())) {
            return UpdateWriterResult::failed;
        }
        verified_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult activate() override {
        if (!active_ || !verified_)
            return UpdateWriterResult::rejected;
        pt_table_entry_config entry{};
        std::uint8_t active_table = 0U;
        if (!current_layout_matches(entry, active_table, true)) {
            return UpdateWriterResult::failed;
        }
        entry.active_index = 0U;
        ++entry.age;
        const auto inactive_table = static_cast<pt_table_id_type>(
            active_table == PT_TABLE_ID_0 ? PT_TABLE_ID_1 : PT_TABLE_ID_0);
        if (pt_table_update_entry(inactive_table, bflb_boot2_get_pt_config(), &entry) !=
            PT_ERROR_SUCCESS) {
            return UpdateWriterResult::failed;
        }
        pt_table_entry_config check{};
        std::uint8_t selected_table = 0U;
        if (!current_layout_matches(check, selected_table, false) ||
            selected_table != inactive_table || check.active_index != 0U) {
            return UpdateWriterResult::failed;
        }
        release_lease();
        return UpdateWriterResult::success;
    }

    void abort() override { release_lease(); }

  private:
    [[nodiscard]] bool initialize_from_prefix(const std::string_view bytes) {
        std::uint8_t digest[update_detail::kEnvelopeDigestSize]{};
        if (!update_detail::decode_envelope(bytes, artifact_size_, envelope_) ||
            !update_detail::decode_layout_digest(kLayoutSha256, digest) ||
            !update_detail::envelope_matches(envelope_, NEWMOTA_UPDATE_CHIP_ID,
                NEWMOTA_UPDATE_FLASH_BYTES, digest, NEWMOTA_APPLICATION_OFFSET,
                NEWMOTA_APPLICATION_SIZE, NEWMOTA_SAFEBOOT_OFFSET, NEWMOTA_SAFEBOOT_SIZE,
                NEWMOTA_FILESYSTEM_OFFSET, NEWMOTA_FILESYSTEM_SIZE) ||
            envelope_.payload_size > NEWMOTA_APPLICATION_SIZE ||
            bytes.size() < update_detail::kEnvelopeSize + kBootHeaderSize ||
            !validate_native_header(
                reinterpret_cast<const std::uint8_t*>(bytes.data() + update_detail::kEnvelopeSize),
                envelope_.payload_size, native_header_.data())) {
            return false;
        }
        pt_table_entry_config entry{};
        std::uint8_t active_table = 0U;
        if (!current_layout_matches(entry, active_table, true))
            return false;

        const auto erase_length = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(envelope_.payload_size) + kSectorSize - 1U) / kSectorSize *
            kSectorSize);
        if (erase_length == 0U || erase_length > NEWMOTA_APPLICATION_SIZE ||
            bflb_flash_erase(NEWMOTA_APPLICATION_OFFSET, erase_length) != 0) {
            return false;
        }
        payload_size_ = envelope_.payload_size;
        initialized_ = true;
        return true;
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
        g_writer_lease.store(false);
    }

    bool active_{};
    bool initialized_{};
    bool verified_{};
    std::uint32_t artifact_size_{};
    std::uint32_t received_bytes_{};
    std::uint32_t payload_size_{};
    std::uint32_t payload_received_{};
    std::uint32_t payload_crc_state_{support::kCrc32InitialState};
    update_detail::UpdateEnvelopeV1 envelope_{};
    std::array<std::uint8_t, kBootHeaderSize> native_header_{};
};

// Construct before the scheduler starts, alongside the other platform services.
BouffaloRecoveryUpdateWriter g_recovery_writer;

} // namespace

UpdateWriter& recovery_update_writer() {
    return g_recovery_writer;
}

/** @brief Returns the application-slot writer used by Safeboot browser updates. */
UpdateWriter& browser_update_writer() {
    return recovery_update_writer();
}

} // namespace newmota::platform
