/*
  serial_console.cpp - serial command console implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/command/serial_console.h"

#include "core/command/result_timestamp.h"
#include "newmota/platform/platform.h"

#include <string_view>

namespace newmota {
namespace {

constexpr std::size_t kMaximumCharactersPerPoll = 64;

/**
 * @brief Adapts command response writes to the active platform console.
 */
bool write_console(void*, const std::string_view text) {
    return platform::write_console(text);
}

} // namespace

/**
 * @brief Reads bounded console input and dispatches each complete command line.
 */
bool SerialConsole::poll(CommandProcessor& processor) {
    char character = 0;
    std::size_t received = 0;
    while (received < kMaximumCharactersPerPoll && platform::read_console_character(character)) {
        ++received;
        if (character == '\r' || character == '\n') {
            if (character == '\n' && last_was_carriage_return_) {
                last_was_carriage_return_ = false;
                continue;
            }
            last_was_carriage_return_ = character == '\r';
            finish_line(processor);
            continue;
        }
        last_was_carriage_return_ = false;

        if (character == '\b' || character == 0x7f) {
            if (!overflow_ && length_ > 0) {
                --length_;
            }
            continue;
        }
        if (static_cast<unsigned char>(character) < 0x20) {
            continue;
        }
        if (overflow_) {
            continue;
        }
        if (length_ == line_.size()) {
            overflow_ = true;
            continue;
        }
        line_[length_++] = character;
    }
    return received != 0;
}

/**
 * @brief Executes or rejects the buffered line and resets input state.
 */
void SerialConsole::finish_line(CommandProcessor& processor) {
    if (overflow_) {
        (void)WriteConsoleResult("{\"Command\":\"Input too long\"}\r\n");
    } else if (length_ != 0) {
#if USE_TASMOTA_STATE
        TimestampedOutput stamped{write_console, nullptr};
        CommandOutput output{TimestampedOutput::write, &stamped};
#else
        CommandOutput output{write_console, nullptr};
#endif
        const auto result =
            processor.execute({line_.data(), length_}, CommandSource::serial, output);
        if (!result.response_suppressed && !result.output_failed && output.size() != 0) {
            (void)platform::write_console("\r\n");
        }
    }
    length_ = 0;
    overflow_ = false;
}

} // namespace newmota
