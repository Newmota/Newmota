/*
  tuya_energy.cpp - TuyaMCU energy data points as an energy provider

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Provider behavior adapted from Tasmota xdrv_16_tuyamcu_v2.ino (Xnrg32).
  Copyright (C) 2021 Theo Arends
  Upstream distributed under GPL version 3 or later, without any warranty.
*/

/** @file tuya_energy.cpp
 * @brief Shows the MCU's power, voltage, current and total as the one energy provider; the
 * TuyaMCU driver keeps the values.
 */
#include "services/energy/energy_service.h"
#include "drivers/tuya/tuya_mcu.h"
#include <new>

namespace newmota::energy {
namespace {
/** @brief One channel without pins. The service integrates the last reported power; a mapped
 * total (fnId 37) drives the accounts instead by its increase since start, as Tasmota's
 * EnergyUpdateTotal: the first report after start and a lower value only set the start. The
 * start is not retained, so a remapped total never continues another counter.
 */
class TuyaEnergy final : public Device {
    std::uint64_t start_wh_{};
    bool total_{};
    bool started_{};

  public:
    TuyaEnergy() : total_(tuya::read_energy().total) {}

#if USE_ENERGY_COUNTER_FILTER
    /** @brief As Tasmota, the MCU's total goes through EnergyUpdateTotal: SetOption72 takes it
     * as the total, within the SetOption39 bandwidth.
     */
    bool source_total_counter() const override { return total_; }
#endif

    bool calibrate(const Calibration& calibration) override {
        return !calibration.power && !calibration.voltage && !calibration.current &&
               !calibration.frequency;
    }

    bool set_options(const Options& options) override {
        return !options.flags && options.invalid_power_frames == 128;
    }

    void poll(std::uint32_t, Sample* samples, std::size_t count) override {
        if (!count)
            return;
        const auto reading = tuya::read_energy();
        Sample sample{};
        // As Tasmota, the values are shown from start, 0 until the MCU reports them.
        sample.valid = true;
        sample.power = reading.power_mw;
        sample.voltage = reading.voltage_mv;
        sample.current = reading.current_ma;
        sample.available_fields = static_cast<std::uint8_t>(
            4U | (reading.voltage ? 1U : 0U) | (reading.current ? 2U : 0U));
        if (total_) {
            sample.use_absolute = true; // The service must not also integrate the power.
            if (reading.total_fresh) {
                if (started_ && reading.total_wh > start_wh_) {
                    sample.import_increment = (reading.total_wh - start_wh_) * 1000000U;
                    sample.increment_fields = 1;
                }
                start_wh_ = reading.total_wh;
                started_ = true;
            }
#if USE_ENERGY_HARDWARE_TOTAL
            sample.import_reference_valid = reading.total_fresh;
            sample.import_reference = {reading.total_wh, 0};
#endif
        }
        samples[0] = sample;
    }
};
} // namespace

/** @brief Creates the TuyaMCU provider: one channel, no pins, bus or calibration.
 * @param config Retained identity and options.
 * @return Provider, or an empty pointer for an invalid configuration or allocation.
 */
std::unique_ptr<Device> create_tuya_energy(const Configuration& config) {
    if (config.channels != 1 || config.identity.has_resources() || config.identity.bus ||
        config.identity.address || config.options.flags ||
        config.options.invalid_power_frames != 128 || !tuya::energy_mapped())
        return {};
    return std::unique_ptr<Device>(new (std::nothrow) TuyaEnergy());
}
} // namespace newmota::energy
