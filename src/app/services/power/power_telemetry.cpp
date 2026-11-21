/*
  power_telemetry.cpp - canonical power telemetry serialization for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  StateText presentation reference: Tasmota support_command.ino/support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_telemetry.cpp
 * @brief Bounded canonical Power serialization with escaped retained state words.
 */
#include "services/power/power_telemetry.h"
#include "services/settings/state_text.h"

#include "core/command/command.h"
#include "newmota_features.h"
#if USE_SLEEP
#include "core/system/loop_sleep.h"
#endif
#include "services/power/power_service.h"
#if USE_PWM || USE_WS2812
#include "services/light/light_service.h"
#endif

#include <cstdarg>
#include <cstdio>

namespace newmota {
namespace {

/** @brief Appends one bounded formatted fragment without partial output. */
bool append(char* const output, const std::size_t capacity, std::size_t& size,
    const char* const format, ...) {
    if (output == nullptr || size >= capacity) {
        return false;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(output + size, capacity - size, format, arguments);
    va_end(arguments);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity - size) {
        return false;
    }
    size += static_cast<std::size_t>(written);
    return true;
}

/** @brief Appends one JSON relay field into caller-owned storage. */
bool append_power_field(char* const output, const std::size_t capacity, std::size_t& size,
    bool& first, const std::uint8_t relay_count, const std::uint8_t instance, const bool on) {
    const char* const separator = first ? "" : ",";
    first = false;
    if (!(relay_count == 1U && instance == 1U
                ? append(output, capacity, size, "%s\"POWER\":", separator)
                : append(output, capacity, size, "%s\"POWER%u\":", separator, unsigned(instance))))
        return false;

    struct Destination {
        char* bytes;
        std::size_t capacity;
        std::size_t& size;
    } target{output, capacity, size};

    CommandOutput writer(
        [](void* context, std::string_view text) {
            auto& destination = *static_cast<Destination*>(context);
            if (text.size() >= destination.capacity - destination.size)
                return false;
            for (const auto byte : text)
                destination.bytes[destination.size++] = byte;
            destination.bytes[destination.size] = 0;
            return true;
        },
        &target);
    return writer.json_string(state_text(on));
}

/** @brief Appends one JSON relay field to a command response. */
bool write_power_field(CommandOutput& output, bool& first, const std::uint8_t relay_count,
    const std::uint8_t instance, const bool on) {
    if (!first && !output.write(",")) {
        return false;
    }
    first = false;
    return (relay_count == 1U && instance == 1U
                   ? output.write("\"POWER\":")
                   : output.format("\"POWER%u\":", unsigned(instance))) &&
           output.json_string(state_text(on));
}

} // namespace

#if USE_SLEEP
bool write_sleep_state_fields(CommandOutput& output) {
    const auto sleep = loop_sleep::status();
    return output.format("\"SleepMode\":\"%s\",\"Sleep\":%u,\"LoadAvg\":%lu",
        loop_sleep::mode_name(sleep.normal), static_cast<unsigned>(sleep.sleep),
        static_cast<unsigned long>(sleep.load));
}
#endif

/** @brief Writes all currently configured relay fields in Tasmota order. */
bool write_power_state_fields(CommandOutput& output, const PowerService& power, bool first) {
    const auto relay_count = power.endpoint_count();
    for (std::uint8_t instance = 1U; instance <= kMaximumPowerOutputs; ++instance) {
        bool on = false;
        if (!power.state(instance, on)) {
            continue;
        }
        if (!write_power_field(output, first, relay_count, instance, on)) {
            return false;
        }
    }
#if USE_PWM || USE_WS2812
    if (power.light() != nullptr && !power.light()->write_state_fields(output, !first))
        return false;
#endif
    return true;
}

/** @brief Serializes one complete bounded canonical STATE document. */
bool serialize_power_state(const PowerService& power, const std::uint32_t uptime_seconds,
    char* const output, const std::size_t capacity, std::size_t& size) {
    size = 0U;
#if !USE_SLEEP
    if (!append(output, capacity, size, "{\"UptimeSec\":%lu",
            static_cast<unsigned long>(uptime_seconds))) {
#else
    const auto sleep = loop_sleep::status();
    if (!append(output, capacity, size,
            "{\"UptimeSec\":%lu,\"SleepMode\":\"%s\",\"Sleep\":%u,\"LoadAvg\":%lu",
            static_cast<unsigned long>(uptime_seconds), loop_sleep::mode_name(sleep.normal),
            static_cast<unsigned>(sleep.sleep), static_cast<unsigned long>(sleep.load))) {
#endif
        size = 0U;
        return false;
    }

    bool first = false;
    const auto relay_count = power.endpoint_count();
    for (std::uint8_t instance = 1U; instance <= kMaximumPowerOutputs; ++instance) {
        bool on = false;
        if (!power.state(instance, on)) {
            continue;
        }
        if (!append_power_field(output, capacity, size, first, relay_count, instance, on)) {
            size = 0U;
            return false;
        }
    }
#if USE_PWM || USE_WS2812
    if (power.light() != nullptr) {
        struct Sink {
            char* data;
            std::size_t capacity;
            std::size_t* size;
        } sink{output, capacity, &size};

        CommandOutput fields{
            [](void* context, const std::string_view text) {
                auto& sink = *static_cast<Sink*>(context);
                if (*sink.size >= sink.capacity || text.size() >= sink.capacity - *sink.size)
                    return false;
                for (const char value : text)
                    sink.data[(*sink.size)++] = value;
                sink.data[*sink.size] = '\0';
                return true;
            },
            &sink};
        if (!power.light()->write_state_fields(fields)) {
            size = 0U;
            return false;
        }
    }
#endif
    if (!append(output, capacity, size, "}")) {
        size = 0U;
        return false;
    }
    return true;
}

/** @brief Keeps the PowerService-facing serializer as the public entry point. */
bool PowerService::serialize_state(const std::uint32_t uptime_seconds, char* const output,
    const std::size_t capacity, std::size_t& size) const {
    return serialize_power_state(*this, uptime_seconds, output, capacity, size);
}

} // namespace newmota
