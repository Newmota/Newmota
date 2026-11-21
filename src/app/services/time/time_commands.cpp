/*
  time_commands.cpp - time commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file time_commands.cpp
 * @brief Persistent civil-time configuration and bounded NTP server commands.
 */

#include "services/time/time_commands.h"

#include "newmota/platform/platform.h"
#include "services/settings/settings.h"
#include "services/time/time_service.h"
#include "support/number.h"
#include "support/text.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace newmota::commands {
namespace {

/** @brief Maps a persistent settings outcome to command status. */
[[nodiscard]] CommandStatus update_status(const SettingsUpdateResult result) {
    switch (result) {
    case SettingsUpdateResult::pending:
    case SettingsUpdateResult::saved:
    case SettingsUpdateResult::unchanged:
        return CommandStatus::handled;
    case SettingsUpdateResult::invalid:
        return CommandStatus::invalid;
    case SettingsUpdateResult::failed:
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

/** @brief Writes one ISO-8601 calendar value without timezone decoration. */
[[nodiscard]] bool write_calendar(CommandOutput& output, const std::uint32_t seconds) {
    const auto value = TimeService::civil_time(seconds);
    return output.format("%04u-%02u-%02uT%02u:%02u:%02u", static_cast<unsigned>(value.year),
        static_cast<unsigned>(value.month), static_cast<unsigned>(value.day),
        static_cast<unsigned>(value.hour), static_cast<unsigned>(value.minute),
        static_cast<unsigned>(value.second));
}

/** @brief Writes a signed timezone offset in Tasmota's +HH:MM form. */
[[nodiscard]] bool write_timezone_value(CommandOutput& output, const std::int16_t minutes) {
    const auto magnitude =
        static_cast<unsigned>(minutes < 0 ? -static_cast<std::int32_t>(minutes) : minutes);
    return output.format("%c%02u:%02u", minutes < 0 ? '-' : '+', magnitude / 60U, magnitude % 60U);
}

/** @brief Writes the configured Timezone command value. */
[[nodiscard]] bool write_timezone_response(CommandOutput& output, const Settings& settings) {
    if (settings.timezone_automatic()) {
        return output.write("{\"Timezone\":99}");
    }
    return output.write("{\"Timezone\":\"") &&
           write_timezone_value(output, settings.timezone_offset_minutes()) && output.write("\"}");
}

/** @brief Writes the Time fields (and Epoch for format 1) without braces. */
[[nodiscard]] bool write_time_fields(CommandOutput& output, const Settings& settings,
    const TimeService& time, const std::uint32_t now, const std::uint8_t format) {
    const auto snapshot = time.snapshot(now);
    if (format == 2) {
        return output.format("\"Time\":%lu", static_cast<unsigned long>(snapshot.utc_seconds));
    }

    if (!output.write("\"Time\":\"") || !write_calendar(output, snapshot.local_seconds)) {
        return false;
    }
    if (format == 3 && !output.format(".%03u", static_cast<unsigned>(time.millisecond(now)))) {
        return false;
    }
    std::uint8_t append_timezone = 0;
    (void)settings.option(52, append_timezone);
    if (append_timezone != 0 && snapshot.valid &&
        !write_timezone_value(output, snapshot.offset_minutes)) {
        return false;
    }
    if (!output.write("\"")) {
        return false;
    }
    return format != 1 ||
           output.format(",\"Epoch\":%lu", static_cast<unsigned long>(snapshot.utc_seconds));
}

/** @brief Writes one Time response in the selected persistent format. */
[[nodiscard]] bool write_time_response(CommandOutput& output, const Settings& settings,
    const TimeService& time, const std::uint32_t now, const std::uint8_t format) {
    return output.write("{") && write_time_fields(output, settings, time, now, format) &&
           output.write("}");
}

/** @brief Parses a fixed Timezone payload into total signed minutes. */
[[nodiscard]] bool parse_timezone(std::string_view text, std::int16_t& minutes) {
    text = support::trim_ascii(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const auto separator = text.find(':');
    const auto hour_text = text.substr(0, separator);
    const auto minute_text =
        separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    if (hour_text.empty() ||
        (separator != std::string_view::npos && minute_text.find(':') != std::string_view::npos)) {
        return false;
    }

    std::uint32_t hours = 0;
    std::uint32_t minute_part = 0;
    if (!support::parse_unsigned_decimal(hour_text, negative ? 13U : 14U, hours) ||
        (!minute_text.empty() && !support::parse_unsigned_decimal(minute_text,
                                     std::numeric_limits<std::uint32_t>::max(), minute_part)) ||
        (separator != std::string_view::npos && minute_text.empty())) {
        return false;
    }
    if (minute_part > 59) {
        minute_part = 59;
    }
    const auto magnitude = static_cast<std::int16_t>(hours * 60U + minute_part);
    minutes = negative ? static_cast<std::int16_t>(-magnitude) : magnitude;
    return true;
}

/** @brief Handles Time query, response-format selection, and manual epochs. */
[[nodiscard]] CommandStatus time_command(
    const CommandRequest& request, Settings& settings, TimeService& time, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }

    auto format = settings.time_format();
    const auto now = platform::monotonic_milliseconds();
    if (!request.is_query()) {
        std::uint32_t value = 0;
        if (!support::parse_unsigned_decimal(
                request.payload, std::numeric_limits<std::uint32_t>::max(), value)) {
            return CommandStatus::invalid;
        }
        if (value >= 1 && value <= 4) {
            const auto status =
                update_status(settings.set_time_format(static_cast<std::uint8_t>(value - 1)));
            if (status != CommandStatus::handled) {
                return status;
            }
            format = static_cast<std::uint8_t>(value - 1);
        } else {
            format = 1;
            if (!time.set_utc_seconds(value, now)) {
                time.enable_network_time(now);
            }
        }
    }
    return write_time_response(output, settings, time, now, format) ? CommandStatus::handled
                                                                    : CommandStatus::failed;
}

#if USE_NTP
/** @brief Queries all NTP slots or updates one explicitly indexed address. */
[[nodiscard]] CommandStatus ntp_server_command(
    const CommandRequest& request, Settings& settings, TimeService& time, CommandOutput& output) {
    if (!request.has_index) {
        if (!output.write("{"))
            return CommandStatus::failed;
        for (std::uint8_t slot = 0; slot < Settings::kNtpServerCount; ++slot) {
            const auto address = settings.ntp_server(slot);
            if (!output.format("%s\"NtpServer%u\":\"%.*s\"", slot == 0 ? "" : ",",
                    static_cast<unsigned>(slot + 1), static_cast<int>(address.size()),
                    address.data()))
                return CommandStatus::failed;
        }
        return output.write("}") ? CommandStatus::handled : CommandStatus::failed;
    }
    if (request.index < 1 || request.index > Settings::kNtpServerCount)
        return CommandStatus::invalid;
    const auto slot = static_cast<std::uint8_t>(request.index - 1);
    if (!request.is_query()) {
        auto value = support::trim_ascii(request.payload);
        if (value == "0")
            value = {};
        else if (value == "1")
            value = Settings::default_ntp_server(slot);
        if (value.size() > Settings::kNtpServerCapacity)
            return CommandStatus::invalid;
        std::array<char, Settings::kNtpServerCapacity> normalized{};
        for (std::size_t index = 0; index < value.size(); ++index)
            normalized[index] = value[index] == ',' ? '.' : value[index];
        const auto result = settings.set_ntp_server(slot, {normalized.data(), value.size()});
        const auto status = update_status(result);
        if (status != CommandStatus::handled)
            return status;
        if (result == SettingsUpdateResult::saved || result == SettingsUpdateResult::unchanged)
            time.retry_network_time(platform::monotonic_milliseconds());
    }
    const auto address = settings.ntp_server(slot);
    return output.format("{\"NtpServer%u\":\"%.*s\"}", static_cast<unsigned>(slot + 1),
               static_cast<int>(address.size()), address.data())
               ? CommandStatus::handled
               : CommandStatus::failed;
}
#endif

/** @brief Handles fixed-offset and Timezone 99 automatic-DST selection. */
[[nodiscard]] CommandStatus timezone_command(
    const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        SettingsUpdateResult result{};
        const auto payload = support::trim_ascii(request.payload);
        std::int32_t whole_hours = 0;
        const bool automatic = payload.find(':') == std::string_view::npos &&
                               support::parse_signed_decimal(payload, -13,
                                   std::numeric_limits<std::int32_t>::max(), whole_hours) &&
                               whole_hours >= 15;
        if (automatic) {
            result = settings.set_timezone(settings.timezone_offset_minutes(), true);
        } else {
            std::int16_t minutes = 0;
            if (!parse_timezone(request.payload, minutes)) {
                return CommandStatus::invalid;
            }
            result = settings.set_timezone(minutes, false);
        }
        const auto status = update_status(result);
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return write_timezone_response(output, settings) ? CommandStatus::handled
                                                     : CommandStatus::failed;
}

/** @brief Clamps an integer to one transition-rule field range. */
[[nodiscard]] std::int32_t clamp(
    const std::int32_t value, const std::int32_t minimum, const std::int32_t maximum) {
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

/** @brief Applies a six-field TimeStd or TimeDst comma-separated payload. */
[[nodiscard]] bool parse_rule(std::string_view payload, TimeRule& rule) {
    std::array<std::string_view, 6> fields{};
    std::size_t count = 0;
    bool extra_field = false;
    while (count < fields.size()) {
        const auto separator = payload.find(',');
        fields[count++] = support::trim_ascii(payload.substr(0, separator));
        if (separator == std::string_view::npos) {
            break;
        }
        payload.remove_prefix(separator + 1);
        extra_field = count == fields.size();
    }
    if (extra_field || count != fields.size()) {
        return false;
    }

    std::array<std::int32_t, 6> values{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].empty()) {
            continue;
        }
        if (!support::parse_signed_decimal(fields[index], -10'000, 10'000, values[index])) {
            return false;
        }
        switch (index) {
        case 0:
            rule.hemisphere = static_cast<std::uint8_t>(values[index] & 1);
            break;
        case 1:
            rule.week = static_cast<std::uint8_t>(clamp(values[index], 0, 4));
            break;
        case 2:
            rule.month = static_cast<std::uint8_t>(clamp(values[index], 1, 12));
            break;
        case 3:
            rule.day_of_week = static_cast<std::uint8_t>(clamp(values[index], 1, 7));
            break;
        case 4:
            rule.hour = static_cast<std::uint8_t>(clamp(values[index], 0, 23));
            break;
        case 5:
            rule.offset_minutes = static_cast<std::int16_t>(clamp(values[index], -900, 900));
            break;
        }
    }
    return true;
}

/** @brief Writes one TimeStd or TimeDst rule response. */
[[nodiscard]] bool write_rule(CommandOutput& output, const char* name, const TimeRule& rule) {
    return output.format("{\"%s\":{\"Hemisphere\":%u,\"Week\":%u,\"Month\":%u,"
                         "\"Day\":%u,\"Hour\":%u,\"Offset\":%d}}",
        name, static_cast<unsigned>(rule.hemisphere), static_cast<unsigned>(rule.week),
        static_cast<unsigned>(rule.month), static_cast<unsigned>(rule.day_of_week),
        static_cast<unsigned>(rule.hour), static_cast<int>(rule.offset_minutes));
}

/** @brief Handles one standard or daylight transition-rule command. */
[[nodiscard]] CommandStatus time_rule_command(const CommandRequest& request, Settings& settings,
    CommandOutput& output, const std::uint8_t index, const char* name) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        TimeRule rule = settings.time_rule(index);
        const auto payload = support::trim_ascii(request.payload);
        if (payload == "0") {
            rule = Settings::default_time_rule(index);
        } else if (payload.find(',') == std::string_view::npos) {
            return write_rule(output, name, rule) ? CommandStatus::handled : CommandStatus::failed;
        } else if (!parse_rule(payload, rule)) {
            return CommandStatus::invalid;
        }
        const auto status = update_status(settings.set_time_rule(index, rule));
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return write_rule(output, name, settings.time_rule(index)) ? CommandStatus::handled
                                                               : CommandStatus::failed;
}

} // namespace

/** @brief Dispatches Tasmota-compatible Time and timezone commands. */
CommandStatus dispatch_time(
    const CommandRequest& request, Settings& settings, TimeService& time, CommandOutput& output) {
    if (support::ascii_iequals(request.name, "time")) {
        return time_command(request, settings, time, output);
    }
    if (support::ascii_iequals(request.name, "timezone")) {
        return timezone_command(request, settings, output);
    }
#if USE_NTP
    if (support::ascii_iequals(request.name, "ntpserver")) {
        return ntp_server_command(request, settings, time, output);
    }
#endif
    if (support::ascii_iequals(request.name, "timestd")) {
        return time_rule_command(request, settings, output, Settings::kStandardTimeRule, "TimeStd");
    }
    if (support::ascii_iequals(request.name, "timedst")) {
        return time_rule_command(request, settings, output, Settings::kDaylightTimeRule, "TimeDst");
    }
    return CommandStatus::unknown;
}

bool write_state_time(CommandOutput& output, const Settings& settings, const TimeService& time,
    const std::uint32_t now) {
    return write_time_fields(output, settings, time, now, settings.time_format());
}

} // namespace newmota::commands
