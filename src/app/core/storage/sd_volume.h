/*
  sd_volume.h - FAT volume on the SD card behind newmota::filesystem

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/storage/filesystem.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

/**
 * @file sd_volume.h
 * @brief Volume-relative SD calls used by the `/sd` routing in filesystem.cpp.
 *
 * Paths are relative to the card root and start with `/`. Every native reader and writer
 * handle of either volume begins with a @ref filesystem::Volume member, so the router can
 * tell a handle's volume from its pointer alone.
 */
namespace newmota::filesystem::sd {

/** @brief Reads the volume tag that begins every native file handle. */
[[nodiscard]] inline Volume handle_volume(const void* const native) {
    return *static_cast<const Volume*>(native);
}

[[nodiscard]] MountResult mount(const platform::SdCardConfig& config);
void unmount();
void set_clock(LocalClock clock, void* context);
[[nodiscard]] bool mounted();
[[nodiscard]] bool space(SpaceInfo& info);
[[nodiscard]] DirectoryResult list(std::string_view path, DirectoryVisitor visitor, void* context);
[[nodiscard]] ReadResult read_file(
    std::string_view path, void* data, std::size_t capacity, std::size_t& size);
[[nodiscard]] ReadResult open_reader(std::string_view path, FileReader& reader);
[[nodiscard]] ReadResult read(
    FileReader& reader, void* data, std::size_t capacity, std::size_t& size);
[[nodiscard]] bool close(FileReader& reader);
[[nodiscard]] ReadResult seek(FileReader& reader, std::uint32_t offset);
[[nodiscard]] ReadResult stat(std::string_view path, EntryType& type, std::uint32_t& size);
[[nodiscard]] MutationResult open_writer(std::string_view path, FileWriter& writer);
[[nodiscard]] MutationResult write(FileWriter& writer, const void* data, std::size_t size);
[[nodiscard]] bool close(FileWriter& writer);
[[nodiscard]] MutationResult write_file(std::string_view path, const void* data, std::size_t size);
[[nodiscard]] MutationResult ensure_directory(std::string_view path);
[[nodiscard]] MutationResult remove(std::string_view path);
[[nodiscard]] MutationResult remove_if_exists(std::string_view path);
[[nodiscard]] MutationResult rename(std::string_view source, std::string_view destination);
/** @brief Returns the last retained `FRESULT`. */
[[nodiscard]] int error();

} // namespace newmota::filesystem::sd
