/*
  berry_port.cpp - Newmota platform hooks for Berry

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/berry_port.h"

#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include "newmota_features.h"
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif

extern "C" {
#include "be_sys.h"
#include "berry.h"
}

#include <array>
#include <cstddef>
#include <cstring>

namespace {

constexpr std::size_t kBerryLineCapacity = 176;
std::array<char, kBerryLineCapacity> berry_line{};
std::size_t berry_line_length = 0;
newmota::BerryFileOpenResult file_open_result = newmota::BerryFileOpenResult::unavailable;
bool exception_report = false, exception_truncated = false;
std::size_t exception_lines = 0;

/** @brief Sends the pending Berry output fragment through Newmota logging. */
void FlushBerryLine() {
#if USE_WEB_CONSOLE
    // One traceback line stays free for the truncation note.
    if (!exception_report || exception_lines + 1 < newmota::console_history::kBerryTraceLines) {
        newmota::console_history::record_berry_output({berry_line.data(), berry_line_length});
        ++exception_lines;
    } else {
        exception_truncated = true;
    }
#endif
    newmota::AddLog(newmota::LogLevel::info, "BRY: %.*s", static_cast<int>(berry_line_length),
        berry_line.data());
    berry_line_length = 0;
}

} // namespace

namespace newmota {

void begin_berry_exception_report() {
    exception_report = true;
    exception_truncated = false;
    exception_lines = 0;
}

void end_berry_exception_report() {
#if USE_WEB_CONSOLE
    if (exception_truncated)
        console_history::record_berry_output("(traceback truncated, full trace in the log)");
#endif
    exception_report = false;
}

/** @brief Returns Berry's last filesystem-open outcome. */
BerryFileOpenResult berry_file_open_result() {
    return file_open_result;
}

} // namespace newmota

/** @brief Routes Berry print output into complete Newmota log lines. */
extern "C" void be_writebuffer(const char* buffer, const size_t length) {
    for (std::size_t index = 0; index < length; ++index) {
        const char character = buffer[index];
        if (character == '\n') {
            FlushBerryLine();
            continue;
        }
        if (character == '\r') {
            continue;
        }
        if (berry_line_length == berry_line.size()) {
            FlushBerryLine();
        }
        berry_line[berry_line_length++] = character;
    }
}

/** @brief Reports that Berry's synchronous input primitive is unavailable. */
extern "C" char* be_readstring(char*, size_t) {
    return nullptr;
}

/** @brief Opens a LittleFS file for Berry's existing streaming parser. */
extern "C" void* be_fopen(const char* const filename, const char* const modes) {
    file_open_result = newmota::BerryFileOpenResult::failed;
    if (modes == nullptr || (std::strcmp(modes, "r") != 0 && std::strcmp(modes, "rb") != 0)) {
        return nullptr;
    }
    if (filename == nullptr || filename[0] == '\0') {
        return nullptr;
    }

    newmota::filesystem::FileReader reader{};
    switch (newmota::filesystem::open_reader(filename, reader)) {
    case newmota::filesystem::ReadResult::succeeded:
        file_open_result = newmota::BerryFileOpenResult::opened;
        return reader.native;
    case newmota::filesystem::ReadResult::not_found:
        file_open_result = newmota::BerryFileOpenResult::not_found;
        return nullptr;
    case newmota::filesystem::ReadResult::unavailable:
        file_open_result = newmota::BerryFileOpenResult::unavailable;
        return nullptr;
    case newmota::filesystem::ReadResult::invalid_path:
    case newmota::filesystem::ReadResult::too_large:
    case newmota::filesystem::ReadResult::failed:
        return nullptr;
    }
    return nullptr;
}

/** @brief Closes a Berry streaming reader and releases its transient cache. */
extern "C" int be_fclose(void* const handle) {
    newmota::filesystem::FileReader reader{handle};
    return newmota::filesystem::close(reader) ? 0 : -1;
}

/** @brief Rejects Berry file writes in the plain-runtime slice. */
extern "C" size_t be_fwrite(void*, const void*, size_t) {
    return 0;
}

/** @brief Reads one parser chunk from a Berry LittleFS handle. */
extern "C" size_t be_fread(void* const handle, void* const buffer, const size_t length) {
    newmota::filesystem::FileReader reader{handle};
    std::size_t read = 0U;
    return newmota::filesystem::read(reader, buffer, length, read) ==
                   newmota::filesystem::ReadResult::succeeded
               ? read
               : 0U;
}

/** @brief Rejects Berry line reads in the initial read-only bridge. */
extern "C" char* be_fgets(void*, void*, int) {
    return nullptr;
}

/** @brief Rejects Berry file seeks in the initial read-only bridge. */
extern "C" int be_fseek(void*, long) {
    return -1;
}

/** @brief Reports no Berry file position in the initial read-only bridge. */
extern "C" long int be_ftell(void*) {
    return -1;
}

/** @brief Rejects Berry file flushing in the initial read-only bridge. */
extern "C" long int be_fflush(void*) {
    return -1;
}

/** @brief Reports zero size without the general Berry file API. */
extern "C" size_t be_fsize(void*) {
    return 0;
}

/** @brief Reports that Berry directory queries are not integrated yet. */
extern "C" int be_isdir(const char*) {
    return 0;
}

/** @brief Reports that Berry file queries are not integrated yet. */
extern "C" int be_isfile(const char*) {
    return 0;
}

/** @brief Reports that Berry path queries are not integrated yet. */
extern "C" int be_isexist(const char*) {
    return 0;
}

/** @brief Rejects Berry current-directory queries in the plain-runtime slice. */
extern "C" char* be_getcwd(char*, size_t) {
    return nullptr;
}

/** @brief Rejects Berry current-directory changes in the plain-runtime slice. */
extern "C" int be_chdir(const char*) {
    return -1;
}

/** @brief Rejects Berry directory creation in the plain-runtime slice. */
extern "C" int be_mkdir(const char*) {
    return -1;
}

/** @brief Rejects Berry file removal in the plain-runtime slice. */
extern "C" int be_unlink(const char*) {
    return -1;
}

/** @brief Rejects Berry directory traversal in the plain-runtime slice. */
extern "C" int be_dirfirst(bdirinfo*, const char*) {
    return -1;
}

/** @brief Rejects Berry directory traversal continuation. */
extern "C" int be_dirnext(bdirinfo*) {
    return -1;
}

/** @brief Accepts closing an unavailable Berry directory iterator. */
extern "C" int be_dirclose(bdirinfo*) {
    return -1;
}
