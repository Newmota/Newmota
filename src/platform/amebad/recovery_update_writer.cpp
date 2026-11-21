/*
  recovery_update_writer.cpp - bounded AmebaD Safeboot OTA2 writer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "recovery_update_writer.h"

#include "newmota/platform/update_envelope.h"
#include "support/crc32.h"

extern "C" {
#include "ameba_soc.h"
#include "device_lock.h"
#include "rtl8721d_flash.h"
#include "rtl8721d_ota.h"
}

#include <array>
#include <atomic>
#include <string.h>

#ifndef NEWMOTA_IMAGE_SAFEBOOT
#error "The AmebaD recovery writer can only be compiled into Safeboot"
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

#ifndef NEWMOTA_OTA1_OFFSET
#error "NEWMOTA_OTA1_OFFSET is required"
#endif

#ifndef NEWMOTA_OTA1_SIZE
#error "NEWMOTA_OTA1_SIZE is required"
#endif

#ifndef NEWMOTA_OTA2_OFFSET
#error "NEWMOTA_OTA2_OFFSET is required"
#endif

#ifndef NEWMOTA_OTA2_SIZE
#error "NEWMOTA_OTA2_SIZE is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_OFFSET
#error "NEWMOTA_FILESYSTEM_OFFSET is required"
#endif

#ifndef NEWMOTA_FILESYSTEM_SIZE
#error "NEWMOTA_FILESYSTEM_SIZE is required"
#endif

extern "C" int ota_readstream_user(std::uint32_t address, std::uint32_t length, std::uint8_t* data);

namespace newmota::platform {
namespace {

constexpr std::size_t kChunkSize = 4096U;
constexpr std::uint32_t kSectorSize = 4096U;
constexpr std::uint32_t kOtaContainerHeaderSize = 32U;
constexpr std::uint32_t kNativeSignatureSize = 8U;
constexpr std::uint32_t kImageSignature0 = 0x35393138U;
constexpr std::uint32_t kImageSignature1 = 0x31313738U;
constexpr std::uint32_t kImageChecksumOffset = 16U;
constexpr std::uint32_t kImageLengthOffset = 20U;
constexpr std::uint32_t kImageOffsetOffset = 24U;
constexpr std::uint32_t kImageFlashAddressOffset = 28U;
constexpr std::uint32_t kOtaFileVersion = UINT32_MAX;
constexpr std::uint32_t kOtaHeaderCount = 1U;
constexpr std::uint32_t kOtaHeaderLength = 24U;
constexpr char kLayoutSha256[] = NEWMOTA_UPDATE_LAYOUT_SHA256;

std::atomic<bool> g_writer_lease{false};
// Safeboot's Newmota task has an 8 KiB minimum stack; keep the read-back
// workspace in static storage so its buffer does not use task stack space.
alignas(4) std::array<std::uint8_t, kChunkSize> g_readback_buffer{};

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] std::uint8_t mapped_ota_index() {
    const auto& entry = RSIP_BASE->FLASH_MMU[0];
    const std::uint32_t control = entry.MMU_ENTRYx_CTRL;
    if ((control & MMU_BIT_ENTRY_VALID) == 0U)
        return 0U;
    const std::uint32_t start = entry.MMU_ENTRYx_STRADDR;
    const std::uint32_t delta = entry.MMU_ENTRYx_OFFSET;
    const std::uint32_t physical =
        (control & MMU_BIT_ENTRY_OFFSET_MINUS) != 0U ? start - delta : start + delta;
    if (physical == SPI_FLASH_BASE + NEWMOTA_OTA1_OFFSET)
        return 1U;
    if (physical == SPI_FLASH_BASE + NEWMOTA_OTA2_OFFSET)
        return 2U;
    return 0U;
}

[[nodiscard]] bool valid_signature(
    const std::uint32_t offset, std::uint8_t* const signature = nullptr) {
    std::array<std::uint32_t, 2> words{};
    auto* const bytes = reinterpret_cast<std::uint8_t*>(words.data());
    if (ota_readstream_user(offset, static_cast<std::uint32_t>(words.size() * 4U), bytes) != 1) {
        return false;
    }
    if (signature != nullptr)
        memcpy(signature, bytes, kNativeSignatureSize);
    return words[0] == kImageSignature0 && words[1] == kImageSignature1;
}

[[nodiscard]] bool validate_ota_all_header(const std::uint8_t* const bytes,
    const std::uint32_t payload_size, std::uint32_t& image_length, std::uint32_t& image_checksum,
    std::uint8_t* const signature) {
    if (bytes == nullptr || payload_size < kOtaContainerHeaderSize + kNativeSignatureSize) {
        return false;
    }
    if (read_u32(bytes) != kOtaFileVersion || read_u32(bytes + 4U) != kOtaHeaderCount ||
        memcmp(bytes + 8U, "OTA1", 4U) != 0 || read_u32(bytes + 12U) != kOtaHeaderLength ||
        read_u32(bytes + kImageOffsetOffset) != kOtaContainerHeaderSize ||
        read_u32(bytes + kImageFlashAddressOffset) != UINT32_MAX) {
        return false;
    }
    image_checksum = read_u32(bytes + kImageChecksumOffset);
    image_length = read_u32(bytes + kImageLengthOffset);
    if (image_length < kNativeSignatureSize ||
        static_cast<std::uint64_t>(image_length) + kOtaContainerHeaderSize != payload_size ||
        image_length > NEWMOTA_OTA2_SIZE) {
        return false;
    }
    memcpy(signature, bytes + kOtaContainerHeaderSize, kNativeSignatureSize);
    return read_u32(signature) == kImageSignature0 && read_u32(signature + 4U) == kImageSignature1;
}

[[nodiscard]] bool readback_matches(std::uint32_t image_length, std::uint32_t expected_checksum,
    std::uint32_t expected_crc, const std::uint8_t* const signature) {
    std::uint32_t checksum = 0U;
    for (std::size_t index = 0; index < kNativeSignatureSize; ++index) {
        checksum += signature[index];
    }
    std::uint32_t crc_state =
        support::crc32_update(support::kCrc32InitialState, signature, kNativeSignatureSize);
    std::uint32_t offset = kNativeSignatureSize;
    while (offset < image_length) {
        const auto count = static_cast<std::size_t>(image_length - offset < g_readback_buffer.size()
                                                        ? image_length - offset
                                                        : g_readback_buffer.size());
        if (ota_readstream_user(NEWMOTA_OTA2_OFFSET + offset, static_cast<std::uint32_t>(count),
                g_readback_buffer.data()) != 1) {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            checksum += g_readback_buffer[index];
        }
        crc_state = support::crc32_update(crc_state, g_readback_buffer.data(), count);
        offset += static_cast<std::uint32_t>(count);
    }
    std::array<std::uint8_t, kNativeSignatureSize> erased_signature{};
    if (ota_readstream_user(
            NEWMOTA_OTA2_OFFSET, erased_signature.size(), erased_signature.data()) != 1) {
        return false;
    }
    for (const auto byte : erased_signature) {
        if (byte != 0xFFU)
            return false;
    }
    return checksum == expected_checksum && support::crc32_finalize(crc_state) == expected_crc;
}

class AmebadRecoveryUpdateWriter final : public UpdateWriter {
  public:
    [[nodiscard]] bool available() const override {
        return !g_writer_lease.load() && mapped_ota_index() == 1U &&
               valid_signature(NEWMOTA_OTA1_OFFSET);
    }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        return static_cast<std::uint32_t>(update_detail::kEnvelopeSize) + kOtaContainerHeaderSize +
               NEWMOTA_OTA2_SIZE;
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return kChunkSize; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override {
        return update_detail::kEnvelopeSize + kOtaContainerHeaderSize + kNativeSignatureSize;
    }

    [[nodiscard]] UpdateWriterResult begin(const std::uint32_t image_size) override {
        if (active_)
            return UpdateWriterResult::busy;
        bool expected = false;
        if (!g_writer_lease.compare_exchange_strong(expected, true)) {
            return UpdateWriterResult::busy;
        }
        if (image_size < minimum_initial_chunk_size() || image_size > maximum_image_size() ||
            mapped_ota_index() != 1U || !valid_signature(NEWMOTA_OTA1_OFFSET)) {
            g_writer_lease.store(false);
            return UpdateWriterResult::rejected;
        }
        artifact_size_ = image_size;
        received_bytes_ = 0U;
        payload_received_ = 0U;
        payload_size_ = 0U;
        image_length_ = 0U;
        image_checksum_ = 0U;
        payload_crc_state_ = support::kCrc32InitialState;
        native_crc_state_ = support::kCrc32InitialState;
        native_checksum_state_ = 0U;
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

        const std::size_t envelope_header_size = update_detail::kEnvelopeSize;
        const std::size_t chunk_end = static_cast<std::size_t>(received_bytes_) + bytes.size();
        if (chunk_end > envelope_header_size) {
            const std::size_t local_start = received_bytes_ < envelope_header_size
                                                ? envelope_header_size - received_bytes_
                                                : 0U;
            const auto payload = bytes.substr(local_start);
            payload_crc_state_ =
                support::crc32_update(payload_crc_state_, payload.data(), payload.size());
            if (!write_ota_all_payload(payload))
                return UpdateWriterResult::failed;
            payload_received_ += static_cast<std::uint32_t>(payload.size());
        }
        received_bytes_ += static_cast<std::uint32_t>(bytes.size());
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult finish() override {
        image_crc32_ = support::crc32_finalize(native_crc_state_);
        if (!active_ || !initialized_ || received_bytes_ != artifact_size_ ||
            payload_received_ != payload_size_ ||
            support::crc32_finalize(payload_crc_state_) != envelope_.payload_crc32 ||
            native_checksum_state_ != image_checksum_ ||
            !readback_matches(image_length_, image_checksum_, image_crc32_, signature_.data())) {
            return UpdateWriterResult::failed;
        }
        verified_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult activate() override {
        if (!active_ || !verified_ || mapped_ota_index() != 1U ||
            !valid_signature(NEWMOTA_OTA1_OFFSET)) {
            return UpdateWriterResult::rejected;
        }
        std::array<std::uint8_t, kNativeSignatureSize> old_signature{};
        if (!valid_signature(NEWMOTA_OTA2_OFFSET, old_signature.data())) {
            device_mutex_lock(RT_DEV_LOCK_FLASH);
            const int write_result =
                FLASH_WriteStream(NEWMOTA_OTA2_OFFSET, signature_.size(), signature_.data());
            device_mutex_unlock(RT_DEV_LOCK_FLASH);
            std::array<std::uint8_t, kNativeSignatureSize> verified_signature{};
            if (write_result != 1 ||
                !valid_signature(NEWMOTA_OTA2_OFFSET, verified_signature.data()) ||
                memcmp(verified_signature.data(), signature_.data(), signature_.size()) != 0) {
                const std::array<std::uint8_t, kNativeSignatureSize> invalid{};
                device_mutex_lock(RT_DEV_LOCK_FLASH);
                (void)FLASH_WriteStream(
                    NEWMOTA_OTA2_OFFSET, invalid.size(), const_cast<std::uint8_t*>(invalid.data()));
                device_mutex_unlock(RT_DEV_LOCK_FLASH);
                return UpdateWriterResult::failed;
            }
        }
        release_lease();
        return UpdateWriterResult::success;
    }

    void abort() override { release_lease(); }

  private:
    [[nodiscard]] bool initialize_from_prefix(const std::string_view bytes) {
        std::uint8_t digest[update_detail::kEnvelopeDigestSize]{};
        const auto prefix = reinterpret_cast<const std::uint8_t*>(bytes.data());
        if (!update_detail::decode_envelope(bytes, artifact_size_, envelope_) ||
            !update_detail::decode_layout_digest(kLayoutSha256, digest) ||
            !update_detail::envelope_matches(envelope_, NEWMOTA_UPDATE_CHIP_ID,
                NEWMOTA_UPDATE_FLASH_BYTES, digest, NEWMOTA_OTA2_OFFSET, NEWMOTA_OTA2_SIZE,
                NEWMOTA_OTA1_OFFSET, NEWMOTA_OTA1_SIZE, NEWMOTA_FILESYSTEM_OFFSET,
                NEWMOTA_FILESYSTEM_SIZE) ||
            !validate_ota_all_header(prefix + update_detail::kEnvelopeSize, envelope_.payload_size,
                image_length_, image_checksum_, signature_.data()) ||
            envelope_.payload_size > NEWMOTA_OTA2_SIZE + kOtaContainerHeaderSize ||
            bytes.size() < minimum_initial_chunk_size() || mapped_ota_index() != 1U ||
            !valid_signature(NEWMOTA_OTA1_OFFSET)) {
            return false;
        }
        device_mutex_lock(RT_DEV_LOCK_FLASH);
        for (std::uint32_t offset = 0U; offset < NEWMOTA_OTA2_SIZE; offset += kSectorSize) {
            FLASH_EraseXIP(EraseSector, NEWMOTA_OTA2_OFFSET + offset);
        }
        device_mutex_unlock(RT_DEV_LOCK_FLASH);
        native_crc_state_ = support::crc32_update(
            support::kCrc32InitialState, signature_.data(), signature_.size());
        for (const auto byte : signature_)
            native_checksum_state_ += byte;
        payload_size_ = envelope_.payload_size;
        initialized_ = true;
        return true;
    }

    [[nodiscard]] bool write_ota_all_payload(const std::string_view payload) {
        const auto* const data = reinterpret_cast<const std::uint8_t*>(payload.data());
        std::uint32_t position = payload_received_;
        std::size_t input = 0U;
        while (input < payload.size()) {
            if (position < kOtaContainerHeaderSize) {
                const auto skip = static_cast<std::size_t>(
                    kOtaContainerHeaderSize - position < payload.size() - input
                        ? kOtaContainerHeaderSize - position
                        : payload.size() - input);
                input += skip;
                position += static_cast<std::uint32_t>(skip);
                continue;
            }
            if (position < kOtaContainerHeaderSize + kNativeSignatureSize) {
                const auto skip = static_cast<std::size_t>(
                    kOtaContainerHeaderSize + kNativeSignatureSize - position <
                            payload.size() - input
                        ? kOtaContainerHeaderSize + kNativeSignatureSize - position
                        : payload.size() - input);
                input += skip;
                position += static_cast<std::uint32_t>(skip);
                continue;
            }
            const auto count = payload.size() - input;
            const auto native_offset = position - kOtaContainerHeaderSize;
            const auto* const chunk = data + input;
            native_crc_state_ = support::crc32_update(native_crc_state_, chunk, count);
            for (std::size_t index = 0; index < count; ++index) {
                native_checksum_state_ += chunk[index];
            }
            device_mutex_lock(RT_DEV_LOCK_FLASH);
            const int result = FLASH_WriteStream(NEWMOTA_OTA2_OFFSET + native_offset,
                static_cast<std::uint32_t>(count), const_cast<std::uint8_t*>(chunk));
            device_mutex_unlock(RT_DEV_LOCK_FLASH);
            if (result != 1)
                return false;
            input += count;
            position += static_cast<std::uint32_t>(count);
        }
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
        payload_received_ = 0U;
        payload_size_ = 0U;
        image_length_ = 0U;
        g_writer_lease.store(false);
    }

    bool active_{};
    bool initialized_{};
    bool verified_{};
    std::uint32_t artifact_size_{};
    std::uint32_t received_bytes_{};
    std::uint32_t payload_received_{};
    std::uint32_t payload_size_{};
    std::uint32_t image_length_{};
    std::uint32_t image_checksum_{};
    std::uint32_t image_crc32_{};
    std::uint32_t payload_crc_state_{support::kCrc32InitialState};
    std::uint32_t native_crc_state_{support::kCrc32InitialState};
    std::uint32_t native_checksum_state_{};
    update_detail::UpdateEnvelopeV1 envelope_{};
    std::array<std::uint8_t, kNativeSignatureSize> signature_{};
};

} // namespace

UpdateWriter& recovery_update_writer() {
    static AmebadRecoveryUpdateWriter writer;
    return writer;
}

/** @brief Returns the application-slot writer used by Safeboot browser updates. */
UpdateWriter& browser_update_writer() {
    return recovery_update_writer();
}

} // namespace newmota::platform
