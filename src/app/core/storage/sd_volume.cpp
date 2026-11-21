/*
  sd_volume.cpp - FatFs volume on the SD card for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/storage/sd_volume.h"

#include "newmota/platform/sd_card.h"
#include "support/text.h"

// ff.h defines the integer types diskio.h uses.
#include "ff.h"

#include "diskio.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace newmota::filesystem::sd {
namespace {

constexpr std::size_t kPathCapacity = 192;

static_assert(FF_MAX_SS == platform::kSdSectorSize && FF_MIN_SS == platform::kSdSectorSize,
    "FatFs must run with the platform SD sector size");
static_assert(sizeof(std::size_t) <= sizeof(UINT), "FatFs transfer sizes must hold size_t");

/** @brief FatFs file state allocated only while a streaming handle is open. */
struct NativeFile final {
    Volume volume{Volume::sd};
    FIL file{};
    NativeFile* next_writer{}; // Open writers, synced before the volume unmounts.
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

/** @brief Hides the entries macOS and Windows leave on removable media, as Tasmota does. */
[[nodiscard]] bool hidden_entry(const std::string_view name) {
    return name.front() == '.' || name == "System Volume Information";
}

/** @brief The opened card; FatFs reaches it through the disk glue below. */
platform::SectorDevice device;

LocalClock wall_clock{};
void* wall_clock_context{};

/**
 * @brief Packs local Unix seconds into the FAT date and time word.
 * @return 0 before 1980 or after 2107, which FAT cannot store.
 */
[[nodiscard]] DWORD fat_time(const std::uint32_t seconds) {
    // Civil date from days since 1970-01-01 (H. Hinnant's days_from_civil inverse).
    const std::int64_t days = seconds / 86400U + 719468;
    const std::int64_t era = days / 146097;
    const auto day_of_era = static_cast<std::uint32_t>(days - era * 146097);
    const std::uint32_t year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const std::uint32_t day_of_year =
        day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const std::uint32_t month_index = (5U * day_of_year + 2U) / 153U;
    const std::uint32_t day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    const std::uint32_t month = month_index < 10U ? month_index + 3U : month_index - 9U;
    const auto year = static_cast<std::uint32_t>(year_of_era + era * 400) + (month <= 2U ? 1U : 0U);
    if (year < 1980U || year > 2107U) {
        return 0;
    }
    const std::uint32_t time_of_day = seconds % 86400U;
    return (year - 1980U) << 25U | month << 21U | day << 16U | (time_of_day / 3600U) << 11U |
           (time_of_day / 60U % 60U) << 5U | (time_of_day % 60U) / 2U;
}

class SdState final {
  public:
    [[nodiscard]] MountResult mount(const platform::SdCardConfig& config) {
        if (filesystem_ != nullptr) {
            return MountResult::already_mounted;
        }
        error_ = FR_OK;
        switch (platform::open_sd_card(config, device)) {
        case platform::SdCardResult::opened:
            break;
        case platform::SdCardResult::unsupported:
            return MountResult::unavailable;
        case platform::SdCardResult::invalid_pins:
            return MountResult::invalid_pins;
        case platform::SdCardResult::no_card:
            return MountResult::no_card;
        case platform::SdCardResult::failed:
            return MountResult::mount_failed;
        }

        filesystem_ = static_cast<FATFS*>(std::calloc(1, sizeof(FATFS)));
        if (filesystem_ == nullptr) {
            error_ = FR_NOT_ENOUGH_CORE;
            device.close();
            return MountResult::mount_failed;
        }
        error_ = f_mount(filesystem_, "", 1);
        if (error_ != FR_OK) {
            const bool no_filesystem = error_ == FR_NO_FILESYSTEM;
            release();
            return no_filesystem ? MountResult::no_filesystem : MountResult::mount_failed;
        }
        return MountResult::mounted;
    }

    /** @brief Unmounts the volume, commits the card and releases it. */
    void unmount() {
        if (filesystem_ == nullptr) {
            return;
        }
        // FatFs does not flush open files on unmount; later closes report the lost volume.
        for (auto* writer = writers_; writer != nullptr; writer = writer->next_writer) {
            const FRESULT synced = f_sync(&writer->file);
            if (synced != FR_OK) {
                error_ = synced;
            }
        }
        const FRESULT result = f_unmount("");
        if (result != FR_OK) {
            error_ = result;
        }
        if (!device.sync() && error_ == FR_OK) {
            error_ = FR_DISK_ERR;
        }
        release();
    }

    [[nodiscard]] bool mounted() const { return filesystem_ != nullptr; }

    [[nodiscard]] bool space(SpaceInfo& info) {
        if (filesystem_ == nullptr) {
            return false;
        }
        DWORD free_clusters = 0;
        FATFS* filesystem = nullptr;
        error_ = f_getfree("", &free_clusters, &filesystem);
        if (error_ != FR_OK) {
            return false;
        }
        const std::uint64_t cluster_bytes =
            static_cast<std::uint64_t>(filesystem->csize) * platform::kSdSectorSize;
        info.total_bytes = static_cast<std::uint64_t>(filesystem->n_fatent - 2U) * cluster_bytes;
        info.used_bytes = info.total_bytes - free_clusters * cluster_bytes;
        return true;
    }

    [[nodiscard]] DirectoryResult list(
        const std::string_view path, const DirectoryVisitor visitor, void* const context) {
        if (filesystem_ == nullptr) {
            return DirectoryResult::unavailable;
        }
        std::array<char, kPathCapacity> path_buffer{};
        if (visitor == nullptr || !copy_path(path, path_buffer)) {
            error_ = FR_INVALID_NAME;
            return DirectoryResult::invalid_path;
        }

        DIR directory{};
        error_ = f_opendir(&directory, path_buffer.data());
        if (error_ != FR_OK) {
            return DirectoryResult::open_failed;
        }

        DirectoryResult result = DirectoryResult::listed;
        FILINFO native_entry{};
        while (true) {
            error_ = f_readdir(&directory, &native_entry);
            if (error_ != FR_OK) {
                result = DirectoryResult::read_failed;
                break;
            }
            const std::string_view name{native_entry.fname};
            if (name.empty()) {
                break;
            }
            if (hidden_entry(name)) {
                continue;
            }
            const bool is_directory = (native_entry.fattrib & AM_DIR) != 0;
            const DirectoryEntry entry{
                name,
                is_directory ? 0U : static_cast<std::uint32_t>(native_entry.fsize),
                is_directory ? EntryType::directory : EntryType::file,
            };
            if (!visitor(context, entry)) {
                result = DirectoryResult::output_stopped;
                break;
            }
        }

        const FRESULT close_error = f_closedir(&directory);
        if (close_error != FR_OK && result == DirectoryResult::listed) {
            error_ = close_error;
            result = DirectoryResult::read_failed;
        }
        return result;
    }

    [[nodiscard]] ReadResult read_file(const std::string_view path, void* const data,
        const std::size_t capacity, std::size_t& size) {
        size = 0;
        FileReader reader{};
        const auto opened = open_reader(path, reader);
        if (opened != ReadResult::succeeded) {
            return opened;
        }
        if (data == nullptr && capacity != 0) {
            error_ = FR_INVALID_PARAMETER;
            (void)close(reader);
            return ReadResult::invalid_path;
        }
        auto* const native = static_cast<NativeFile*>(reader.native);
        const FSIZE_t file_size = f_size(&native->file);
        if (file_size > capacity) {
            error_ = FR_DENIED;
            (void)close(reader);
            return ReadResult::too_large;
        }
        UINT read = 0;
        error_ = f_read(&native->file, data, static_cast<UINT>(file_size), &read);
        if (error_ == FR_OK && read != file_size) {
            error_ = FR_DISK_ERR;
        }
        const FRESULT read_error = error_;
        const bool closed = close(reader);
        if (read_error != FR_OK) {
            error_ = read_error;
            return ReadResult::failed;
        }
        if (!closed) {
            return ReadResult::failed;
        }
        size = static_cast<std::size_t>(file_size);
        return ReadResult::succeeded;
    }

    [[nodiscard]] ReadResult open_reader(const std::string_view path, FileReader& reader) {
        if (filesystem_ == nullptr) {
            return ReadResult::unavailable;
        }
        std::array<char, kPathCapacity> path_buffer{};
        if (reader.native != nullptr || !copy_path(path, path_buffer)) {
            error_ = FR_INVALID_NAME;
            return ReadResult::invalid_path;
        }
        auto* const native = allocate_native_file();
        if (native == nullptr) {
            error_ = FR_NOT_ENOUGH_CORE;
            return ReadResult::failed;
        }
        error_ = f_open(&native->file, path_buffer.data(), FA_READ);
        if (error_ != FR_OK) {
            release_native_file(native);
            return not_found() ? ReadResult::not_found : ReadResult::failed;
        }
        reader.native = native;
        return ReadResult::succeeded;
    }

    [[nodiscard]] ReadResult read(
        FileReader& reader, void* const data, const std::size_t capacity, std::size_t& size) {
        size = 0U;
        auto* const native = static_cast<NativeFile*>(reader.native);
        if (native == nullptr || (data == nullptr && capacity != 0U)) {
            error_ = FR_INVALID_PARAMETER;
            return ReadResult::invalid_path;
        }
        UINT read = 0;
        error_ = f_read(&native->file, data, static_cast<UINT>(capacity), &read);
        if (error_ != FR_OK) {
            return ReadResult::failed;
        }
        size = read;
        return ReadResult::succeeded;
    }

    [[nodiscard]] bool close(FileReader& reader) {
        auto* const native = static_cast<NativeFile*>(reader.native);
        if (native == nullptr) {
            error_ = FR_INVALID_OBJECT;
            return false;
        }
        error_ = f_close(&native->file);
        release_native_file(native);
        reader.native = nullptr;
        return error_ == FR_OK;
    }

    [[nodiscard]] ReadResult seek(FileReader& reader, const std::uint32_t offset) {
        auto* const native = static_cast<NativeFile*>(reader.native);
        if (native == nullptr) {
            return ReadResult::invalid_path;
        }
        error_ = f_lseek(&native->file, offset);
        return error_ == FR_OK ? ReadResult::succeeded : ReadResult::failed;
    }

    [[nodiscard]] ReadResult stat(
        const std::string_view path, EntryType& type, std::uint32_t& size) {
        if (filesystem_ == nullptr) {
            return ReadResult::unavailable;
        }
        if (path == "/") {
            type = EntryType::directory;
            size = 0;
            return ReadResult::succeeded;
        }
        std::array<char, kPathCapacity> buffer{};
        if (!copy_path(path, buffer)) {
            return ReadResult::invalid_path;
        }
        FILINFO info{};
        error_ = f_stat(buffer.data(), &info);
        if (error_ != FR_OK) {
            return not_found() ? ReadResult::not_found : ReadResult::failed;
        }
        const bool is_directory = (info.fattrib & AM_DIR) != 0;
        type = is_directory ? EntryType::directory : EntryType::file;
        size = is_directory ? 0U : static_cast<std::uint32_t>(info.fsize);
        return ReadResult::succeeded;
    }

    [[nodiscard]] MutationResult open_writer(const std::string_view path, FileWriter& writer) {
        if (filesystem_ == nullptr) {
            return MutationResult::unavailable;
        }
        std::array<char, kPathCapacity> buffer{};
        if (writer.native != nullptr || !copy_path(path, buffer)) {
            error_ = FR_INVALID_NAME;
            return MutationResult::invalid_path;
        }
        auto* const native = allocate_native_file();
        if (native == nullptr) {
            error_ = FR_NOT_ENOUGH_CORE;
            return MutationResult::failed;
        }
        error_ = f_open(&native->file, buffer.data(), FA_WRITE | FA_CREATE_ALWAYS);
        if (error_ != FR_OK) {
            release_native_file(native);
            return MutationResult::failed;
        }
        native->next_writer = writers_;
        writers_ = native;
        writer.native = native;
        return MutationResult::succeeded;
    }

    [[nodiscard]] MutationResult write(
        FileWriter& writer, const void* const data, const std::size_t size) {
        auto* const native = static_cast<NativeFile*>(writer.native);
        if (native == nullptr || (data == nullptr && size != 0U)) {
            error_ = FR_INVALID_PARAMETER;
            return MutationResult::invalid_path;
        }
        UINT written = 0;
        error_ = f_write(&native->file, data, static_cast<UINT>(size), &written);
        if (error_ == FR_OK && written != size) {
            error_ = FR_DENIED; // FatFs reports a full volume as a short write.
        }
        return error_ == FR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    [[nodiscard]] bool close(FileWriter& writer) {
        auto* const native = static_cast<NativeFile*>(writer.native);
        if (native == nullptr) {
            return false;
        }
        error_ = f_close(&native->file); // Flushes the file before closing.
        for (auto** link = &writers_; *link != nullptr; link = &(*link)->next_writer) {
            if (*link == native) {
                *link = native->next_writer;
                break;
            }
        }
        release_native_file(native);
        writer.native = nullptr;
        return error_ == FR_OK;
    }

    [[nodiscard]] MutationResult write_file(
        const std::string_view path, const void* const data, const std::size_t size) {
        FileWriter writer{};
        const auto opened = open_writer(path, writer);
        if (opened != MutationResult::succeeded) {
            return opened;
        }
        const auto written = write(writer, data, size);
        const FRESULT write_error = error_;
        const bool closed = close(writer);
        if (written != MutationResult::succeeded) {
            error_ = write_error;
            return written;
        }
        return closed ? MutationResult::succeeded : MutationResult::failed;
    }

    [[nodiscard]] MutationResult ensure_directory(const std::string_view path) {
        if (filesystem_ == nullptr) {
            return MutationResult::unavailable;
        }
        if (path == "/") {
            return MutationResult::succeeded;
        }
        std::array<char, kPathCapacity> buffer{};
        if (!copy_path(path, buffer)) {
            error_ = FR_INVALID_NAME;
            return MutationResult::invalid_path;
        }
        FILINFO info{};
        error_ = f_stat(buffer.data(), &info);
        if (error_ == FR_OK) {
            if ((info.fattrib & AM_DIR) != 0) {
                return MutationResult::succeeded;
            }
            error_ = FR_EXIST;
            return MutationResult::failed;
        }
        if (error_ != FR_NO_FILE) {
            return MutationResult::failed;
        }
        error_ = f_mkdir(buffer.data());
        return error_ == FR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    [[nodiscard]] MutationResult remove(const std::string_view path) {
        if (filesystem_ == nullptr) {
            return MutationResult::unavailable;
        }
        std::array<char, kPathCapacity> buffer{};
        if (path == "/" || !copy_path(path, buffer)) {
            error_ = FR_INVALID_NAME;
            return MutationResult::invalid_path;
        }
        error_ = f_unlink(buffer.data());
        return error_ == FR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    [[nodiscard]] MutationResult remove_if_exists(const std::string_view path) {
        const auto result = remove(path);
        if (result == MutationResult::failed && not_found()) {
            error_ = FR_OK;
            return MutationResult::succeeded;
        }
        return result;
    }

    /** @brief Renames like LittleFS: an existing destination file is replaced. */
    [[nodiscard]] MutationResult rename(
        const std::string_view source, const std::string_view destination) {
        if (filesystem_ == nullptr) {
            return MutationResult::unavailable;
        }
        std::array<char, kPathCapacity> source_buffer{};
        std::array<char, kPathCapacity> destination_buffer{};
        if (source == "/" || destination == "/" || !copy_path(source, source_buffer) ||
            !copy_path(destination, destination_buffer)) {
            error_ = FR_INVALID_NAME;
            return MutationResult::invalid_path;
        }
        // FAT names are case-insensitive: a case-only rename finds the source itself.
        if (!support::ascii_iequals(source, destination)) {
            FILINFO info{};
            error_ = f_stat(destination_buffer.data(), &info);
            if (error_ == FR_OK) {
                if ((info.fattrib & AM_DIR) != 0) {
                    error_ = FR_EXIST;
                    return MutationResult::failed;
                }
                error_ = f_unlink(destination_buffer.data());
                if (error_ != FR_OK) {
                    return MutationResult::failed;
                }
            } else if (error_ != FR_NO_FILE) {
                return MutationResult::failed;
            }
        }
        error_ = f_rename(source_buffer.data(), destination_buffer.data());
        return error_ == FR_OK ? MutationResult::succeeded : MutationResult::failed;
    }

    [[nodiscard]] int error() const { return error_; }

  private:
    [[nodiscard]] bool not_found() const { return error_ == FR_NO_FILE || error_ == FR_NO_PATH; }

    /** @brief Frees the FatFs work area and closes the card. */
    void release() {
        std::free(filesystem_);
        filesystem_ = nullptr;
        device.close();
    }

    FATFS* filesystem_{nullptr};
    NativeFile* writers_{nullptr};
    FRESULT error_{FR_OK};
};

SdState state;

} // namespace

MountResult mount(const platform::SdCardConfig& config) {
    return state.mount(config);
}

void unmount() {
    state.unmount();
}

void set_clock(const LocalClock clock, void* const context) {
    wall_clock = clock;
    wall_clock_context = context;
}

bool mounted() {
    return state.mounted();
}

bool space(SpaceInfo& info) {
    return state.space(info);
}

DirectoryResult list(
    const std::string_view path, const DirectoryVisitor visitor, void* const context) {
    return state.list(path, visitor, context);
}

ReadResult read_file(
    const std::string_view path, void* const data, const std::size_t capacity, std::size_t& size) {
    return state.read_file(path, data, capacity, size);
}

ReadResult open_reader(const std::string_view path, FileReader& reader) {
    return state.open_reader(path, reader);
}

ReadResult read(
    FileReader& reader, void* const data, const std::size_t capacity, std::size_t& size) {
    return state.read(reader, data, capacity, size);
}

bool close(FileReader& reader) {
    return state.close(reader);
}

ReadResult seek(FileReader& reader, const std::uint32_t offset) {
    return state.seek(reader, offset);
}

ReadResult stat(const std::string_view path, EntryType& type, std::uint32_t& size) {
    return state.stat(path, type, size);
}

MutationResult open_writer(const std::string_view path, FileWriter& writer) {
    return state.open_writer(path, writer);
}

MutationResult write(FileWriter& writer, const void* const data, const std::size_t size) {
    return state.write(writer, data, size);
}

bool close(FileWriter& writer) {
    return state.close(writer);
}

MutationResult write_file(
    const std::string_view path, const void* const data, const std::size_t size) {
    return state.write_file(path, data, size);
}

MutationResult ensure_directory(const std::string_view path) {
    return state.ensure_directory(path);
}

MutationResult remove(const std::string_view path) {
    return state.remove(path);
}

MutationResult remove_if_exists(const std::string_view path) {
    return state.remove_if_exists(path);
}

MutationResult rename(const std::string_view source, const std::string_view destination) {
    return state.rename(source, destination);
}

int error() {
    return state.error();
}

} // namespace newmota::filesystem::sd

// FatFs disk glue: the single physical drive 0 is the opened SD card.

/** @brief Stamps files with local time, or FatFs's fixed date while the time is unknown. */
extern "C" DWORD get_fattime() {
    namespace sd = newmota::filesystem::sd;
    const std::uint32_t seconds =
        sd::wall_clock != nullptr ? sd::wall_clock(sd::wall_clock_context) : 0U;
    const DWORD packed = seconds != 0U ? sd::fat_time(seconds) : 0U;
    return packed != 0U ? packed
                        : static_cast<DWORD>(FF_NORTC_YEAR - 1980) << 25U |
                              static_cast<DWORD>(FF_NORTC_MON) << 21U |
                              static_cast<DWORD>(FF_NORTC_MDAY) << 16U;
}

extern "C" DSTATUS disk_status(const BYTE drive) {
    return drive == 0 && newmota::filesystem::sd::device.valid() ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize(const BYTE drive) {
    return disk_status(drive); // open_sd_card() initialized the card before f_mount().
}

extern "C" DRESULT disk_read(
    const BYTE drive, BYTE* const buffer, const LBA_t sector, const UINT count) {
    if (drive != 0) {
        return RES_PARERR;
    }
    return newmota::filesystem::sd::device.read(sector, buffer, count) ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_write(
    const BYTE drive, const BYTE* const buffer, const LBA_t sector, const UINT count) {
    if (drive != 0) {
        return RES_PARERR;
    }
    return newmota::filesystem::sd::device.write(sector, buffer, count) ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_ioctl(const BYTE drive, const BYTE command, void* const buffer) {
    const auto& device = newmota::filesystem::sd::device;
    if (drive != 0 || !device.valid()) {
        return RES_NOTRDY;
    }
    switch (command) {
    case CTRL_SYNC:
        return device.sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *static_cast<LBA_t*>(buffer) = device.sector_count();
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
