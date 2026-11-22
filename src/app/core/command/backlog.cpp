/*
  backlog.cpp - Tasmota's Backlog queue, Delay and NoDelay for Newmota (trial, plan 43)

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file backlog.cpp
 * @brief Copies Tasmota's CmndBacklog, CmndDelay and BacklogLoop (support_command.ino,
 * tasmota.ino, Tasmota 15.5.0.2) behind USE_BACKLOG.
 */

#include "core/command/command.h"

#include "core/command/result_timestamp.h"
#include "newmota/platform/platform.h"
#include "services/settings/settings.h"
#if USE_TIME
#include "services/time/time_service.h"
#endif
#include "support/text.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

namespace newmota {

/** @brief One queued command; its text follows the node in the same allocation. */
struct CommandProcessor::BacklogEntry final {
    BacklogEntry* next;
    std::size_t size;

    [[nodiscard]] char* text() { return reinterpret_cast<char*>(this + 1); }
};

namespace {

constexpr std::string_view kBacklog = "Backlog";
constexpr std::string_view kReset = "Reset";
constexpr std::string_view kNoDelay = "NoDelay";

/** @brief Tasmota's strncasecmp(text, prefix, strlen(prefix)) == 0. */
[[nodiscard]] bool starts_with(const std::string_view text, const std::string_view prefix) {
    return text.size() >= prefix.size() &&
           support::ascii_iequals(text.substr(0U, prefix.size()), prefix);
}

/** @brief Tasmota's ChrCount. */
[[nodiscard]] std::size_t count(const std::string_view text, const char character) {
    std::size_t result = 0U;
    for (const char c : text) {
        result += c == character ? 1U : 0U;
    }
    return result;
}

/** @brief One strtok(…, ";") step: skips separators and returns the next token's range. */
[[nodiscard]] bool next_token(
    const std::string_view data, std::size_t& position, std::size_t& begin, std::size_t& end) {
    while (position < data.size() && data[position] == ';') {
        ++position;
    }
    if (position == data.size()) {
        return false;
    }
    begin = position;
    while (position < data.size() && data[position] != ';') {
        ++position;
    }
    end = position;
    if (position < data.size()) {
        ++position;
    }
    return true;
}

/** @brief Writes queued results to the serial console, as the serial console does. */
bool write_console(void*, const std::string_view text) {
    return platform::write_console(text);
}

} // namespace

/** @brief Recognises a line that Tasmota's command parser would hand to CmndBacklog. */
bool is_backlog_line(const std::string_view line) {
    auto command = support::trim_ascii(line);
    if (!command.empty() && command.front() == '_') {
        command.remove_prefix(1U);
        command = support::trim_ascii(command);
    }
    auto name = command.substr(0U, command.find_first_of(" \t\r\n\f\v"));
    if (!starts_with(name, kBacklog)) {
        return false;
    }
    name.remove_prefix(kBacklog.size());
    for (const char c : name) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

/** @brief Appends one entry at the tail, like LList::addToLast with a strdup. */
void CommandProcessor::append_backlog(const std::string_view command) {
    void* const memory = std::malloc(sizeof(BacklogEntry) + command.size() + 1U);
    if (memory == nullptr) {
        return; // Tasmota drops the entry when strdup fails.
    }
    auto* const entry = ::new (memory) BacklogEntry{nullptr, command.size()};
    std::memcpy(entry->text(), command.data(), command.size());
    entry->text()[command.size()] = '\0';
    if (backlog_tail_ == nullptr) {
        backlog_head_ = entry;
    } else {
        backlog_tail_->next = entry;
    }
    backlog_tail_ = entry;
}

/** @brief Removes the head entry; the caller frees it. */
CommandProcessor::BacklogEntry* CommandProcessor::pop_backlog() {
    auto* const entry = backlog_head_;
    if (entry != nullptr) {
        backlog_head_ = entry->next;
        if (backlog_head_ == nullptr) {
            backlog_tail_ = nullptr;
        }
    }
    return entry;
}

/** @brief Tasmota's CmndBacklog. */
CommandStatus CommandProcessor::backlog_command(
    const CommandRequest& request, CommandOutput& output) {
    if (!request.is_query()) {
        const std::uint32_t index = request.has_index ? request.index : 1U;
        backlog_nodelay_ = (index & 0x01U) == 0U;          // Backlog0, Backlog2
        backlog_no_mqtt_response_ = (index & 0x02U) != 0U; // Backlog2, Backlog3

        const auto data = request.payload;
        std::size_t position = 0U;
        std::size_t begin = 0U;
        std::size_t end = 0U;
        bool more = next_token(data, position, begin, end);
        while (more) {
            // Ignore semicolons between braces {}: the entry grows by whole tokens.
            while (count(data.substr(begin, end - begin), '{') !=
                   count(data.substr(begin, end - begin), '}')) {
                std::size_t next_begin = 0U;
                if (!next_token(data, position, next_begin, end)) {
                    more = false;
                    break;
                }
            }
            // Skip unnecessary Backlog words at the start of the entry.
            auto command = support::trim_ascii(data.substr(begin, end - begin));
            while (starts_with(command, kBacklog)) {
                command.remove_prefix(kBacklog.size());
                command = support::trim_ascii(command);
            }
            // Reset is never queued.
            if (!command.empty() && !starts_with(command, kReset)) {
                append_backlog(command);
            }
            more = more && next_token(data, position, begin, end);
        }
        backlog_timer_ = platform::monotonic_milliseconds();
        return CommandStatus::handled; // ResponseClear: no answer
    }
    const bool empty = backlog_head_ == nullptr;
    while (auto* const entry = pop_backlog()) {
        std::free(entry);
    }
    return output.write(empty ? "{\"Backlog\":\"Empty\"}" : "{\"Backlog\":\"Aborted\"}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Tasmota's CmndDelay. */
CommandStatus CommandProcessor::delay_command(
    const CommandRequest& request, CommandOutput& output) {
    // XdrvMailbox.payload: strtol with base 0, a state word, or -99 without a number.
    std::int32_t payload = -99;
    if (!request.is_query()) {
        std::array<char, 24> text{};
        const auto size =
            request.payload.size() < text.size() ? request.payload.size() : text.size() - 1U;
        std::memcpy(text.data(), request.payload.data(), size);
        char* parsed_end = nullptr;
        const long value = std::strtol(text.data(), &parsed_end, 0);
        if (parsed_end != text.data()) {
            payload = static_cast<std::int32_t>(value);
        }
        const auto state = parse_command_state(request.payload);
        if (state != CommandState::invalid) {
            payload = state == CommandState::off ? 0 : state == CommandState::on ? 1 : 2;
        }
    }
    const auto now = platform::monotonic_milliseconds();
    if (payload == -1) {
        // Next full second of the clock, as Tasmota's 1000 - RtcMillis().
        std::uint32_t millisecond = now % 1000U;
#if USE_TIME
        if (time_.synchronized()) {
            millisecond = time_.millisecond(now);
        }
#endif
        backlog_timer_ = now + (1000U - millisecond);
    } else if (payload >= 2 && payload <= 3600) {
        backlog_timer_ = now + 100U * static_cast<std::uint32_t>(payload);
    }
    const auto delta = static_cast<std::int32_t>(now - backlog_timer_);
    const std::uint32_t remaining = delta < 0 ? static_cast<std::uint32_t>(-delta) / 100U : 0U;
    return output.format("{\"Delay\":%u}", static_cast<unsigned int>(remaining))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief CommandHandler's backlog_timer = millis() + SetOption34, for every command. */
void CommandProcessor::restart_backlog_gap() {
    std::uint8_t gap = 0U;
    (void)settings_.option(34U, gap);
    backlog_timer_ = platform::monotonic_milliseconds() + gap;
}

/** @brief Tasmota's BacklogLoop: one queued command per main-loop pass. */
void CommandProcessor::poll_backlog(const std::uint32_t now) {
    if (static_cast<std::int32_t>(now - backlog_timer_) < 0) {
        return;
    }
    bool nodelay = false;
    while (auto* const entry = pop_backlog()) {
        const std::string_view command{entry->text(), entry->size};
        if (starts_with(command, kNoDelay)) {
            std::free(entry);
            nodelay = true;
            continue;
        }
#if USE_TASMOTA_STATE
        TimestampedOutput stamped{write_console, nullptr};
        CommandOutput output{TimestampedOutput::write, &stamped};
#else
        CommandOutput output{write_console, nullptr};
#endif
        const auto result = execute(command, CommandSource::backlog, output);
        if (!result.response_suppressed && !result.output_failed && output.size() != 0U) {
            (void)platform::write_console("\r\n");
        }
        std::free(entry);
        if (nodelay || backlog_nodelay_) {
            // Reset the timer the command has just set.
            backlog_timer_ = platform::monotonic_milliseconds();
        }
        break;
    }
    if (backlog_head_ == nullptr) {
        backlog_nodelay_ = false;
    }
}

} // namespace newmota
