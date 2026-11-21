/*
  recovery_update_writer.cpp - bounded ESP-IDF Safeboot application writer

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "recovery_update_writer.h"

#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "image_descriptor.h"

#include <array>
#include <atomic>
#include <cstring>

#ifndef NEWMOTA_IMAGE_SAFEBOOT
#error "The ESP recovery writer can only be compiled into Safeboot"
#endif

#ifndef NEWMOTA_ESP_LAYOUT_ID
#error "NEWMOTA_ESP_LAYOUT_ID is required"
#endif

#ifndef NEWMOTA_ESP_APP0_OFFSET
#error "NEWMOTA_ESP_APP0_OFFSET is required"
#endif

#ifndef NEWMOTA_ESP_SAFEBOOT_OFFSET
#error "NEWMOTA_ESP_SAFEBOOT_OFFSET is required"
#endif

#ifndef NEWMOTA_ESP_OTADATA_OFFSET
#error "NEWMOTA_ESP_OTADATA_OFFSET is required"
#endif

#ifndef NEWMOTA_ESP_NVS_OFFSET
#error "NEWMOTA_ESP_NVS_OFFSET is required"
#endif

#ifndef NEWMOTA_ESP_PSRAM_BYTES
#error "NEWMOTA_ESP_PSRAM_BYTES is required"
#endif

#ifndef NEWMOTA_ESP_SDK_TARGET
#error "NEWMOTA_ESP_SDK_TARGET is required"
#endif

namespace newmota::platform {
namespace {

constexpr std::size_t kUpdateChunkSize = 4096U;
constexpr std::size_t kMinimumImagePrefixSize =
    esp_idf::kCustomDescriptorOffset + sizeof(esp_idf::NewmotaEspImageDescriptor);
constexpr std::uint32_t kMinimumNvsBytes = 0x1000U;
constexpr std::uint32_t kMinimumOtaDataBytes = 0x2000U;
constexpr std::uint32_t kApplicationDescriptionMagic = ESP_APP_DESC_MAGIC_WORD;
constexpr std::array<char, 8> kDescriptorMagic{'N', 'E', 'W', 'M', 'O', 'T', 'A', '\0'};
constexpr char kExpectedSdkTarget[16] = NEWMOTA_ESP_SDK_TARGET;
constexpr char kExpectedLayoutId[65] = NEWMOTA_ESP_LAYOUT_ID;

bool descriptor_values_match(const esp_idf::NewmotaEspImageDescriptor& descriptor);

/** @brief Checks the standard ESP app description and Newmota layout binding. */
bool descriptor_matches(const void* const bytes, const std::size_t size) {
    using esp_idf::kCustomDescriptorOffset;
    using esp_idf::NewmotaEspImageDescriptor;
    if (bytes == nullptr || size < kCustomDescriptorOffset + sizeof(NewmotaEspImageDescriptor)) {
        return false;
    }
    const auto* const data = static_cast<const std::uint8_t*>(bytes);
    esp_app_desc_t app_description{};
    std::memcpy(&app_description,
        data + esp_idf::kImageHeaderSize + esp_idf::kImageSegmentHeaderSize,
        sizeof(app_description));
    if (app_description.magic_word != kApplicationDescriptionMagic)
        return false;

    NewmotaEspImageDescriptor descriptor{};
    std::memcpy(&descriptor, data + kCustomDescriptorOffset, sizeof(descriptor));
    return descriptor_values_match(descriptor);
}

/** @brief Compares one decoded descriptor with the running recovery layout. */
bool descriptor_values_match(const esp_idf::NewmotaEspImageDescriptor& descriptor) {
    using esp_idf::kApplicationImageRole;
    using esp_idf::kCustomDescriptorSchema;
    return std::memcmp(descriptor.magic, kDescriptorMagic.data(), kDescriptorMagic.size()) == 0 &&
           descriptor.schema == kCustomDescriptorSchema &&
           descriptor.descriptor_size == sizeof(descriptor) &&
           descriptor.image_role == kApplicationImageRole &&
           descriptor.app0_offset == NEWMOTA_ESP_APP0_OFFSET &&
           descriptor.safeboot_offset == NEWMOTA_ESP_SAFEBOOT_OFFSET &&
           descriptor.otadata_offset == NEWMOTA_ESP_OTADATA_OFFSET &&
           descriptor.nvs_offset == NEWMOTA_ESP_NVS_OFFSET &&
           std::memcmp(descriptor.sdk_target, kExpectedSdkTarget, sizeof(descriptor.sdk_target)) ==
               0 &&
           std::memcmp(descriptor.layout_id, kExpectedLayoutId, sizeof(descriptor.layout_id)) ==
               0 &&
           descriptor.psram_bytes <= NEWMOTA_ESP_PSRAM_BYTES;
}

/** @brief Checks nonempty, in-flash partition bounds without integer wrap. */
bool partition_in_flash(const esp_partition_t* const partition, const std::uint32_t flash_bytes,
    const std::uint32_t minimum_size = 1U) {
    if (partition == nullptr || partition->size < minimum_size)
        return false;
    const auto end = static_cast<std::uint64_t>(partition->address) +
                     static_cast<std::uint64_t>(partition->size);
    return end <= flash_bytes;
}

[[nodiscard]] bool ranges_overlap(std::uint32_t first_address, std::uint32_t first_size,
    std::uint32_t second_address, std::uint32_t second_size);

/** @brief Rejects an app slot that overlaps any other declared partition. */
bool application_slot_is_isolated(const esp_partition_t* const application) {
    if (application == nullptr)
        return false;
    auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator != nullptr) {
        const auto* const partition = esp_partition_get(iterator);
        const bool same_application =
            partition != nullptr && partition->address == application->address &&
            partition->size == application->size && partition->type == application->type &&
            partition->subtype == application->subtype &&
            std::strcmp(partition->label, application->label) == 0;
        if (partition == nullptr ||
            (!same_application && ranges_overlap(application->address, application->size,
                                      partition->address, partition->size))) {
            esp_partition_iterator_release(iterator);
            return false;
        }
        iterator = esp_partition_next(iterator);
    }
    return true;
}

/** @brief Verifies the running device's full Newmota boot-layout identity. */
bool runtime_layout_matches(const esp_partition_t* const nvs, const esp_partition_t* const ota_data,
    const esp_partition_t* const recovery, const esp_partition_t* const application) {
    std::uint32_t flash_bytes = 0U;
    if (esp_flash_get_size(nullptr, &flash_bytes) != ESP_OK ||
        !partition_in_flash(nvs, flash_bytes, kMinimumNvsBytes) ||
        !partition_in_flash(ota_data, flash_bytes, kMinimumOtaDataBytes) ||
        !partition_in_flash(recovery, flash_bytes) ||
        !partition_in_flash(application, flash_bytes) || nvs->address != NEWMOTA_ESP_NVS_OFFSET ||
        ota_data->address != NEWMOTA_ESP_OTADATA_OFFSET ||
        recovery->address != NEWMOTA_ESP_SAFEBOOT_OFFSET ||
        application->address != NEWMOTA_ESP_APP0_OFFSET) {
        return false;
    }
    return application_slot_is_isolated(application);
}

[[nodiscard]] bool ranges_overlap(const std::uint32_t first_address, const std::uint32_t first_size,
    const std::uint32_t second_address, const std::uint32_t second_size) {
    const auto first_end = static_cast<std::uint64_t>(first_address) + first_size;
    const auto second_end = static_cast<std::uint64_t>(second_address) + second_size;
    return first_address < second_end && second_address < first_end;
}

/** @brief Validates native ESP image/chip headers before the target is erased. */
bool valid_image_prefix(const std::string_view bytes, const std::uint32_t expected_image_size) {
    using esp_idf::kCustomDescriptorOffset;
    using esp_idf::NewmotaEspImageDescriptor;
    if (bytes.size() < kCustomDescriptorOffset + sizeof(NewmotaEspImageDescriptor)) {
        return false;
    }
    esp_image_header_t image_header{};
    std::memcpy(&image_header, bytes.data(), sizeof(image_header));
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image_header.segment_count == 0U ||
        image_header.segment_count > ESP_IMAGE_MAX_SEGMENTS ||
        bootloader_common_check_chip_validity(&image_header, ESP_IMAGE_APPLICATION) != ESP_OK) {
        return false;
    }
    esp_image_segment_header_t segment_header{};
    std::memcpy(&segment_header, bytes.data() + sizeof(image_header), sizeof(segment_header));
    if (segment_header.data_len < sizeof(esp_app_desc_t) + sizeof(NewmotaEspImageDescriptor) ||
        segment_header.data_len >
            expected_image_size - sizeof(image_header) - sizeof(segment_header)) {
        return false;
    }
    return descriptor_matches(bytes.data(), bytes.size());
}

class EspRecoveryUpdateWriter final : public UpdateWriter {
  public:
    [[nodiscard]] bool available() const override {
        const auto* const running = esp_ota_get_running_partition();
        const auto* const boot = esp_ota_get_boot_partition();
        const auto* const recovery = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "safeboot");
        const auto* const application = app_partition();
        const auto* const ota_data = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
        const auto* const nvs = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        const bool recovery_is_running = running != nullptr &&
                                         running->address == NEWMOTA_ESP_SAFEBOOT_OFFSET &&
                                         std::strcmp(running->label, "safeboot") == 0;
        const bool recovery_is_selected = boot != nullptr &&
                                          boot->address == NEWMOTA_ESP_SAFEBOOT_OFFSET &&
                                          std::strcmp(boot->label, "safeboot") == 0;
        return esp_idf::recovery_partition_is_selected(recovery_is_running, recovery_is_selected) &&
               recovery != nullptr && application != nullptr && ota_data != nullptr &&
               nvs != nullptr && recovery->address == NEWMOTA_ESP_SAFEBOOT_OFFSET &&
               runtime_layout_matches(nvs, ota_data, recovery, application);
    }

    [[nodiscard]] std::uint32_t maximum_image_size() const override {
        const auto* const application = app_partition();
        return available() && application != nullptr ? application->size : 0U;
    }

    [[nodiscard]] std::size_t maximum_chunk_size() const override { return kUpdateChunkSize; }

    [[nodiscard]] std::size_t minimum_initial_chunk_size() const override {
        return kMinimumImagePrefixSize;
    }

    [[nodiscard]] UpdateWriterResult begin(const std::uint32_t image_size) override {
        if (lease_active_.test_and_set(std::memory_order_acquire))
            return UpdateWriterResult::busy;
        if (!available()) {
            lease_active_.clear(std::memory_order_release);
            return UpdateWriterResult::unavailable;
        }
        const auto* const application = app_partition();
        if (application == nullptr || image_size == 0U || image_size > application->size) {
            lease_active_.clear(std::memory_order_release);
            return UpdateWriterResult::rejected;
        }
        target_ = application;
        expected_bytes_ = image_size;
        received_bytes_ = 0U;
        receiving_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult write(
        const std::uint32_t offset, const std::string_view bytes) override {
        if (!receiving_ || target_ == nullptr || bytes.empty() || bytes.size() > kUpdateChunkSize ||
            offset != received_bytes_ || bytes.size() > expected_bytes_ - received_bytes_) {
            return UpdateWriterResult::rejected;
        }
        if (received_bytes_ == 0U) {
            if (!valid_image_prefix(bytes, expected_bytes_))
                return UpdateWriterResult::rejected;
            if (esp_ota_begin(target_, expected_bytes_, &handle_) != ESP_OK)
                return UpdateWriterResult::failed;
            handle_valid_ = true;
        }
        if (!handle_valid_ || esp_ota_write(handle_, bytes.data(), bytes.size()) != ESP_OK) {
            return UpdateWriterResult::failed;
        }
        received_bytes_ += static_cast<std::uint32_t>(bytes.size());
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult finish() override {
        if (!receiving_ || !handle_valid_ || target_ == nullptr ||
            received_bytes_ != expected_bytes_) {
            return UpdateWriterResult::rejected;
        }
        const auto handle = handle_;
        handle_valid_ = false;
        handle_ = 0;
        const auto image_result = esp_ota_end(handle);
        if (image_result != ESP_OK) {
            return image_result == ESP_ERR_OTA_VALIDATE_FAILED ? UpdateWriterResult::rejected
                                                               : UpdateWriterResult::failed;
        }
        esp_idf::NewmotaEspImageDescriptor descriptor{};
        if (esp_partition_read(target_, esp_idf::kCustomDescriptorOffset, &descriptor,
                sizeof(descriptor)) != ESP_OK ||
            !descriptor_values_match(descriptor)) {
            return UpdateWriterResult::rejected;
        }
        receiving_ = false;
        verified_ = true;
        return UpdateWriterResult::success;
    }

    [[nodiscard]] UpdateWriterResult activate() override {
        if (!verified_ || target_ == nullptr)
            return UpdateWriterResult::rejected;
        if (esp_ota_set_boot_partition(target_) != ESP_OK)
            return UpdateWriterResult::failed;
        verified_ = false;
        target_ = nullptr;
        expected_bytes_ = 0U;
        received_bytes_ = 0U;
        lease_active_.clear(std::memory_order_release);
        return UpdateWriterResult::success;
    }

    void abort() override {
        if (handle_valid_)
            (void)esp_ota_abort(handle_);
        handle_ = 0;
        handle_valid_ = false;
        receiving_ = false;
        verified_ = false;
        target_ = nullptr;
        expected_bytes_ = 0U;
        received_bytes_ = 0U;
        lease_active_.clear(std::memory_order_release);
    }

  private:
    [[nodiscard]] static const esp_partition_t* app_partition() {
        const auto* const application = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app0");
        return application != nullptr && application->address == NEWMOTA_ESP_APP0_OFFSET
                   ? application
                   : nullptr;
    }

    const esp_partition_t* target_{};
    esp_ota_handle_t handle_{};
    std::uint32_t expected_bytes_{};
    std::uint32_t received_bytes_{};
    std::atomic_flag lease_active_ = ATOMIC_FLAG_INIT;
    bool handle_valid_{};
    bool receiving_{};
    bool verified_{};
};

} // namespace

UpdateWriter& recovery_update_writer() {
    static EspRecoveryUpdateWriter writer;
    return writer;
}

/** @brief Returns the application-slot writer used by Safeboot browser updates. */
UpdateWriter& browser_update_writer() {
    return recovery_update_writer();
}

} // namespace newmota::platform
