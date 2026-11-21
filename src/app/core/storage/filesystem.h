/*
  filesystem.h - portable filesystem interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::platform {
struct SdCardConfig;
} // namespace newmota::platform

namespace newmota::filesystem {

enum class MountResult : std::uint8_t {
    mounted,
    already_mounted,
    unavailable,
    invalid_device,
    mount_failed,
    grow_failed,
    invalid_pins,  /**< SD card pins cannot be routed or are owned elsewhere. */
    no_card,       /**< No SD card answered. */
    no_filesystem, /**< The SD card holds no FAT volume. */
};

/**
 * @brief A mounted volume. With `USE_SDCARD`, `/sd` and every path below it belong to the
 * SD card; all other paths belong to the LittleFS flash volume.
 */
enum class Volume : std::uint8_t {
    flash,
    sd,
};

struct SpaceInfo final {
    std::uint64_t total_bytes;
    std::uint64_t used_bytes;
};

enum class EntryType : std::uint8_t {
    file,
    directory,
};

struct DirectoryEntry final {
    std::string_view name;
    std::uint32_t size_bytes;
    EntryType type;
};

enum class DirectoryResult : std::uint8_t {
    listed,
    unavailable,
    invalid_path,
    open_failed,
    read_failed,
    output_stopped,
};

enum class MutationResult : std::uint8_t {
    succeeded,
    unavailable,
    invalid_path,
    failed,
};

enum class ReadResult : std::uint8_t {
    succeeded,
    unavailable,
    invalid_path,
    not_found,
    too_large,
    failed,
};

/** @brief Opaque state for one sequential filesystem reader. */
struct FileReader final {
    void* native{};
};

/** @brief Opaque state for a bounded sequential writer. */
struct FileWriter final {
    void* native{};
};

/** @brief Opens a file for replacement with bounded sequential writes. */
[[nodiscard]] MutationResult open_writer(std::string_view path, FileWriter& writer);
/** @brief Writes all supplied bytes or reports the native write failure. */
[[nodiscard]] MutationResult write(FileWriter& writer, const void* data, std::size_t size);
/** @brief Synchronizes, closes, and releases a writer even after failure. */
[[nodiscard]] bool close(FileWriter& writer);
/** @brief Positions a live reader at the supplied absolute byte offset. */
[[nodiscard]] ReadResult seek(FileReader& reader, std::uint32_t offset);
/** @brief Reports entry type and file size without reading contents. */
[[nodiscard]] ReadResult stat(std::string_view path, EntryType& type, std::uint32_t& size);

using DirectoryVisitor = bool (*)(void* context, const DirectoryEntry& entry);

/** @brief Returns the volume that owns @p path. */
[[nodiscard]] Volume volume(std::string_view path);

/**
 * @brief Mounts the configured filesystem without formatting it.
 * @return The mount outcome.
 */
[[nodiscard]] MountResult mount();
/**
 * @brief Unmounts the filesystem when it is mounted.
 */
void unmount();
/**
 * @brief Reports whether @p target is mounted.
 */
[[nodiscard]] bool is_mounted(Volume target = Volume::flash);
/**
 * @brief Opens the SD card and mounts its FAT volume at `/sd` without formatting it.
 * @return The mount outcome; unavailable in builds without `USE_SDCARD`.
 */
[[nodiscard]] MountResult mount_sd(const platform::SdCardConfig& config);
/**
 * @brief Synchronizes open writers, unmounts the SD card volume, then releases the card.
 */
void unmount_sd();
/** @brief Returns local wall-clock Unix seconds, or 0 while the time is unknown. */
using LocalClock = std::uint32_t (*)(void* context);
/**
 * @brief Supplies the time stamped on SD card files; without it files carry 2026-01-01.
 */
void set_sd_clock(LocalClock clock, void* context);
/**
 * @brief Reads the flash volume's total capacity from its mounted block geometry.
 * @return true when mounted and @p total_bytes is populated; false when
 * unmounted, leaving @p total_bytes unchanged.
 */
[[nodiscard]] bool total_capacity(std::uint32_t& total_bytes);
/**
 * @brief Reads total and used capacity of @p target.
 * @return true when @p info was populated.
 */
[[nodiscard]] bool space(SpaceInfo& info, Volume target = Volume::flash);
/**
 * @brief Visits each direct child of @p path.
 *
 * Listing `/` also visits the `sd` directory while the SD card is mounted.
 * @return The directory traversal outcome.
 */
[[nodiscard]] DirectoryResult list(std::string_view path, DirectoryVisitor visitor, void* context);
/**
 * @brief Reads a complete file into caller-owned bounded storage.
 * @return The read outcome and sets @p size on success.
 */
[[nodiscard]] ReadResult read_file(
    std::string_view path, void* data, std::size_t capacity, std::size_t& size);
/**
 * @brief Opens one file for bounded sequential reads.
 * @return The open outcome; @p reader owns a native handle on success.
 */
[[nodiscard]] ReadResult open_reader(std::string_view path, FileReader& reader);
/**
 * @brief Reads up to @p capacity bytes and reports zero bytes at end of file.
 * @return succeeded for data or end-of-file, otherwise the read failure.
 */
[[nodiscard]] ReadResult read(
    FileReader& reader, void* data, std::size_t capacity, std::size_t& size);
/**
 * @brief Closes a sequential reader and clears its opaque handle.
 * @return true when the handle was valid and closed successfully.
 */
[[nodiscard]] bool close(FileReader& reader);
/**
 * @brief Replaces a file with the supplied bytes.
 * @return The mutation outcome.
 */
[[nodiscard]] MutationResult write_file(std::string_view path, const void* data, std::size_t size);
/**
 * @brief Creates a directory or accepts an existing directory.
 * @return The mutation outcome.
 */
[[nodiscard]] MutationResult ensure_directory(std::string_view path);
/**
 * @brief Removes the file or empty directory at @p path.
 * @return The mutation outcome.
 */
[[nodiscard]] MutationResult remove(std::string_view path);
/**
 * @brief Removes @p path while treating a missing path as success.
 * @return The mutation outcome.
 */
[[nodiscard]] MutationResult remove_if_exists(std::string_view path);
/**
 * @brief Renames or moves one entry within one volume, replacing a destination file.
 *
 * LittleFS replaces atomically; FAT removes the destination file first.
 * @return The mutation outcome; invalid_path when the paths are on different volumes.
 */
[[nodiscard]] MutationResult rename(std::string_view source, std::string_view destination);
/**
 * @brief Returns the most recent native error code of the volume used last.
 *
 * LittleFS errors are negative `LFS_ERR_*`; FAT errors are positive `FRESULT` values.
 */
[[nodiscard]] int last_error();

} // namespace newmota::filesystem
