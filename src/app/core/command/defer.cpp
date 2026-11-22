/*
  defer.cpp - one-shot deferred commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file defer.cpp
 * @brief Fixed slots of command lines with a due time, run from the main loop.
 */

#include "core/command/defer.h"

#include "core/command/result_timestamp.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"

#include <array>
#include <cstring>
#include <string_view>

namespace newmota::defer {
namespace {

struct Slot final {
    std::uint32_t due;
    std::uint32_t order; ///< Scheduling order, so equal due times run as scheduled.
    std::uint8_t size;   ///< 0 marks a free slot.
    std::array<char, kLineCapacity> line;
};

std::array<Slot, kSlotCount> slots{};
std::uint32_t next_order{};

[[nodiscard]] unsigned int pending() {
    unsigned int count = 0U;
    for (const auto& slot : slots) {
        count += slot.size != 0U ? 1U : 0U;
    }
    return count;
}

bool write_console(void*, const std::string_view text) {
    return platform::write_console(text);
}

} // namespace

CommandStatus command(const CommandRequest& request, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "defer")) {
        return CommandStatus::unknown;
    }
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.has_payload) {
        const auto cancelled = pending();
        clear();
        return output.format("{\"Defer\":{\"Cancelled\":%u}}", cancelled) ? CommandStatus::handled
                                                                          : CommandStatus::failed;
    }
    if (request.is_query()) {
        return output.format("{\"Defer\":{\"Pending\":%u}}", pending()) ? CommandStatus::handled
                                                                        : CommandStatus::failed;
    }

    const auto payload = support::trim_ascii(request.payload);
    const auto separator = payload.find_first_of(" \t");
    if (separator == std::string_view::npos) {
        return CommandStatus::invalid;
    }
    std::uint32_t milliseconds = 0U;
    const auto line = support::trim_ascii(payload.substr(separator + 1U));
    // The deferred line is checked now, with the full chain rules, so it cannot fail later
    // for its syntax.
    if (!support::parse_unsigned_decimal(
            payload.substr(0U, separator), kMaximumMilliseconds, milliseconds) ||
        line.empty() || line.size() > kLineCapacity || !validate_command_line(line)) {
        return CommandStatus::invalid;
    }
    Slot* free_slot = nullptr;
    for (auto& slot : slots) {
        if (slot.size == 0U) {
            free_slot = &slot;
            break;
        }
    }
    if (free_slot == nullptr) {
        (void)output.write("{\"Defer\":\"Full\"}");
        return CommandStatus::failed;
    }
    free_slot->due = platform::monotonic_milliseconds() + milliseconds;
    free_slot->order = next_order++;
    std::memcpy(free_slot->line.data(), line.data(), line.size());
    free_slot->size = static_cast<std::uint8_t>(line.size());
    return output.format("{\"Defer\":{\"Ms\":%lu,\"Pending\":%u}}",
               static_cast<unsigned long>(milliseconds), pending())
               ? CommandStatus::handled
               : CommandStatus::failed;
}

void poll(CommandProcessor& processor, const std::uint32_t now) {
    Slot* next = nullptr;
    for (auto& slot : slots) {
        if (slot.size == 0U || static_cast<std::int32_t>(now - slot.due) < 0) {
            continue;
        }
        if (next == nullptr || static_cast<std::int32_t>(slot.due - next->due) < 0 ||
            (slot.due == next->due && static_cast<std::int32_t>(slot.order - next->order) < 0)) {
            next = &slot;
        }
    }
    if (next == nullptr) {
        return;
    }
    // Free the slot first: the command may defer or cancel further commands.
    std::array<char, kLineCapacity> line{};
    const std::size_t size = next->size;
    std::memcpy(line.data(), next->line.data(), size);
    next->size = 0U;

    // No caller waits any more: answer on the serial console, in the web console history and
    // through the MQTT result mirror, as a serial command does.
#if USE_TASMOTA_STATE
    TimestampedOutput stamped{write_console, nullptr};
    CommandOutput output{TimestampedOutput::write, &stamped};
#else
    CommandOutput output{write_console, nullptr};
#endif
    const auto result = processor.execute({line.data(), size}, CommandSource::internal, output);
    if (!result.response_suppressed && !result.output_failed && output.size() != 0U) {
        (void)platform::write_console("\r\n");
    }
}

void clear() {
    for (auto& slot : slots) {
        slot.size = 0U;
    }
}

} // namespace newmota::defer
