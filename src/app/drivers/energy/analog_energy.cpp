/*
  analog_energy.cpp - Portable voltage/current ADC energy provider

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Equations and per-pin configuration adapted from Tasmota xsns_02_analog.ino.
  Copyright (C) 2021 Theo Arends. Upstream distributed under GPL version 3 or later,
  without any warranty.
*/

/** @file analog_energy.cpp
 * @brief Portable voltage/current ADC energy provider.
 */

#include "drivers/adc/adc_service.h"
#include "services/energy/energy_service.h"
#if USE_BERRY_ENERGY_API
#include "services/energy/energy_observation_age.h"
#endif
#include "core/command/command.h"
#include "support/number.h"
#include "support/text.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <string>

namespace newmota::energy {
namespace {
constexpr unsigned voltage_base = static_cast<unsigned>(PinFunction::adc_voltage1);
constexpr unsigned current_base = static_cast<unsigned>(PinFunction::adc_current1);
using Parameters = std::array<std::int32_t, 4>;
constexpr Parameters defaults{0, 4095, 0, 33000};

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

std::string_view token(std::string_view& text, char separator) {
    const auto at = text.find(separator);
    const auto result = trim(text.substr(0, at));
    if (at == text.npos)
        text = {};
    else
        text.remove_prefix(at + 1);
    return result;
}

bool integer(std::string_view text, std::int32_t& output) {
    return support::parse_signed_decimal(trim(text), INT32_MIN, INT32_MAX, output);
}

/** @brief Parses engineering endpoints at source precision, truncating extra decimal digits. */
bool fixed4(std::string_view text, std::int32_t& output) {
    text = trim(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty())
        return false;
    std::uint64_t whole{}, fraction{};
    unsigned digits{};
    bool decimal{}, any{};
    for (const char c : text) {
        if (c == '.' && !decimal) {
            decimal = true;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        any = true;
        if (!decimal) {
            whole = whole * 10 + c - '0';
            if (whole > 214748)
                return false;
        } else if (digits < 4) {
            fraction = fraction * 10 + c - '0';
            ++digits;
        }
    }
    while (digits++ < 4)
        fraction *= 10;
    const auto value = whole * 10000 + fraction;
    if (!any || value > std::uint64_t(INT32_MAX) + (negative ? 1 : 0))
        return false;
    output = static_cast<std::int32_t>(negative ? -static_cast<std::int64_t>(value) : value);
    return true;
}

bool valid(const Parameters& value) {
    return value[0] != value[1];
}

/** @brief A configured pin owns its sampling handle and calibration, never another instance's pad.
 */
struct Input {
    PinId pin{};
    adc::Handle handle{};
    Parameters parameters{defaults};
    adc::Snapshot snapshot{};
    unsigned phase{};
    bool voltage{}, opened{};
};

using Settings = std::array<Parameters, PhysicalIdentity::kMaximumPins>;

/** @brief DC metrology with source-compatible one-second deca-microWh quantization. */
class AnalogEnergy final : public Device {
  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies a configured phase's source-calculation age without sampling or ticking. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!observation_ages_ || channel >= count_)
            return false;
        output = observation_ages_[channel].value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source provider's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    explicit AnalogEnergy(const Configuration& config) : count_(config.channels) {
        for (const auto& binding : config.identity.pins) {
            if (binding.function == PinFunction::none)
                continue;
            const auto role = static_cast<unsigned>(binding.function);
            const bool voltage = role >= voltage_base && role < voltage_base + 7;
            if (!voltage && !(role >= current_base && role < current_base + 7))
                return;
            auto& input = inputs_[size_++];
            input.pin = binding.pin;
            input.voltage = voltage;
            input.phase = role - (voltage ? voltage_base : current_base);
            if (voltage)
                voltage_mask_ |= 1U << input.phase;
            else
                current_mask_ |= 1U << input.phase;
        }
        if (!size_ || !voltage_mask_ || !current_mask_)
            return;
        const auto largest = std::max(voltage_mask_, current_mask_);
        if (count_ > 7 || largest != (1U << count_) - 1 || (voltage_mask_ & (voltage_mask_ + 1)) ||
            (current_mask_ & (current_mask_ + 1)))
            return;
#if USE_BERRY_ENERGY_API
        /** @brief Allocation failure leaves age unknown without changing ADC metrology startup. */
        observation_ages_.reset(new (std::nothrow) ObservationAge[count_]);
#endif
        std::sort(inputs_.begin(), inputs_.begin() + size_,
            [](const Input& a, const Input& b) { return a.pin.value() < b.pin.value(); });
        if (!apply_profile(config.modbus_profile))
            return;
        for (std::size_t i = 0; i < size_; ++i) {
            auto& input = inputs_[i];
            const auto result = adc::open(input.pin, input.handle);
            input.opened = input.handle.generation != 0;
            if (result != adc::Result::success)
                return;
        }
        ready_ = true;
    }

    ~AnalogEnergy() override {
        for (auto& input : inputs_)
            if (input.opened)
                adc::close(input.handle);
    }

    bool ready() const { return ready_; }

    /** @brief The upstream analog driver calibrates per pin, not through P/V/I coefficients. */
    bool calibrate(const Calibration&) override { return false; }

    bool set_options(const Options& options) override { return !options.flags; }

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
#if USE_BERRY_ENERGY_API
        if (observation_ages_)
            for (std::size_t phase = 0; phase < count_; ++phase)
                (void)observation_ages_[phase].poll(now, false);
#endif
        std::array<long double, 7> voltages{}, currents{};
        unsigned healthy_voltages{}, healthy_currents{};
        for (std::size_t i = 0; i < size_; ++i) {
            auto& input = inputs_[i];
            if (adc::snapshot(input.handle, input.snapshot) != adc::Result::success ||
                !input.snapshot.valid || now - input.snapshot.completed_at > 2000) {
                continue;
            }
            // Match upstream floor-average millivolts, then normalize to its 12-bit 3.3-V scale.
            const auto normalized =
                static_cast<std::uint32_t>(input.snapshot.millivolts / 3300.0f * 4095.0f);
            const auto& p = input.parameters;
            const long double engineering = (static_cast<long double>(p[1]) - normalized) /
                                                (static_cast<long double>(p[1]) - p[0]) *
                                                (static_cast<long double>(p[2]) - p[3]) +
                                            p[3];
            const long double milli = std::max(0.0L, engineering / 10);
            if (!std::isfinite(milli) || milli >= 0x1p63L) {
                continue;
            }
            (input.voltage ? voltages : currents)[input.phase] = milli;
            (input.voltage ? healthy_voltages : healthy_currents) |= 1U << input.phase;
        }
        const bool due = timed_ && now - second_at_ >= 1000;
        if (!timed_ || due) {
            second_at_ = now;
            timed_ = true;
        }
        for (std::size_t phase = 0; phase < count && phase < count_; ++phase) {
            auto& sample = output[phase];
            sample = {};
            sample.use_absolute = true;
            sample.auxiliary_fields = 1U << 3;
            sample.direct_current = true;
            const auto voltage = voltages[voltage_mask_ == current_mask_ ? phase : 0];
            const auto reported_voltage_phase = voltage_mask_ == 1 ? 0 : phase;
            const auto current = currents[phase];
            sample.voltage = saturate_i32(voltages[reported_voltage_phase]);
            sample.current = saturate_i32(current);
            const long double power = voltage * current / 1000;
            const auto voltage_phase = voltage_mask_ == current_mask_ ? phase : 0;
            sample.valid =
                ready_ && (healthy_voltages & (1U << voltage_phase)) &&
                (!(current_mask_ & (1U << phase)) || (healthy_currents & (1U << phase))) &&
                power < 0x1p63L;
            sample.available_fields =
                6 | ((healthy_voltages & (1U << reported_voltage_phase)) ? 1 : 0);
#if USE_BERRY_ENERGY_API
            /** @brief AdcEnergyEverySecond renews every phase after its calculation regardless
             * of input availability. Only the existing one-second cadence is a source event;
             * repeated cached ADC snapshots or successful metrology alone cannot renew age.
             */
            if (due && observation_ages_)
                (void)observation_ages_[phase].poll(now, true);
#endif
            if (!sample.valid)
                continue;
            sample.power = static_cast<std::int64_t>(power);
            if (due) {
                // Tasmota adds floor(power_W*1000/36) units of 10 microWh each second.
                sample.import_increment = static_cast<std::uint64_t>(sample.power / 36) * 10;
                sample.increment_fields = 1;
            }
        }
    }

    /** @brief Validates a complete pin-keyed profile without opening or reclaiming any pad. */
    bool apply_profile(std::string_view text) override {
        Settings settings;
        for (auto& value : settings)
            value = defaults;
        std::uint16_t seen{};
        while (!text.empty()) {
            auto entry = token(text, ';');
            std::int32_t pin{};
            if (!integer(token(entry, ','), pin) || pin < 0)
                return false;
            std::size_t index = 0;
            while (index < size_ && inputs_[index].pin.value() != unsigned(pin))
                ++index;
            if (index == size_ || (seen & (1U << index)))
                return false;
            for (unsigned field = 0; field < 4; ++field)
                if (!integer(token(entry, ','), settings[index][field]))
                    return false;
            if (!entry.empty() || !valid(settings[index]))
                return false;
            seen |= 1U << index;
        }
        for (std::size_t i = 0; i < size_; ++i)
            inputs_[i].parameters = settings[i];
        return true;
    }

    /** @brief Maps instance-routed AdcGpio/AdcParam commands to a complete durable profile. */
    bool update_profile(std::string_view payload, std::string& profile) const override {
        auto name = token(payload, ' ');
        bool ordinal = false;
        constexpr std::string_view gpio = "AdcGpio", param = "AdcParam";
        std::string_view index_text;
        if (support::ascii_iequals(name.substr(0, gpio.size()), gpio))
            index_text = name.substr(gpio.size());
        else if (support::ascii_iequals(name.substr(0, param.size()), param)) {
            index_text = name.substr(param.size());
            ordinal = true;
        } else
            return false;
        std::uint32_t number{};
        if (!support::parse_unsigned_decimal(index_text, 65535, number))
            return false;
        std::size_t index = 0;
        if (ordinal) {
            if (!number || number > size_)
                return false;
            index = number - 1;
        } else {
            while (index < size_ && inputs_[index].pin.value() != number)
                ++index;
            if (index == size_)
                return false;
        }
        Parameters updated = defaults;
        payload = trim(payload);
        if (payload != "1") {
            const auto separators = std::count(payload.begin(), payload.end(), ',');
            // Tasmota's AdcParam includes the ignored selector; accept four direct endpoints too.
            if (ordinal && separators == 4 && token(payload, ',') != "1")
                return false;
            else if (separators < 2 || separators > 3 + unsigned(ordinal))
                return false;
            for (unsigned field = 0; field < 4; ++field) {
                if (field == 3 && payload.empty()) {
                    updated[field] = 0;
                    break;
                }
                const auto value = token(payload, ',');
                if (!(field < 2 ? integer(value, updated[field]) : fixed4(value, updated[field])))
                    return false;
            }
            if (!payload.empty() || !valid(updated))
                return false;
        }
        profile.clear();
        for (std::size_t i = 0; i < size_; ++i) {
            if (i)
                profile += ';';
            profile += std::to_string(inputs_[i].pin.value());
            for (const auto value : i == index ? updated : inputs_[i].parameters)
                profile += ',' + std::to_string(value);
        }
        return profile.size() <= Configuration::kMaximumModbusProfile;
    }

    bool write_registers(CommandOutput& output) const override {
        if (!output.write("{\"AnalogEnergy\":{\"Pins\":["))
            return false;
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& input = inputs_[i];
            const auto& p = input.parameters;
            if (i && !output.write(","))
                return false;
            if (!output.format("{\"Pin\":%u,\"Role\":\"%c%u\",\"Parameters\":[%ld,%ld,%ld,%ld]",
                    input.pin.value(), input.voltage ? 'v' : 'i', input.phase + 1, long(p[0]),
                    long(p[1]), long(p[2]), long(p[3])))
                return false;
            if (!output.format(",\"Millivolts\":%u,\"Raw\":%u,\"Reference\":\"%s\",\"Errors\":%u}",
                    input.snapshot.millivolts, input.snapshot.raw,
                    input.snapshot.reference == adc::Reference::sdk_calibrated ? "sdk" : "nominal",
                    input.snapshot.errors))
                return false;
        }
        return output.write("]}}");
    }

  private:
#if USE_BERRY_ENERGY_API
    /** @brief Reporting-only ages allocated for configured phases, never sample validity inputs. */
    std::unique_ptr<ObservationAge[]> observation_ages_;
#endif
    std::array<Input, PhysicalIdentity::kMaximumPins> inputs_{};
    std::size_t size_{}, count_{};
    unsigned voltage_mask_{}, current_mask_{};
    std::uint32_t second_at_{};
    bool ready_{}, timed_{};
};
} // namespace

/** @brief Opens voltage/current ADC routes with independent persistent per-pin endpoints.
 * @param configuration Identity roles v1..v7/i1..i7, channel count and retained profile.
 * @return Configured provider, or nullptr for invalid topology, conflicting pads or absent ADC
 * routes.
 */
std::unique_ptr<Device> create_analog_energy(const Configuration& configuration) {
    if (configuration.identity.bus || configuration.identity.address || configuration.options.flags)
        return {};
    auto device = std::unique_ptr<AnalogEnergy>(new (std::nothrow) AnalogEnergy(configuration));
    return device && device->ready() ? std::move(device) : nullptr;
}
} // namespace newmota::energy
