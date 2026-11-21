/*
  filesystem.cpp - ESP-IDF filesystem block device for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/block_device.h"

#include "esp_partition.h"

namespace newmota::platform {
namespace {

constexpr std::uint32_t kReadSize = 1;
constexpr std::uint32_t kUnencryptedProgramSize = 1;
constexpr std::uint32_t kEncryptedProgramSize = 16;
constexpr char kPartitionLabel[] = "spiffs";

/** @brief Reads a range from the selected ESP-IDF data partition. */
bool read_partition(const void* const context, const std::uint32_t offset, void* const buffer,
    const std::size_t size) {
    const auto* const partition = static_cast<const esp_partition_t*>(context);
    return esp_partition_read(partition, offset, buffer, size) == ESP_OK;
}

/** @brief Programs a range in the selected ESP-IDF data partition. */
bool program_partition(const void* const context, const std::uint32_t offset,
    const void* const buffer, const std::size_t size) {
    const auto* const partition = static_cast<const esp_partition_t*>(context);
    return esp_partition_write(partition, offset, buffer, size) == ESP_OK;
}

/** @brief Erases an aligned range in the selected ESP-IDF data partition. */
bool erase_partition(
    const void* const context, const std::uint32_t offset, const std::size_t size) {
    const auto* const partition = static_cast<const esp_partition_t*>(context);
    return esp_partition_erase_range(partition, offset, size) == ESP_OK;
}

/** @brief Completes a block-device sync; ESP-IDF writes are already synchronous. */
bool sync_partition(const void*) {
    return true;
}

} // namespace

/**
 * @brief Opens the writable Tasmota-compatible filesystem partition.
 * @return true when @p device was populated.
 */
bool open_filesystem_block_device(BlockDevice& device) {
    const auto* const partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, kPartitionLabel);
    if (partition == nullptr || partition->readonly) {
        return false;
    }
    device = BlockDevice{partition, partition->size, kReadSize,
        partition->encrypted ? kEncryptedProgramSize : kUnencryptedProgramSize,
        partition->erase_size, read_partition, program_partition, erase_partition, sync_partition};
    return true;
}

} // namespace newmota::platform
