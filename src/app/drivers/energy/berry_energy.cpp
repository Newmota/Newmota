/*
  berry_energy.cpp - Independently configured Berry virtual energy provider

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Retained-value provider behavior adapted from Tasmota xnrg_28_berry.ino.
  Copyright (C) 2021 Theo Arends
  Upstream distributed under GPL version 3 or later, without any warranty.
*/

/** @file berry_energy.cpp
 * @brief Retains script measurements independently of VM lifetime and accounts each channel once.
 */
#include "services/energy/energy_service.h"
#include "newmota/platform/platform.h"
#include <new>

namespace newmota::energy {
namespace {
/** @brief Runtime allocation contains only configured virtual channels, with no hardware claims. */
class BerryEnergy final : public Device {
    struct Channel {
        Sample sample{};
        std::uint32_t integrated_at{};
        std::uint8_t pending{};
    };

    std::unique_ptr<Channel[]> channels_;
    std::size_t count_{};
    Options options_{};

  public:
#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    /** @brief Matches the source model's shared voltage/frequency topology. */
    bool common_voltage() const override { return true; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return true; }

    explicit BerryEnergy(std::size_t count)
        : channels_(new (std::nothrow) Channel[count]), count_(count) {
        if (channels_)
            for (std::size_t c = 0; c < count_; ++c) {
                channels_[c].sample.valid = true;
                channels_[c].sample.auxiliary_fields = 1 | 2 | (1U << 4) | (1U << 5);
                channels_[c].integrated_at = platform::monotonic_milliseconds();
            }
    }

    bool ready() const { return channels_ != nullptr; }

    bool script_writable() const override { return true; }

    bool calibrate(const Calibration&) override { return true; }

    bool set_options(const Options& value) override {
        if (value.flags & 0x80U || value.invalid_power_frames != 128)
            return false;
        const auto changed_mode = (options_.flags ^ value.flags) & 32;
        options_ = value;
        if (changed_mode && channels_)
            for (std::size_t c = 0; c < count_; ++c) {
                channels_[c].integrated_at = platform::monotonic_milliseconds();
                channels_[c].pending = 0; // Require a fresh counter after changing accounting mode.
            }
        return true;
    }

    std::uint8_t options_counter_fields(
        const Options& before, const Options& after) const override {
        return (before.flags ^ after.flags) & 32 ? 3 : 0;
    }

    /** @brief Replaces a value snapshot; one-shot counters are retained separately from output. */
    bool inject(std::size_t channel, const Sample& value) override {
        if (channel >= count_ || value.use_pulses || value.pulse_count || value.increment_fields ||
            value.import_floor_valid || value.import_counter.micro_wh >= 1000000 ||
            value.export_counter.micro_wh >= 1000000 || value.power == INT64_MIN)
            return false;
        auto& current = channels_[channel];
        current.sample = value;
        current.pending |= value.counter_fields & 3;
        current.sample.counter_fields = 0;
        return true;
    }

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        for (std::size_t c = 0; c < count && c < count_; ++c) {
            auto& current = channels_[c];
            auto sample = current.sample;
            sample.use_absolute = true; // The service must never also integrate retained power.
            sample.available_fields &=
                4 | (options_.flags & 8 ? 1 : 0) | (options_.flags & 16 ? 2 : 0);
            sample.direct_current = options_.flags & 4;
            sample.auxiliary_fields |= 8;
            sample.counter_fields = 0;
            if (options_.flags & 32) {
                if (sample.valid) {
                    sample.counter_fields = current.pending;
                    current.pending = 0;
                }
                sample.import_counter_exporting = (options_.flags & 64) && sample.power < 0;
                current.integrated_at = now;
            } else {
                const auto elapsed = now - current.integrated_at;
                if (!sample.valid)
                    current.integrated_at = now;
                else if (elapsed >= 1000) {
                    // Bound a single multiplication; a longer application pause is an explicit gap.
                    if (elapsed <= 60000 && (sample.available_fields & 4)) {
                        const auto magnitude = sample.power < 0 ? std::uint64_t(-sample.power)
                                                                : std::uint64_t(sample.power);
                        if (!magnitude || magnitude <= UINT64_MAX / elapsed) {
                            sample.increment_denominator = 3600;
                            if (sample.power < 0) {
                                sample.export_increment = magnitude * elapsed;
                                sample.increment_fields = 2;
                            } else {
                                sample.import_increment = magnitude * elapsed;
                                sample.increment_fields = 1;
                            }
                        } else
                            sample.valid = false;
                    }
                    current.integrated_at = now;
                }
            }
            output[c] = sample;
        }
    }
};
} // namespace

/** @brief Creates a selected 1–8-channel script provider without GPIO or bus ownership.
 * @param config Retained virtual identity and independent operating options.
 * @return Allocated provider or an empty pointer for invalid resources/allocation.
 */
std::unique_ptr<Device> create_berry_energy(const Configuration& config) {
    if (!config.channels || config.channels > 8 || config.identity.has_resources() ||
        config.identity.bus || config.identity.address)
        return {};
    auto device = std::unique_ptr<BerryEnergy>(new (std::nothrow) BerryEnergy(config.channels));
    if (!device || !device->ready() || !device->set_options(config.options))
        return {};
    return device;
}
} // namespace newmota::energy
