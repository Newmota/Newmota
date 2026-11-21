/*
  dummy.cpp - Configurable virtual energy device

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Simulation/command behavior reference: Tasmota xnrg_30_dummy.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file dummy.cpp
 * @brief Configurable virtual energy device.
 */

#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"
#include "newmota/platform/platform.h"
#include <array>
#include <cstdio>
#include <new>

namespace newmota::energy {
namespace {
/** @brief Parses signed decimal engineering units into bounded milli-units without floats. */
bool milli(std::string_view text, std::int32_t& value) {
    text = support::trim_ascii(text);
    bool negative = !text.empty() && text.front() == '-';
    if (negative || (!text.empty() && text.front() == '+'))
        text.remove_prefix(1);
    const auto dot = text.find('.');
    std::uint32_t whole{}, fraction{};
    if (!(dot == 0) &&
        !support::parse_unsigned_decimal(text.substr(0, dot), INT32_MAX / 1000, whole))
        return false;
    if (dot != std::string_view::npos) {
        const auto digits = text.substr(dot + 1);
        if (digits.empty())
            return false;
        for (std::size_t i = 0; i < digits.size(); ++i) {
            if (digits[i] < '0' || digits[i] > '9')
                return false;
            if (i < 3)
                fraction = fraction * 10 + (digits[i] - '0');
        }
        for (auto n = digits.size(); n < 3; ++n)
            fraction *= 10;
    }
    const auto result = std::uint64_t(whole) * 1000 + fraction;
    if (result > INT32_MAX)
        return false;
    value = negative ? -static_cast<std::int32_t>(result) : static_cast<std::int32_t>(result);
    return true;
}

/** @brief Owns configured simulation/override state; injected full samples remain distinct. */
class Dummy final : public Device {
    struct Channel {
        Sample injected{}, published{};
        Calibration calibration{};
        ObservationAge age{};
        std::int32_t power{}, current{};
        bool injected_mode{};
    };

    std::size_t count_;
    std::unique_ptr<Channel[]> channels_;
    std::uint32_t second_at_{}, power_mask_{};
    bool primary_controlled_{}, primary_on_{};

    static std::uint32_t effective(std::uint32_t value, std::uint32_t fallback) {
        return value ? value : fallback;
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies the existing source age without changing watchdog or accounting state. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        /** @brief Newmota's independent sample-injection lane has no source observation clock. */
        if (!primary_controlled_ || !channels_ || channel >= count_ ||
            channels_[channel].injected_mode)
            return false;
        output = channels_[channel].age.value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    /** @brief Declares common voltage without forcing common per-channel coefficients. */
    bool common_voltage() const override { return true; }

    /** @brief Declares the source common-frequency presentation topology. */
    bool common_frequency() const override { return true; }

    /** @brief Keeps every channel's simulation settings independently durable. */
    bool independent_calibration() const override { return true; }

    /** @brief CurrentSet uses hundredths of mA, matching the actual source handler. */
    std::uint32_t current_reference_scale() const override { return 100; }

    explicit Dummy(std::size_t count)
        : count_(count), channels_(new (std::nothrow) Channel[count]) {}

    bool ready() const { return channels_ != nullptr; }

    /** @brief Source coefficients describe engineering values, rather than conversion gains. */
    std::uint32_t default_coefficient(CalibrationField field) const override {
        switch (field) {
        case CalibrationField::power:
            return 10000;
        case CalibrationField::voltage:
            return 24000;
        case CalibrationField::current:
            return 41666;
        case CalibrationField::frequency:
            return 5000;
        }
        return 0;
    }

    /** @brief Applies simulation coefficients. */
    bool calibrate(const Calibration& c) override {
        if (!ready() || c.power > INT32_MAX || c.voltage > INT32_MAX || c.current > INT32_MAX ||
            c.frequency > INT32_MAX)
            return false;
        for (std::size_t i = 0; i < count_; ++i)
            channels_[i].calibration = c;
        return true;
    }

    /** @brief Applies one independently retained channel's coefficients. */
    bool calibrate_channel(std::size_t channel, const Calibration& c) override {
        if (channel >= count_ || c.power > INT32_MAX || c.voltage > INT32_MAX ||
            c.current > INT32_MAX || c.frequency > INT32_MAX)
            return false;
        channels_[channel].calibration = c;
        return true;
    }

    /** @brief Set commands store bounded engineering values without a measured reference. */
    bool calibration_for_channel(std::size_t channel, CalibrationField field,
        std::uint32_t reference, Calibration& output) const override {
        if (channel >= count_)
            return false;
        const auto value = field == CalibrationField::current ? reference : reference / 10;
        switch (field) {
        case CalibrationField::power:
            if (value < 100 || value > 16000000)
                return false;
            output.power = value;
            break;
        case CalibrationField::voltage:
            if (value < 10000 || value > 40000)
                return false;
            output.voltage = value;
            break;
        case CalibrationField::current:
            if (value < 1000 || value > 40000000)
                return false;
            output.current = value;
            break;
        case CalibrationField::frequency:
            if (value < 4500 || value > 6500)
                return false;
            output.frequency = value;
            break;
        }
        return true;
    }

    /** @brief Source global gate and channel Power bits remain independent inputs. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }

    /** @brief Supplies real channel Power bits; SO21 never changes their state. */
    void set_primary_power_mask(std::uint32_t mask) override { power_mask_ = mask; }

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        const bool second = now - second_at_ >= 1000;
        if (second)
            second_at_ = now;
        for (std::size_t i = 0; i < count && i < count_; ++i) {
            auto& state = channels_[i];
            if (!primary_controlled_ || state.injected_mode) {
                output[i] = state.injected; // Newmota full-Sample input lane is independent.
                /** Metrology/absolute snapshots persist, but each supplied confirmed delta
                 * is consumed once. Repeated app polls must not replay injected increments.
                 */
                state.injected.increment_fields = 0;
                state.injected.import_increment = state.injected.export_increment = 0;
                state.injected.pulse_count = 0;
                continue;
            }
            auto& value = state.published;
            const bool on = primary_on_ && (power_mask_ & (std::uint32_t{1} << i));
            (void)state.age.poll(now, second && on);
            if (second && primary_on_) {
                const auto& cal = state.calibration;
                const auto voltage = effective(cal.voltage, 24000);
                const auto frequency = effective(cal.frequency, 5000);
                if (voltage > 10000)
                    value.voltage = saturate_i32(std::uint64_t(voltage) * 10);
                value.frequency_millihz = frequency > 4500 ? std::uint64_t(frequency) * 10 : 0;
                value.auxiliary_fields = frequency > 4500 ? 1 : 0;
                value.watchdog_reset = false;
                if (on) {
                    value.power =
                        state.power ? state.power : std::int64_t(effective(cal.power, 10000)) * 10;
                    value.current = !value.power ? 0
                                    : state.current
                                        ? saturate_i32(state.current)
                                        : saturate_i32(effective(cal.current, 41666) / 100);
                    value.valid = true;
                }
            }
            if (state.age.expired_now()) {
                value.voltage = value.current = value.power = value.frequency_millihz = 0;
                value.auxiliary_fields = 1;
                value.valid = false;
                value.watchdog_reset = true;
            }
            if (state.age.stale())
                value.valid = false;
            value.available_fields = 7;
            value.source_retained = !value.valid && !value.watchdog_reset;
            auto sample = value;
            sample.use_absolute = true;
            /** Only actual one-second channel-ON hooks count: retained OFF power cannot
             * enter Service's previous-sample integration. Source int32 delta units are
             * 10 micro-Wh; truncate each signed addition before routing its direction.
             */
            if (second && on && sample.power) {
                const bool exporting = sample.power < 0;
                const auto magnitude = exporting ? std::uint64_t(-(sample.power + 1)) + 1
                                                 : std::uint64_t(sample.power);
                const auto increment = magnitude / 36 * 10;
                if (exporting)
                    sample.export_increment = increment;
                else
                    sample.import_increment = increment;
                sample.increment_fields = exporting ? 2 : 1;
            }
            output[i] = sample;
        }
    }

    /** @brief Keeps full-Sample input independent of zero-fallback P/I source overrides.
     * Supplied metrology/validity persist; supplied confirmed increments are consumed once.
     */
    bool inject(std::size_t channel, const Sample& sample) override {
        if (channel >= count_)
            return false;
        channels_[channel].injected = sample;
        channels_[channel].injected_mode = true;
        return true;
    }

    /** @brief Applies volatile Current/Power,channel,engineering-value source overrides.
     * Signed values are milli-A/milli-W internally; zero returns to the coefficient value.
     */
    bool configure_registers(std::string_view payload) override {
        const auto first = payload.find(',');
        if (first == std::string_view::npos)
            return false;
        if (!primary_controlled_)
            return false;
        const auto name = support::trim_ascii(payload.substr(0, first));
        payload.remove_prefix(first + 1);
        const auto second = payload.find(',');
        std::uint32_t channel{};
        if (support::ascii_iequals(name, "Source")) {
            if (second != std::string_view::npos ||
                !support::parse_unsigned_decimal(payload, count_, channel) || !channel)
                return false;
            channels_[channel - 1].injected_mode = false;
            return true;
        }
        std::int32_t value{};
        if (second == std::string_view::npos ||
            !support::parse_unsigned_decimal(payload.substr(0, second), count_, channel) ||
            !channel || !milli(payload.substr(second + 1), value))
            return false;
        auto& state = channels_[channel - 1];
        if (support::ascii_iequals(name, "Current"))
            state.current = value;
        else if (support::ascii_iequals(name, "Power"))
            state.power = value;
        else
            return false;
        return true;
    }

    /** @brief Reports volatile source overrides and the separate input-lane selection. */
    bool write_registers(CommandOutput& out) const override {
        if (!out.write("{\"Dummy\":["))
            return false;
        for (std::size_t i = 0; i < count_; ++i) {
            if ((i && !out.write(",")) ||
                !out.format("{\"Channel\":%u,\"Current_mA\":%ld,\"Power_mW\":%ld,\"Injected\":%s}",
                    unsigned(i + 1), long(channels_[i].current), long(channels_[i].power),
                    channels_[i].injected_mode ? "true" : "false"))
                return false;
        }
        return out.write("]}");
    }
};
} // namespace

/** @brief Routes scalar source aliases only for a configured primary Dummy.
 * Physical EnergyConfig tuples and empty configuration queries retain their existing meaning.
 */
CommandStatus Service::dummy_config_command(const CommandRequest& request, CommandOutput& out) {
    auto& primary = provider_;
    if (!primary.device || primary.configuration.identity.driver != 30)
        return CommandStatus::unknown;
    const std::array<std::string_view, 4> names{"Power", "Voltage", "Current", "Frequency"};
    for (std::size_t f = 0; f < names.size(); ++f) {
        const auto field = static_cast<CalibrationField>(f);
        std::array<char, 24> name{};
        std::snprintf(name.data(), name.size(), "%.*sSet", int(names[f].size()), names[f].data());
        const bool set = support::ascii_iequals(request.name, name.data());
        std::snprintf(name.data(), name.size(), "%.*sCal", int(names[f].size()), names[f].data());
        const bool cal = support::ascii_iequals(request.name, name.data());
        if (!set && !cal)
            continue;
        /** The pinned source handles FrequencySet, but has no FrequencyCal switch case. */
        if (cal && field == CalibrationField::frequency)
            return CommandStatus::failed;
        if (set && request.payload.find(',') != std::string_view::npos)
            return CommandStatus::unknown; // Existing shared resistive-load paired Set path.
        const std::size_t channel = request.has_index ? request.index : 1;
        if (channel > primary.configuration.channels || (cal && !channel))
            return CommandStatus::invalid;
        std::uint32_t value{};
        bool update{};
        if (!request.is_query()) {
            if (set) {
                std::int32_t parsed{};
                if (milli(request.payload, parsed)) {
                    value = std::uint32_t(parsed < 0 ? -std::int64_t(parsed) : parsed) / 10;
                    const std::array<std::uint32_t, 4> minimum{100, 10000, 1000, 4500};
                    const std::array<std::uint32_t, 4> maximum{16000000, 40000, 40000000, 6500};
                    update = value >= minimum[f] && value <= maximum[f];
                }
            } else
                update = support::parse_unsigned_decimal(request.payload, INT32_MAX, value) &&
                         value > 99;
        }
        if (update) {
            poll(platform::monotonic_milliseconds());
            std::array<Calibration, 8> values{};
            for (std::size_t c = 0; c < primary.configuration.channels; ++c) {
                values[c] = primary.channels[c].calibration;
                if (channel && c != channel - 1)
                    continue;
                switch (field) {
                case CalibrationField::power:
                    values[c].power = value;
                    break;
                case CalibrationField::voltage:
                    values[c].voltage = value;
                    break;
                case CalibrationField::current:
                    values[c].current = value;
                    break;
                case CalibrationField::frequency:
                    values[c].frequency = value;
                    break;
                }
            }
            if (storage_fault_ ||
                !apply_channel_calibrations(values.data(), primary.configuration.channels))
                return CommandStatus::failed;
        }
        return write_primary_calibration(out, field) ? CommandStatus::handled
                                                     : CommandStatus::failed;
    }
    if (!support::ascii_iequals(request.name, "EnergyConfig") || !request.has_index ||
        request.payload.empty() || request.payload.find(',') != std::string_view::npos)
        return CommandStatus::unknown;
    std::size_t channel{};
    bool power{};
    if (request.index >= 1 && request.index <= 3)
        channel = request.index;
    else if (request.index >= 4 && request.index <= 6) {
        channel = request.index - 3;
        power = true;
    } else if (request.index >= 11 && request.index < 18)
        channel = request.index - 10;
    else if (request.index >= 111 && request.index < 118) {
        channel = request.index - 110;
        power = true;
    } else
        return CommandStatus::unknown;
    if (channel > primary.configuration.channels || request.payload.size() > 24)
        return CommandStatus::invalid;
    std::array<char, 64> payload{};
    const auto size =
        std::snprintf(payload.data(), payload.size(), "%s,%u,%.*s", power ? "Power" : "Current",
            unsigned(channel), int(request.payload.size()), request.payload.data());
    if (size < 0 || std::size_t(size) >= payload.size() ||
        !configure_registers({payload.data(), std::size_t(size)}))
        return CommandStatus::invalid;
    return out.format("{\"EnergyConfig%u\":\"Done\"}", unsigned(request.index))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Creates a virtual simulator with only its configured channels allocated.
 * @param configuration Resource-free identity, channel count and retained calibration.
 * @return Virtual owner, or nullptr when resources/count/allocation are invalid.
 */
std::unique_ptr<Device> create_dummy(const Configuration& configuration) {
    if (configuration.identity.has_resources() || configuration.identity.bus ||
        configuration.identity.address || !configuration.channels || configuration.channels > 8)
        return {};
    auto result = std::unique_ptr<Dummy>(new (std::nothrow) Dummy(configuration.channels));
    if (!result || !result->ready() || !result->calibrate(configuration.calibration))
        return {};
    return result;
}
} // namespace newmota::energy
