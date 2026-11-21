/*
  result_timestamp.h - console command result timestamps for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace newmota {

#if USE_TASMOTA_STATE
/** @brief Starts every line a console sink receives with the log timestamp. */
class TimestampedOutput final {
  public:
    TimestampedOutput(const CommandOutput::Writer sink, void* const context)
        : sink_(sink), context_(context) {}

    /** @brief CommandOutput writer whose context is a TimestampedOutput. */
    static bool write(void* const context, std::string_view text) {
        auto& self = *static_cast<TimestampedOutput*>(context);
        while (!text.empty()) {
            if (self.at_line_start_) {
                std::array<char, kLogTimestampCapacity> stamp{};
                const auto size = WriteLogTimestamp(stamp.data(), stamp.size());
                if (!self.sink_(self.context_, {stamp.data(), size})) {
                    return false;
                }
                self.at_line_start_ = false;
            }
            const auto newline = text.find('\n');
            const auto size = newline == std::string_view::npos ? text.size() : newline + 1U;
            if (!self.sink_(self.context_, text.substr(0U, size))) {
                return false;
            }
            self.at_line_start_ = newline != std::string_view::npos;
            text.remove_prefix(size);
        }
        return true;
    }

  private:
    CommandOutput::Writer sink_;
    void* context_;
    bool at_line_start_{true};
};
#endif

/** @brief Writes one complete result to the platform console, timestamped in applications. */
inline bool WriteConsoleResult(const std::string_view text) {
#if USE_TASMOTA_STATE
    std::array<char, kLogTimestampCapacity> stamp{};
    const auto size = WriteLogTimestamp(stamp.data(), stamp.size());
    if (!platform::write_console({stamp.data(), size})) {
        return false;
    }
#endif
    return platform::write_console(text);
}

} // namespace newmota
