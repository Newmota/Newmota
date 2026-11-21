/*
  energy_counter_filter.cpp - Primary hardware-counter plausibility policy

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  EnergyUpdateTotal reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_counter_filter.cpp
 * @brief Rejects out-of-band primary import observations before baselines or floors change.
 */
#include "services/energy/energy_service.h"
#include "newmota/log.h"

namespace newmota::energy {
/** @brief Mirrors source whole-kWh bandwidth without narrowing exact uint64 Wh accounts.
 * The source start is volatile and independent of lifetime seeds. Rejection leaves that
 * start, physical baselines, reset counts and floors unchanged; it does not invalidate
 * metrology or an independent export counter. Accepted first/zero readings establish
 * source start truthiness without inventing measured consumption.
 */
bool Service::accept_counter_import(
    const Provider& instance, Channel& channel, const Sample& sample) {
    if (&instance != &provider_ || !instance.device->source_total_counter() || !sample.valid)
        return true;
    const bool counter = sample.use_absolute && (sample.counter_fields & 1);
#if USE_ENERGY_HARDWARE_TOTAL
    const bool reference = sample.import_reference_valid && hardware_total_enabled_ &&
                           hardware_total_enabled_(hardware_total_context_);
    if (!counter && !reference && !sample.import_floor_valid)
        return true;
    const auto value = counter     ? sample.import_counter
                       : reference ? sample.import_reference
                                   : sample.import_floor;
#else
    if (!counter && !sample.import_floor_valid)
        return true;
    const auto value = counter ? sample.import_counter : sample.import_floor;
#endif
    if (value.micro_wh >= 1000000)
        return true; // Existing accounting validation reports malformed values.
    const std::uint8_t threshold =
        counter_filter_read_ ? counter_filter_read_(counter_filter_context_) : std::uint8_t{0};
    const bool started =
        channel.source_total_started && (!counter || (channel.account.baseline_fields & 1));
    if (started && threshold > 0 && threshold < 128) {
        SignedEnergy total{};
        if (!channel.account.compatibility_total(total))
            return false;
        /** abs(trunc(kWh)) uses only magnitude whole Wh; subtraction cannot overflow.
         * Exact wide arithmetic intentionally avoids upstream float/int overflow and UB.
         */
        const auto published = total.magnitude.wh / 1000;
        const auto incoming = value.wh / 1000;
        const auto distance = published > incoming ? published - incoming : incoming - published;
        if (distance > threshold) {
            if (!channel.source_total_rejected)
                AddLog(LogLevel::debug, "NRG: absolute import outside SetOption39 bandwidth");
            channel.source_total_rejected = true;
            return false;
        }
    }
    channel.source_total_rejected = false;
    channel.source_total_started = value.wh || value.micro_wh;
    return true;
}
} // namespace newmota::energy
