/*
  energy_protection_policy.cpp - Shared source protection timing and event publication

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyMarginCheck/MaxPower/MaxEnergy event reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/
/** @file energy_protection_policy.cpp
 * @brief Shared startup/steady gate and bounded MaxPower/MaxEnergy event publication.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_sum.h"
#include "services/power/power_service.h"
#include "services/settings/settings.h"
#include "services/settings/state_text.h"
#include "services/time/time_service.h"
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"
#include "newmota/log.h"
#include <algorithm>
#include <new>

namespace newmota::energy {
void Service::bind_protection_policy(const Settings& settings, const TimeService& time) {
    protection_settings_ = &settings;
    protection_time_ = &time;
    protection_revision_ = protection_power_ ? protection_power_->operation_revision() : 0;
}

void Service::bind_protection_events(bool (*event)(void*, std::string_view, std::string_view, bool),
    void (*sensor)(void*), void* context) {
    protection_event_sink_ = event;
    protection_sensor_ = sensor;
    protection_event_context_ = context;
}

void Service::publish_protection_event(
    ProtectionEvent kind, std::size_t channel, std::uint16_t watts, const SignedEnergy* daily) {
    const bool retry =
        kind == ProtectionEvent::power_retry || kind == ProtectionEvent::energy_start;
    const bool exhausted = kind == ProtectionEvent::power_exhausted;
    const bool energy =
        kind == ProtectionEvent::energy_trip || kind == ProtectionEvent::energy_start;
    std::array<char, 224> bytes{};

    struct Buffer {
        char* data;
        std::size_t used, capacity;
    } buffer{bytes.data(), 0, bytes.size()};

    CommandOutput output(
        [](void* context, std::string_view value) {
            auto& b = *static_cast<Buffer*>(context);
            if (value.size() > b.capacity - b.used)
                return false;
            std::copy(value.begin(), value.end(), b.data + b.used);
            b.used += value.size();
            return true;
        },
        &buffer);
    bool okay = output.write("{");
    if (protection_time_ && protection_settings_) {
        const auto snapshot = protection_time_->snapshot(now_);
        const auto civil = TimeService::civil_time(snapshot.local_seconds);
        const auto format = protection_settings_->time_format();
        if (format == 2)
            okay = okay &&
                   output.format("\"Time\":%lu,", static_cast<unsigned long>(snapshot.utc_seconds));
        else {
            okay =
                okay && output.format("\"Time\":\"%04u-%02u-%02uT%02u:%02u:%02u",
                            unsigned(civil.year), unsigned(civil.month), unsigned(civil.day),
                            unsigned(civil.hour), unsigned(civil.minute), unsigned(civil.second));
            if (format == 3)
                okay =
                    okay && output.format(".%03u", unsigned(protection_time_->millisecond(now_)));
            okay = okay && output.write("\",");
            if (format == 1)
                okay = okay && output.format("\"Epoch\":%lu,",
                                   static_cast<unsigned long>(snapshot.utc_seconds));
        }
    }
    const char* key = energy      ? (retry ? "EnergyMonitor" : "MaxEnergyReached")
                      : retry     ? "PowerMonitor"
                      : exhausted ? "MaxPowerReachedRetry"
                                  : "MaxPowerReached";
    okay = okay && (energy ? output.format("\"%s\":", key)
                           : output.format("\"%s%u\":", key, unsigned(channel)));
    if (retry || exhausted)
        okay = okay && output.json_string(state_text(retry ? 1 : 0));
    else if (energy) {
        detail::EnergySum value{};
        if (daily)
            value.add(*daily);
        okay = okay && daily && value.write(output, 3);
    } else
        okay = okay && output.format("%u", unsigned(watts));
    okay = okay && output.write("}");
    /** A bounded copying sink is fire-and-forget like the source. Disconnected/full
     * queues are diagnosed, never delay an enforcement attempt or replay a stale warning.
     */
    if (!okay || !protection_event_sink_ ||
        !protection_event_sink_(protection_event_context_,
            retry ? (energy ? "EnergyMonitor" : "PowerMonitor") : "WARNING",
            {bytes.data(), buffer.used}, retry))
        AddLog(LogLevel::error, "NRG: %s event unavailable", key);
    if (!retry && protection_sensor_)
        protection_sensor_(protection_event_context_);
}

bool Service::protection_tick(std::uint32_t now) {
    if (protection_power_) {
        const auto revision = protection_power_->operation_revision();
        if (revision != protection_revision_) {
            protection_revision_ = revision;
            protection_steady_ = 2;
        }
    }
    if (now >= 8000)
        protection_started_ = true;
    if (now - protection_at_ < 1000)
        return false;
    protection_at_ = now;
    if (!protection_started_ || !protection_settings_ || !provider_.device ||
        !provider_.configuration.channels)
        return false;
    if (protection_steady_) {
        --protection_steady_;
        return false;
    }
    return true;
}
} // namespace newmota::energy
