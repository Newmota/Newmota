/*
  energy_power_delta.cpp - Primary per-phase power change reporting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  PowerDelta/history/settling reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_power_delta.cpp
 * @brief Reporting-only policy; physical counters and samples are never changed.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "support/number.h"
#include "newmota/log.h"
#include <algorithm>
#include <new>

namespace newmota::energy {
void Service::bind_power_delta(bool (*event)(void*, std::string_view, std::string_view),
    void (*sensor)(void*), std::uint32_t (*revision)(void*), void* context) {
    delta_event_ = event;
    delta_sensor_ = sensor;
    delta_power_revision_ = revision;
    delta_context_ = context;
    delta_revision_ = revision ? revision(context) : 0;
}

CommandStatus Service::power_delta_command(const CommandRequest& request, CommandOutput& output) {
    auto& instance = provider_;
    const auto count = instance.configuration.channels;
    const std::size_t index = request.has_index ? request.index : 0;
    if (!count || !instance.channels || index > count)
        return CommandStatus::invalid;
    std::uint32_t value{};
    if (!request.is_query() && support::parse_unsigned_decimal(request.payload, 32000, value)) {
        std::array<std::uint16_t, kMaximumChannels> previous{};
        for (std::size_t c = 0; c < count; ++c) {
            previous[c] = instance.channels[c].power_delta;
            if (!index || c + 1 == index)
                instance.channels[c].power_delta = value;
        }
        const auto was_dirty = dirty_;
        dirty_ = true;
        if (!save()) {
            for (std::size_t c = 0; c < count; ++c)
                instance.channels[c].power_delta = previous[c];
            dirty_ = was_dirty;
            return CommandStatus::failed;
        }
    }
    if (index)
        return output.format("{\"PowerDelta%u\":%u}", unsigned(index),
                   unsigned(instance.channels[index - 1].power_delta))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    if (count == 1)
        return output.format("{\"PowerDelta\":%u}", unsigned(instance.channels[0].power_delta))
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    if (!output.write("{\"PowerDelta\":["))
        return CommandStatus::failed;
    for (std::size_t c = 0; c < count; ++c)
        if (!output.format("%s%u", c ? "," : "", unsigned(instance.channels[c].power_delta)))
            return CommandStatus::failed;
    return output.write("]}") ? CommandStatus::handled : CommandStatus::failed;
}

void Service::poll_power_delta(std::uint32_t now) {
    auto& primary = provider_;
    const auto count = primary.configuration.channels;
    if (delta_identity_ != primary.generation || delta_count_ != count) {
        delta_identity_ = primary.generation;
        delta_count_ = count;
        delta_phases_.reset(count ? new (std::nothrow) DeltaPhase[count] : nullptr);
        delta_stable_ = delta_steady_ = 0;
        delta_pending_event_ = delta_queue_failed_ = false;
        delta_page_cursor_ = 0;
        if (count && !delta_phases_)
            AddLog(LogLevel::error, "NRG: PowerDelta history allocation failed");
    }
    if (delta_power_revision_) {
        const auto revision = delta_power_revision_(delta_context_);
        if (revision != delta_revision_) {
            delta_revision_ = revision;
            delta_steady_ = 2;
        }
    }
    /** The startup latch remains armed across uint32 monotonic wrap. */
    if (now >= 8000)
        delta_started_ = true;
    if (now - delta_at_ < 1000) {
        flush_power_delta();
        return;
    }
    /** A stalled loop cannot manufacture historical one-second samples. */
    delta_at_ = now;
    bool changed = false;
#if USE_ENERGY_MARGINS
    bool margin_changed = false;
    const bool powered = primary_power_ && primary_power_(primary_power_context_);
#endif
    std::array<std::int16_t, kMaximumChannels> differences{};
    if (delta_started_ && count && delta_phases_ && primary.device) {
        if (delta_steady_)
            --delta_steady_;
        else
            for (std::size_t c = 0; c < count; ++c) {
#if USE_ENERGY_MARGINS
                if (powered) {
                    const auto changes = margin_changes(c);
                    delta_phases_[c].pending_margins |= changes;
                    margin_changed |= changes != 0;
                }
#endif
                const auto& sample = primary.channels[c].sample;
                /** Unknown power must not become a fabricated zero-watt crossing. */
                if (!sample.presentation_valid() || !(sample.available_fields & 4))
                    continue;
                auto& history = delta_phases_[c].history;
                /** Define source 16-bit narrowing explicitly, including signed export readings. */
                const auto watts = static_cast<std::uint16_t>(sample.power / 1000);
                const auto bits = static_cast<std::uint16_t>(watts - history[0]);
                const auto difference =
                    bits <= 32767 ? std::int32_t(bits) : std::int32_t(bits) - 65536;
                const auto magnitude = std::uint32_t(difference < 0 ? -difference : difference);
                const auto setting = primary.channels[c].power_delta;
                bool met = false;
                if (setting && magnitude) {
                    if (setting <= 100) {
                        const auto denominator =
                            std::max<std::uint16_t>(1, std::min(watts, history[0]));
                        const auto percent = std::uint16_t(magnitude * 100 / denominator);
                        met = percent >= setting;
                    } else
                        met = magnitude >= unsigned(setting - 100);
                }
                if (met) {
                    differences[c] = difference;
                    history[1] = history[2] = watts;
                    changed = true;
                }
                history[0] = history[1];
                history[1] = history[2];
                history[2] = watts;
            }
    }
    if (changed) {
        delta_pending_event_ = true;
        for (std::size_t c = 0; c < count; ++c)
            delta_phases_[c].pending_difference = differences[c];
    }
    if (changed
#if USE_ENERGY_MARGINS
        || margin_changed
#endif
    ) {
        if (delta_sensor_)
            delta_sensor_(delta_context_);
        delta_stable_ = 3;
    }
    /** Source decrements in the same second that a new event sets this counter to three. */
    if (delta_stable_ && !--delta_stable_ && delta_sensor_)
        delta_sensor_(delta_context_);
    flush_power_delta();
}

void Service::flush_power_delta() {
    if (!delta_count_ || !delta_phases_ || (delta_queue_failed_ && now_ - delta_retry_at_ < 1000))
        return;
    bool pending = delta_pending_event_;
#if USE_ENERGY_MARGINS
    std::array<std::uint8_t, kMaximumChannels> changes{};
    std::size_t selected = delta_count_;
    /** One phase's six flags fits beside the complete 32-phase PowerDelta vector.
     * This bounded page stays below the MQTT payload cap without truncating transitions.
     */
    for (std::size_t offset = 0; offset < delta_count_; ++offset) {
        const auto c = (delta_page_cursor_ + offset) % delta_count_;
        if (delta_phases_[c].pending_margins) {
            selected = c;
            changes[c] = delta_phases_[c].pending_margins;
            pending = true;
            break;
        }
    }
#endif
    if (!pending)
        return;
    /** At most31 printable alias bytes need at most62 escaped bytes; one phase plus
     * the bounded32-phase delta vector fits MQTT's1024-byte payload limit.
     */
    std::array<char, USE_ENERGY_MARGINS ? (USE_STATE_TEXT ? 1024 : 512) : 320> payload{};

    struct Destination {
        char* bytes;
        std::size_t capacity, used;
    } destination{payload.data(), payload.size(), 0};

    CommandOutput output(
        [](void* context, std::string_view text) {
            auto& target = *static_cast<Destination*>(context);
            if (text.size() > target.capacity - target.used)
                return false;
            std::copy(text.begin(), text.end(), target.bytes + target.used);
            target.used += text.size();
            return true;
        },
        &destination);
    bool complete = output.write("{\"MARGINS\":{");
    bool first = true;
    if (delta_pending_event_) {
        complete = complete && output.write("\"PowerDelta\":");
        if (delta_count_ > 1)
            complete = complete && output.write("[");
        for (std::size_t c = 0; c < delta_count_; ++c)
            complete = complete && output.format("%s%d", c ? "," : "",
                                       int(delta_phases_[c].pending_difference));
        if (delta_count_ > 1)
            complete = complete && output.write("]");
        first = false;
    }
#if USE_ENERGY_MARGINS
    complete = complete && write_margin_events(output, changes.data(), first);
#else
    (void)first;
#endif
    complete = complete && output.write("}}");
    if (!complete || !delta_event_ ||
        !delta_event_(delta_context_, "MARGINS", {payload.data(), destination.used})) {
        if (!delta_queue_failed_)
            AddLog(LogLevel::error, "NRG: MARGINS page pending; output or queue unavailable");
        delta_queue_failed_ = true;
        delta_retry_at_ = now_;
        return;
    }
    delta_queue_failed_ = false;
    delta_pending_event_ = false;
#if USE_ENERGY_MARGINS
    if (selected < delta_count_) {
        delta_phases_[selected].pending_margins = 0;
        delta_page_cursor_ = (selected + 1) % delta_count_;
    }
#endif
}
} // namespace newmota::energy
