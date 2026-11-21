/*
  filesystem.cpp - portable LittleFS implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/filesystem.h"

#include "newmota_features.h"

#ifndef NEWMOTA_FILESYSTEM_ENABLED
#define NEWMOTA_FILESYSTEM_ENABLED 0
#endif

#if NEWMOTA_FILESYSTEM_ENABLED

#include "lfs.h"
#include "newmota/platform/block_device.h"

#if USE_SDCARD
#include "core/storage/sd_volume.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace newmota::filesystem {
namespace {

constexpr std::uint32_t kDiskVersion = 0x00020000;
constexpr std::uint32_t kReadSize = 128;
constexpr std::uint32_t kProgramSize = 128;
constexpr std::uint32_t kCacheSize = 512;
constexpr std::uint32_t kLookaheadSize = 128;
constexpr std::int32_t kBlockCycles = 512;
constexpr std::size_t kPathCapacity = 192;

static_assert(LFS_VERSION == 0x0002000b, "Newmota requires LittleFS 2.11.x");

/**
 * @brief Copies a validated string view into a null-terminated path buffer.
 * @return false for empty, oversized, or embedded-null paths.
 */
[[nodiscard]] bool copy_path(const std::string_view path, std::array<char, kPathCapacity>& buffer) {
    if (path.empty() || path.size() >= buffer.size() || path.find('\0') != std::string_view::npos) {
        return false;
    }
    for (std::size_t index = 0; index < path.size(); ++index) {
        buffer[index] = path[index];
    }
    buffer[path.size()] = '\0';
    return true;
}

/**
 * @brief Validates a LittleFS block range and converts it to a byte address.
 * @return true when @p address was populated.
 */
[[nodiscard]] bool block_address(const lfs_config* const config, const lfs_block_t block,
    const lfs_off_t offset, const lfs_size_t size, std::uint32_t& address) {
    const auto* const device = static_cast<const platform::BlockDevice*>(config->context);
    if (device == nullptr || block >= device->size() / device->erase_size() ||
        offset > device->erase_size() || size > device->erase_size() - offset) {
        return false;
    }
    address = block * device->erase_size() + offset;
    return true;
}

/** @brief Native LittleFS state allocated only while a streaming handle is open. */
struct NativeFile final {
    Volume volume{Volume::flash}; // Begins every handle; see sd_volume.h.
    lfs_file_t file{};
    lfs_file_config config{};
    alignas(std::uint32_t) std::array<std::byte, kCacheSize> buffer{};
};

static_assert(std::is_standard_layout_v<NativeFile> && offsetof(NativeFile, volume) == 0,
    "the volume tag must begin the handle");

/** @brief Allocates file state without depending on throwing operator new. */
[[nodiscard]] NativeFile* allocate_native_file() {
    void* const storage = std::malloc(sizeof(NativeFile));
    return storage != nullptr ? new (storage) NativeFile{} : nullptr;
}

/** @brief Destroys file state and returns its storage to the platform heap. */
void release_native_file(NativeFile* const native) {
    native->~NativeFile();
    std::free(native);
}

/**
 * @brief Adapts a LittleFS read request to the platform block device.
 */
int read_block(const lfs_config* const config, const lfs_block_t block, const lfs_off_t offset,
    void* const buffer, const lfs_size_t size) {
    std::uint32_t address = 0;
    if (!block_address(config, block, offset, size, address)) {
        return LFS_ERR_IO;
    }
    const auto& device = *static_cast<const platform::BlockDevice*>(config->context);
    return device.read(address, buffer, size) ? LFS_ERR_OK : LFS_ERR_IO;
}

/**
 * @brief Adapts a LittleFS program request to the platform block device.
 */
int program_block(const lfs_config* const config, const lfs_block_t block, const lfs_off_t offset,
    const void* const buffer, const lfs_size_t size) {
    std::uint32_t address = 0;
    if (!block_address(config, block, offset, size, address)) {
        return LFS_ERR_IO;
    }
    const auto& device = *static_cast<const platform::BlockDevice*>(config->context);
    return device.program(address, buffer, size) ? LFS_ERR_OK : LFS_ERR_IO;
}

/**
 * @brief Adapts a LittleFS erase request to the platform block device.
 */
int erase_block(const lfs_config* const config, const lfs_block_t block) {
    std::uint32_t address = 0;
    if (!block_address(config, block, 0, config->block_size, address)) {
        return LFS_ERR_IO;
    }
    const auto& device = *static_cast<const platform::BlockDevice*>(config->context);
    return device.erase(address) ? LFS_ERR_OK : LFS_ERR_IO;
}

/**
 * @brief Adapts a LittleFS sync request to the platform block device.
 */
int sync_device(const lfs_config* const config) {
    const auto* const device = static_cast<const platform::BlockDevice*>(config->context);
    return device != nullptr && device->sync() ? LFS_ERR_OK : LFS_ERR_IO;
}

class FilesystemState final {
  public:
    /**
     * @brief Opens, mounts, validates, and grows the configured LittleFS volume.
     */
    [[nodiscard]] MountResult mount() {
        if (mounted_) {
            return MountResult::already_mounted;
        }

        device_ = {};
        error_ = LFS_ERR_OK;
        if (!platform::open_filesystem_block_device(device_)) {
            return MountResult::unavailable;
        }
        if (!valid_geometry()) {
            return MountResult::invalid_device;
        }

        config_ = {};
        config_.context = &device_;
        config_.read = read_block;
        config_.prog = program_block;
        config_.erase = erase_block;
        config_.sync = sync_device;
        config_.read_size = kReadSize;
        config_.prog_size = kProgramSize;
        config_.block_size = device_.erase_size();
        config_.block_count = 0;
        config_.block_cycles = kBlockCycles;
        config_.cache_size = kCacheSize;
        config_.lookahead_size = kLookaheadSize;
        config_.read_buffer = read_buffer_.data();
        config_.prog_buffer = program_buffer_.data();
        config_.lookahead_buffer = lookahead_buffer_.data();
        config_.disk_version = kDiskVersion;

        error_ = lfs_mount(&filesystem_, &config_);
        if (error_ != LFS_ERR_OK) {
            return MountResult::mount_failed;
        }
        mounted_ = true;

        lfs_fsinfo filesystem_info{};
        error_ = lfs_fs_stat(&filesystem_, &filesystem_info);
        if (error_ != LFS_ERR_OK) {
            unmount();
            return MountResult::mount_failed;
        }

        const auto device_blocks = device_.size() / device_.erase_size();
        if (filesystem_info.block_count > device_blocks) {
            error_ = LFS_ERR_INVAL;
            unmount();
            return MountResult::invalid_device;
        }
        if (filesystem_info.block_count < device_blocks) {
            error_ = lfs_fs_grow(&filesystem_, device_blocks);
            if (error_ != LFS_ERR_OK) {
                unmount();
                return MountResult::grow_failed;
            }
        }
        return MountResult::mounted;
    }

    /**
     * @brief Unmounts the volume and retains any native unmount error.
     */
    void unmount() {
        if (!mounted_) {
            return;
        }
        const int result = lfs_unmount(&filesystem_);
        mounted_ = false;
        if (result != LFS_ERR_OK) {
            error_ = result;
        }
    }

    /** @brief Reports whether the LittleFS volume is currently mounted. */
    [[nodiscard]] bool mounted() const { return mounted_; }

    /**
     * @brief Reads total capacity without traversing filesystem entries.
     * @return true when mounted and @p total_bytes is populated; false when
     * unmounted, leaving @p total_bytes unchanged.
     */
    [[nodiscard]] bool total_capacity(std::uint32_t& total_bytes) const {
        if (!mounted_) {
            return false;
        }
        total_bytes = device_.size(); // mount() grows LittleFS to this region.
        return true;
    }

    /**
     * @brief Reads total and allocated filesystem capacity.
     * @return true when @p info was populated.
     */
    [[nodiscard]] bool space(SpaceInfo& info) {
        if (!mounted_) {
            return false;
        }
        lfs_fsinfo filesystem_info{};
        error_ = lfs_fs_stat(&filesystem_, &filesystem_info);
        if (error_ != LFS_ERR_OK) {
            return false;
        }
        const lfs_ssize_t used_blocks = lfs_fs_size(&filesystem_);
        if (used_blocks < 0) {
            error_ = static_cast<int>(used_blocks);
            return false;
        }
        info.total_bytes =
            static_cast<std::uint64_t>(filesystem_info.block_count) * filesystem_info.block_size;
        info.used_bytes = static_cast<std::uint64_t>(used_blocks) * filesystem_info.block_size;
        return true;
    }

    /**
     * @brief Visits direct directory entries while filtering dot entries.
     * @return The traversal outcome.
     */
    [[nodiscard]] DirectoryResult list(
        const std::string_view path, const DirectoryVisitor visitor, void* const context) {
        if (!mounted_) {
            return DirectoryResult::unavailable;
        }
        std::array<char, kPathCapacity> path_buffer{};
        if (visitor == nullptr || !copy_path(path, path_buffer)) {
            error_ = LFS_ERR_INVAL;
            return DirectoryResult::invalid_path;
        }

        lfs_dir_t directory{};
        error_ = lfs_dir_open(&filesystem_, &directory, path_buffer.data());
        if (error_ != LFS_ERR_OK) {
            return DirectoryResult::open_failed;
        }

        DirectoryResult result = DirectoryResult::listed;
        lfs_info native_entry{};
        while (true) {
            error_ = lfs_dir_read(&filesystem_, &directory, &native_entry);
            if (error_ < 0) {
                result = DirectoryResult::read_failed;
                break;
            }
            if (error_ == 0) {
                error_ = LFS_ERR_OK;
                break;
            }
            const std::string_view name{native_entry.name};
            if (name == "." || name == "..") {
                continue;
            }
            const DirectoryEntry entry{
                name,
                native_entry.type == LFS_TYPE_REG ? native_entry.size : 0,
                native_entry.type == LFS_TYPE_DIR ? EntryType::directory : EntryType::file,
            };
            if (!visitor(context, entry)) {
                result = DirectoryResult::output_stopped;
                break;
            }
        }

        const int close_error = lfs_dir_close(&filesystem_, &directory);
        if (close_error != LFS_ERR_OK && result == DirectoryResult::listed) {
            error_ = close_error;
            result = DirectoryResult::read_failed;
        }
        return result;
    }

    /**
     * @brief Reads an entire file into bounded caller storage.
     * @return The read outcome.
     */
    [[nodiscard]] ReadResult read_file(const std::string_view path, void* const data,
        const std::size_t capacity, std::size_t& size) {
        size = 0;
        if (!mounted_) {
            return ReadResult::unavailable;
        }
        std::array<char, kPathCapacity> path_buffer{};
        if (!copy_path(path, path_buffer) || (data == nullptr && capacity != 0)) {
            error_ = LFS_ERR_INVAL;
            return ReadResult::invalid_path;
        }

        alignas(std::uint32_t) std::array<std::byte, kCacheSize> file_buffer{};
        lfs_file_config file_config{};
        file_config.buffer = file_buffer.data();
        lfs_file_t file{};
        error_ =
            lfs_file_opencfg(&filesystem_, &file, path_buffer.data(), LFS_O_RDONLY, &file_config);
        if (error_ == LFS_ERR_NOENT) {
            return ReadResult::not_found;
        }
        if (error_ != LFS_ERR_OK) {
            return ReadResult::failed;
        }

        const lfs_soff_t file_size = lfs_file_size(&filesystem_, &file);
        if (file_size < 0) {
            error_ = static_cast<int>(file_size);
            (void)lfs_file_close(&filesystem_, &file);
            return ReadResult::failed;
        }
        if (static_cast<std::size_t>(file_size) > capacity) {
            error_ = LFS_ERR_FBIG;
            (void)lfs_file_close(&filesystem_, &file);
            return ReadResult::too_large;
        }

        const lfs_ssize_t read = lfs_file_read(&filesystem_, &file, data, file_size);
        if (read != file_size) {
            error_ = read < 0 ? static_cast<int>(read) : LFS_ERR_IO;
            (void)lfs_file_close(&filesystem_, &file);
            return ReadResult::failed;
        }
        error_ = lfs_file_close(&filesystem_, &file);
        if (error_ != LFS_ERR_OK) {
            return ReadResult::failed;
        }
        size = static_cast<std::size_t>(file_size);
        return ReadResult::succeeded;
    }

    /**
     * @brief Opens one dynamically owned LittleFS reader without buffering the file.
     * @return The open outcome and a live opaque handle on success.
     */
    [[nodiscard]] ReadResult open_reader(const std::string_view path, FileReader& reader) {
        if (!mounted_) {
            return ReadResult::unavailable;
        }
        if (reader.native != nullptr) {
            error_ = LFS_ERR_INVAL;
            return ReadResult::invalid_path;
        }
        std::array<char, kPathCapacity> path_buffer{};
        if (!copy_path(path, path_buffer)) {
            error_ = LFS_ERR_INVAL;
            return ReadResult::invalid_path;
        }

        auto* const native = allocate_native_file();
        if (native == nullptr) {
            error_ = LFS_ERR_NOMEM;
            return ReadResult::failed;
        }
        native->config.buffer = native->buffer.data();
        error_ = lfs_file_opencfg(
            &filesystem_, &native->file, path_buffer.data(), LFS_O_RDONLY, &native->config);
        if (error_ != LFS_ERR_OK) {
            release_native_file(native);
            return error_ == LFS_ERR_NOENT ? ReadResult::not_found : ReadResult::failed;
        }
        reader.native = native;
        return ReadResult::succeeded;
    }

    /** @brief Reads the next block from one live LittleFS reader. */
    [[nodiscard]] ReadResult read(
        FileReader& reader, void* const data, const std::size_t capacity, std::size_t& size) {
        size = 0U;
        auto* const native = static_cast<NativeFile*>(reader.native);
        if (native == nullptr || (data == nullptr && capacity != 0U) ||
            capacity > static_cast<std::size_t>(LFS_FILE_MAX)) {
            error_ = LFS_ERR_INVAL;
            return ReadResult::invalid_path;
        }
        const auto result =
            lfs_file_read(&filesystem_, &native->file, data, static_cast<lfs_size_t>(capacity));
        if (result < 0) {
            error_ = static_cast<int>(result);
            return ReadResult::failed;
        }
        size = static_cast<std::size_t>(result);
        return ReadResult::succeeded;
    }

    /** @brief Closes and releases one dynamically owned LittleFS reader. */
    [[nodiscard]] bool close(FileReader& reader) {
        auto* const native = static_cast<NativeFile*>(reader.native);
        if (native == nullptr) {
            error_ = LFS_ERR_INVAL;
            return false;
        }
        error_ = lfs_file_close(&filesystem_, &native->file);
        release_native_file(native);
        reader.native = nullptr;
        return error_ == LFS_ERR_OK;
    }

    [[nodiscard]] ReadResult stat(std::string_view path, EntryType& type, std::uint32_t& size) {
        if (!mounted_)
            return ReadResult::unavailable;
        std::array<char, kPathCapacity> buffer{};
        if (!copy_path(path, buffer))
            return ReadResult::invalid_path;
        lfs_info info{};
        error_ = lfs_stat(&filesystem_, buffer.data(), &info);
        if (error_ != LFS_ERR_OK)
            return error_ == LFS_ERR_NOENT ? ReadResult::not_found : ReadResult::failed;
        type = info.type == LFS_TYPE_DIR ? EntryType::directory : EntryType::file;
        size = info.size;
        return ReadResult::succeeded;
    }

    [[nodiscard]] ReadResult seek(FileReader& reader, std::uint32_t offset) {
        auto* native = static_cast<NativeFile*>(reader.native);
        if (!native || offset > LFS_FILE_MAX)
            return ReadResult::invalid_path;
        const auto result = lfs_file_seek(&filesystem_, &native->file, offset, LFS_SEEK_SET);
        if (result < 0) {
            error_ = result;
            return ReadResult::failed;
        }
        return ReadResult::succeeded;
    }

    [[nodiscard]] MutationResult open_writer(std::string_view path, FileWriter& writer) {
        if (!mounted_)
            return MutationResult::unavailable;
        std::array<char, kPathCapacity> buffer{};
        if (writer.native || !copy_path(path, buffer))
            return MutationResult::invalid_path;
        auto* native = allocate_native_file();
        if (!native) {
            error_ = LFS_ERR_NOMEM;
            return MutationResult::failed;
        }
        native->config.buffer = native->buffer.data();
        error_ = lfs_file_opencfg(&filesystem_, &native->file, buffer.data(),
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &native->config);
        if (error_ != LFS_ERR_OK) {
            release_native_file(native);
            return MutationResult::failed;
        }
        writer.native = native;
        return MutationResult::succeeded;
    }

    [[nodiscard]] MutationResult write(FileWriter& writer, const void* data, std::size_t size) {
        auto* native = static_cast<NativeFile*>(writer.native);
        if (!native || (!data && size) || size > LFS_FILE_MAX)
            return MutationResult::invalid_path;
        const auto result =
            lfs_file_write(&filesystem_, &native->file, data, static_cast<lfs_size_t>(size));
        if (result != static_cast<lfs_ssize_t>(size)) {
            error_ = result < 0 ? static_cast<int>(result) : LFS_ERR_IO;
            return MutationResult::failed;
        }
        return MutationResult::succeeded;
    }

    [[nodiscard]] bool close(FileWriter& writer) {
        auto* native = static_cast<NativeFile*>(writer.native);
        if (!native)
            return false;
        const auto sync = lfs_file_sync(&filesystem_, &native->file);
        const auto closed = lfs_file_close(&filesystem_, &native->file);
        error_ = sync != LFS_ERR_OK ? sync : closed;
        release_native_file(native);
        writer.native = nullptr;
        return error_ == LFS_ERR_OK;
    }

    /**
     * @brief Creates or replaces a file and synchronizes it before success.
     * @return The mutation outcome.
     */
    [[nodiscard]] MutationResult write_file(
        const std::string_view path, const void* const data, const std::size_t size) {
        if (!mounted_) {
            return MutationResult::unavailable;
        }

        std::array<char, kPathCapacity> path_buffer{};
        if (!copy_path(path, path_buffer) || (data == nullptr && size != 0) ||
            size > static_cast<std::size_t>(LFS_FILE_MAX)) {
            error_ = LFS_ERR_INVAL;
            return MutationResult::invalid_path;
        }

        alignas(std::uint32_t) std::array<std::byte, kCacheSize> file_buffer{};
        lfs_file_config file_config{};
        file_config.buffer = file_buffer.data();
        lfs_file_t file{};
        error_ = lfs_file_opencfg(&filesystem_, &file, path_buffer.data(),
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &file_config);
        if (error_ != LFS_ERR_OK) {
            return MutationResult::failed;
        }

        const lfs_ssize_t written = lfs_file_write(&filesystem_, &file, data, size);
        if (written != static_cast<lfs_ssize_t>(size)) {
            error_ = written < 0 ? static_cast<int>(written) : LFS_ERR_IO;
            (void)lfs_file_close(&filesystem_, &file);
            return MutationResult::failed;
        }
        error_ = lfs_file_sync(&filesystem_, &file);
        if (error_ != LFS_ERR_OK) {
            (void)lfs_file_close(&filesystem_, &file);
            return MutationResult::failed;
        }
        error_ = lfs_file_close(&filesystem_, &file);
        return error_ == LFS_ERR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    /**
     * @brief Creates a directory or accepts an existing directory.
     * @return The mutation outcome.
     */
    [[nodiscard]] MutationResult ensure_directory(const std::string_view path) {
        if (!mounted_) {
            return MutationResult::unavailable;
        }

        std::array<char, kPathCapacity> path_buffer{};
        if (!copy_path(path, path_buffer)) {
            error_ = LFS_ERR_INVAL;
            return MutationResult::invalid_path;
        }

        lfs_info info{};
        error_ = lfs_stat(&filesystem_, path_buffer.data(), &info);
        if (error_ == LFS_ERR_OK) {
            if (info.type == LFS_TYPE_DIR) {
                return MutationResult::succeeded;
            }
            error_ = LFS_ERR_EXIST;
            return MutationResult::failed;
        }
        if (error_ != LFS_ERR_NOENT) {
            return MutationResult::failed;
        }
        error_ = lfs_mkdir(&filesystem_, path_buffer.data());
        return error_ == LFS_ERR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    /**
     * @brief Removes one file or empty directory.
     * @return The mutation outcome.
     */
    [[nodiscard]] MutationResult remove(const std::string_view path) {
        if (!mounted_) {
            return MutationResult::unavailable;
        }

        std::array<char, kPathCapacity> path_buffer{};
        if (!copy_path(path, path_buffer)) {
            error_ = LFS_ERR_INVAL;
            return MutationResult::invalid_path;
        }
        error_ = lfs_remove(&filesystem_, path_buffer.data());
        return error_ == LFS_ERR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    /**
     * @brief Removes a path while accepting an already absent entry.
     * @return The mutation outcome.
     */
    [[nodiscard]] MutationResult remove_if_exists(const std::string_view path) {
        const auto result = remove(path);
        if (result == MutationResult::failed && error_ == LFS_ERR_NOENT) {
            error_ = LFS_ERR_OK;
            return MutationResult::succeeded;
        }
        return result;
    }

    /**
     * @brief Renames or moves one filesystem entry.
     * @return The mutation outcome.
     */
    [[nodiscard]] MutationResult rename(
        const std::string_view source, const std::string_view destination) {
        if (!mounted_) {
            return MutationResult::unavailable;
        }

        std::array<char, kPathCapacity> source_buffer{};
        std::array<char, kPathCapacity> destination_buffer{};
        if (!copy_path(source, source_buffer) || !copy_path(destination, destination_buffer)) {
            error_ = LFS_ERR_INVAL;
            return MutationResult::invalid_path;
        }
        error_ = lfs_rename(&filesystem_, source_buffer.data(), destination_buffer.data());
        return error_ == LFS_ERR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    /** @brief Returns the last retained LittleFS error code. */
    [[nodiscard]] int error() const { return error_; }

  private:
    /** @brief Validates device geometry against the fixed LittleFS cache geometry. */
    [[nodiscard]] bool valid_geometry() const {
        return device_.valid() && device_.read_size() <= kReadSize &&
               kReadSize % device_.read_size() == 0 && device_.program_size() <= kProgramSize &&
               kProgramSize % device_.program_size() == 0 && device_.erase_size() % kCacheSize == 0;
    }

    platform::BlockDevice device_{};
    lfs_t filesystem_{};
    lfs_config config_{};
    alignas(std::uint32_t) std::array<std::byte, kCacheSize> read_buffer_{};
    alignas(std::uint32_t) std::array<std::byte, kCacheSize> program_buffer_{};
    alignas(std::uint32_t) std::array<std::byte, kLookaheadSize> lookahead_buffer_{};
    bool mounted_{false};
    int error_{LFS_ERR_OK};
};

FilesystemState state;

#if USE_SDCARD
/**
 * @brief Splits an `/sd` path into its card-relative path.
 * @return true when @p path belongs to the SD card volume.
 */
[[nodiscard]] bool sd_path(const std::string_view path, std::string_view& relative) {
    if (path == "/sd") {
        relative = "/";
        return true;
    }
    if (path.size() > 3 && path.substr(0, 4) == "/sd/") {
        relative = path.substr(3);
        return true;
    }
    return false;
}

/** @brief Volume of the latest routed call, which owns last_error(). */
Volume last_volume = Volume::flash;

/** @brief Adds the mounted card as `sd` to a flash root listing and hides the shadowed entry. */
struct RootListing final {
    DirectoryVisitor visitor;
    void* context;
};

bool visit_flash_root_entry(void* const context, const DirectoryEntry& entry) {
    const auto& listing = *static_cast<const RootListing*>(context);
    return entry.name == "sd" || listing.visitor(listing.context, entry);
}
#endif

} // namespace

/** @brief Returns the volume that owns @p path. */
Volume volume(const std::string_view path) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        return Volume::sd;
    }
#else
    (void)path;
#endif
    return Volume::flash;
}

/** @brief Mounts the process-wide LittleFS state. */
MountResult mount() {
    return state.mount();
}

/** @brief Unmounts the process-wide LittleFS state. */
void unmount() {
    state.unmount();
}

/** @brief Reports whether @p target is mounted. */
bool is_mounted(const Volume target) {
#if USE_SDCARD
    if (target == Volume::sd) {
        return sd::mounted();
    }
#endif
    return target == Volume::flash && state.mounted();
}

/** @brief Mounts the SD card volume at `/sd`. */
MountResult mount_sd(const platform::SdCardConfig& config) {
#if USE_SDCARD
    last_volume = Volume::sd;
    return sd::mount(config);
#else
    (void)config;
    return MountResult::unavailable;
#endif
}

/** @brief Unmounts the SD card volume. */
void unmount_sd() {
#if USE_SDCARD
    sd::unmount();
#endif
}

/** @brief Installs the wall clock for SD card file times. */
void set_sd_clock(const LocalClock clock, void* const context) {
#if USE_SDCARD
    sd::set_clock(clock, context);
#else
    (void)clock;
    (void)context;
#endif
}

/** @brief Reads total capacity from the process-wide filesystem state. */
bool total_capacity(std::uint32_t& total_bytes) {
    return state.total_capacity(total_bytes);
}

/** @brief Reads capacity of one volume. */
bool space(SpaceInfo& info, const Volume target) {
#if USE_SDCARD
    if (target == Volume::sd) {
        last_volume = Volume::sd;
        return sd::space(info);
    }
    last_volume = Volume::flash;
#endif
    return target == Volume::flash && state.space(info);
}

/** @brief Lists a directory on the volume that owns @p path. */
DirectoryResult list(
    const std::string_view path, const DirectoryVisitor visitor, void* const context) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::list(relative, visitor, context);
    }
    last_volume = Volume::flash;
    if (path == "/" && visitor != nullptr && sd::mounted()) {
        if (!visitor(context, DirectoryEntry{"sd", 0, EntryType::directory})) {
            return DirectoryResult::output_stopped;
        }
        RootListing listing{visitor, context};
        return state.list(path, visit_flash_root_entry, &listing);
    }
#endif
    return state.list(path, visitor, context);
}

/** @brief Reads a file from the volume that owns @p path. */
ReadResult read_file(
    const std::string_view path, void* const data, const std::size_t capacity, std::size_t& size) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::read_file(relative, data, capacity, size);
    }
    last_volume = Volume::flash;
#endif
    return state.read_file(path, data, capacity, size);
}

/** @brief Opens a streaming reader on the volume that owns @p path. */
ReadResult open_reader(const std::string_view path, FileReader& reader) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::open_reader(relative, reader);
    }
    last_volume = Volume::flash;
#endif
    return state.open_reader(path, reader);
}

/** @brief Reads from a streaming reader on its own volume. */
ReadResult read(
    FileReader& reader, void* const data, const std::size_t capacity, std::size_t& size) {
#if USE_SDCARD
    if (reader.native != nullptr && sd::handle_volume(reader.native) == Volume::sd) {
        last_volume = Volume::sd;
        return sd::read(reader, data, capacity, size);
    }
    last_volume = Volume::flash;
#endif
    return state.read(reader, data, capacity, size);
}

/** @brief Closes a streaming reader on its own volume. */
bool close(FileReader& reader) {
#if USE_SDCARD
    if (reader.native != nullptr && sd::handle_volume(reader.native) == Volume::sd) {
        last_volume = Volume::sd;
        return sd::close(reader);
    }
    last_volume = Volume::flash;
#endif
    return state.close(reader);
}

/** @brief Opens a writer on the volume that owns @p path. */
MutationResult open_writer(const std::string_view path, FileWriter& writer) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::open_writer(relative, writer);
    }
    last_volume = Volume::flash;
#endif
    return state.open_writer(path, writer);
}

/** @brief Writes through a writer on its own volume. */
MutationResult write(FileWriter& writer, const void* const data, const std::size_t size) {
#if USE_SDCARD
    if (writer.native != nullptr && sd::handle_volume(writer.native) == Volume::sd) {
        last_volume = Volume::sd;
        return sd::write(writer, data, size);
    }
    last_volume = Volume::flash;
#endif
    return state.write(writer, data, size);
}

/** @brief Closes a writer on its own volume. */
bool close(FileWriter& writer) {
#if USE_SDCARD
    if (writer.native != nullptr && sd::handle_volume(writer.native) == Volume::sd) {
        last_volume = Volume::sd;
        return sd::close(writer);
    }
    last_volume = Volume::flash;
#endif
    return state.close(writer);
}

/** @brief Positions a streaming reader on its own volume. */
ReadResult seek(FileReader& reader, const std::uint32_t offset) {
#if USE_SDCARD
    if (reader.native != nullptr && sd::handle_volume(reader.native) == Volume::sd) {
        last_volume = Volume::sd;
        return sd::seek(reader, offset);
    }
    last_volume = Volume::flash;
#endif
    return state.seek(reader, offset);
}

/** @brief Reports one entry on the volume that owns @p path. */
ReadResult stat(const std::string_view path, EntryType& type, std::uint32_t& size) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::stat(relative, type, size);
    }
    last_volume = Volume::flash;
#endif
    return state.stat(path, type, size);
}

/** @brief Writes a file on the volume that owns @p path. */
MutationResult write_file(
    const std::string_view path, const void* const data, const std::size_t size) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::write_file(relative, data, size);
    }
    last_volume = Volume::flash;
#endif
    return state.write_file(path, data, size);
}

/** @brief Ensures a directory on the volume that owns @p path. */
MutationResult ensure_directory(const std::string_view path) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::ensure_directory(relative);
    }
    last_volume = Volume::flash;
#endif
    return state.ensure_directory(path);
}

/** @brief Removes a path on the volume that owns it. */
MutationResult remove(const std::string_view path) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::remove(relative);
    }
    last_volume = Volume::flash;
#endif
    return state.remove(path);
}

/** @brief Removes an optional path on the volume that owns it. */
MutationResult remove_if_exists(const std::string_view path) {
#if USE_SDCARD
    std::string_view relative;
    if (sd_path(path, relative)) {
        last_volume = Volume::sd;
        return sd::remove_if_exists(relative);
    }
    last_volume = Volume::flash;
#endif
    return state.remove_if_exists(path);
}

/** @brief Renames a path within the volume that owns both paths. */
MutationResult rename(const std::string_view source, const std::string_view destination) {
#if USE_SDCARD
    std::string_view source_relative;
    std::string_view destination_relative;
    const bool source_sd = sd_path(source, source_relative);
    if (source_sd != sd_path(destination, destination_relative)) {
        return MutationResult::invalid_path;
    }
    if (source_sd) {
        last_volume = Volume::sd;
        return sd::rename(source_relative, destination_relative);
    }
    last_volume = Volume::flash;
#endif
    return state.rename(source, destination);
}

/** @brief Returns the retained native error code of the volume used last. */
int last_error() {
#if USE_SDCARD
    if (last_volume == Volume::sd) {
        return sd::error();
    }
#endif
    return state.error();
}

} // namespace newmota::filesystem

#else

namespace newmota::filesystem {

/** @brief Reports unavailable when filesystem support is excluded. */
MountResult mount() {
    return MountResult::unavailable;
}

/** @brief Performs no work when filesystem support is excluded. */
void unmount() {}

/** @brief Reports the flash volume for every path when filesystem support is excluded. */
Volume volume(std::string_view) {
    return Volume::flash;
}

/** @brief Reports unmounted when filesystem support is excluded. */
bool is_mounted(Volume) {
    return false;
}

/** @brief Reports unavailable when filesystem support is excluded. */
MountResult mount_sd(const platform::SdCardConfig&) {
    return MountResult::unavailable;
}

/** @brief Performs no work when filesystem support is excluded. */
void unmount_sd() {}

/** @brief Performs no work when filesystem support is excluded. */
void set_sd_clock(LocalClock, void*) {}

/** @brief Reports no capacity when filesystem support is excluded. */
bool total_capacity(std::uint32_t&) {
    return false;
}

/** @brief Reports no capacity when filesystem support is excluded. */
bool space(SpaceInfo&, Volume) {
    return false;
}

/** @brief Reports unavailable when directory support is excluded. */
DirectoryResult list(std::string_view, DirectoryVisitor, void*) {
    return DirectoryResult::unavailable;
}

/** @brief Reports unavailable and clears size when filesystem support is excluded. */
ReadResult read_file(std::string_view, void*, std::size_t, std::size_t& size) {
    size = 0;
    return ReadResult::unavailable;
}

/** @brief Reports unavailable when streaming filesystem reads are excluded. */
ReadResult open_reader(std::string_view, FileReader& reader) {
    reader.native = nullptr;
    return ReadResult::unavailable;
}

/** @brief Reports unavailable when streaming filesystem reads are excluded. */
ReadResult read(FileReader&, void*, std::size_t, std::size_t& size) {
    size = 0U;
    return ReadResult::unavailable;
}

/** @brief Clears an unavailable streaming-reader handle. */
bool close(FileReader& reader) {
    reader.native = nullptr;
    return false;
}

MutationResult open_writer(std::string_view, FileWriter&) {
    return MutationResult::unavailable;
}

MutationResult write(FileWriter&, const void*, std::size_t) {
    return MutationResult::unavailable;
}

bool close(FileWriter& writer) {
    writer.native = nullptr;
    return false;
}

ReadResult seek(FileReader&, std::uint32_t) {
    return ReadResult::unavailable;
}

ReadResult stat(std::string_view, EntryType&, std::uint32_t&) {
    return ReadResult::unavailable;
}

/** @brief Reports unavailable when file writes are excluded. */
MutationResult write_file(std::string_view, const void*, std::size_t) {
    return MutationResult::unavailable;
}

/** @brief Reports unavailable when directory creation is excluded. */
MutationResult ensure_directory(std::string_view) {
    return MutationResult::unavailable;
}

/** @brief Reports unavailable when removal support is excluded. */
MutationResult remove(std::string_view) {
    return MutationResult::unavailable;
}

/** @brief Reports unavailable when optional removal support is excluded. */
MutationResult remove_if_exists(std::string_view) {
    return MutationResult::unavailable;
}

/** @brief Reports unavailable when rename support is excluded. */
MutationResult rename(std::string_view, std::string_view) {
    return MutationResult::unavailable;
}

/** @brief Returns no native error when filesystem support is excluded. */
int last_error() {
    return 0;
}

} // namespace newmota::filesystem

#endif
