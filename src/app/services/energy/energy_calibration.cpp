/*
  energy_calibration.cpp - Durable provider and per-channel energy calibration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_calibration.cpp
 * @brief Applies calibrated references with independent channel persistence and rollback.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include <array>

namespace newmota::energy {
namespace {
std::uint32_t* coefficient(Calibration& c, CalibrationField field) {
    switch (field) {
    case CalibrationField::power:
        return &c.power;
    case CalibrationField::voltage:
        return &c.voltage;
    case CalibrationField::current:
        return &c.current;
    case CalibrationField::frequency:
        return &c.frequency;
    }
    return nullptr;
}
} // namespace

bool Service::calibrate(
    std::uint32_t power, std::uint32_t voltage, std::uint32_t current, std::uint32_t frequency) {
    if (storage_fault_)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    const auto old = instance.configuration.calibration;
    Calibration updated{
        power, voltage, current, frequency == UINT32_MAX ? old.frequency : frequency};
    if (instance.device)
        updated = instance.device->canonical_calibration(updated);
    std::array<Calibration, 32> previous{};
    std::array<std::uint8_t, 32> previous_baselines{};
    for (std::size_t c = 0; c < instance.configuration.channels; ++c)
        previous[c] = instance.channels[c].calibration;
    if (!instance.device || !instance.device->calibrate(updated))
        return false;
    for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
        auto value = updated;
        if (frequency == UINT32_MAX && instance.device->independent_calibration()) {
            value.frequency = previous[c].frequency;
            if (!instance.device->calibrate_channel(c, value)) {
                (void)instance.device->calibrate(old);
                for (std::size_t rollback = 0; rollback < instance.configuration.channels;
                    ++rollback) {
                    instance.channels[rollback].calibration = previous[rollback];
                    (void)instance.device->calibrate_channel(rollback, previous[rollback]);
                }
                return false;
            }
        }
        instance.channels[c].calibration = value;
    }
    for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
        auto& account = instance.channels[c].account;
        previous_baselines[c] = account.baseline_fields;
        account.baseline_fields &= ~instance.device->calibration_counter_fields(
            previous[c], instance.channels[c].calibration);
    }
    instance.configuration.calibration = updated;
    dirty_ = true;
    if (save())
        return true;
    instance.configuration.calibration = old;
    (void)instance.device->calibrate(old);
    for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
        instance.channels[c].calibration = previous[c];
        instance.channels[c].account.baseline_fields = previous_baselines[c];
        if (instance.device->independent_calibration())
            (void)instance.device->calibrate_channel(c, previous[c]);
    }
    return false;
}

bool Service::calibrate_channel(std::size_t channel, const Calibration& calibration) {
    if (storage_fault_)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    if (!channel || channel > instance.configuration.channels || !instance.device)
        return false;
    if (!instance.device->independent_calibration()) {
        if (channel != 1 || instance.configuration.channels != 1)
            return false;
        return calibrate(
            calibration.power, calibration.voltage, calibration.current, calibration.frequency);
    }
    const auto resolved = instance.device->canonical_calibration(calibration);
    if (instance.device->shared_voltage_calibration()) {
        std::array<Calibration, 32> updated{};
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            updated[c] = c == channel - 1 ? resolved : instance.channels[c].calibration;
            updated[c].voltage = resolved.voltage;
        }
        return apply_channel_calibrations(updated.data(), instance.configuration.channels);
    }
    const auto old = instance.channels[channel - 1].calibration;
    const auto old_baselines = instance.channels[channel - 1].account.baseline_fields;
    if (!instance.device->calibrate_channel(channel - 1, resolved))
        return false;
    instance.channels[channel - 1].calibration = resolved;
    instance.channels[channel - 1].account.baseline_fields &=
        ~instance.device->calibration_counter_fields(old, resolved);
    dirty_ = true;
    if (save())
        return true;
    instance.channels[channel - 1].calibration = old;
    instance.channels[channel - 1].account.baseline_fields = old_baselines;
    (void)instance.device->calibrate_channel(channel - 1, old);
    return false;
}

bool Service::set_reference(CalibrationField field, std::uint32_t reference, std::size_t channel,
    std::uint32_t reference_divisor) {
    if (storage_fault_ || !reference)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    if (!instance.device || channel > instance.configuration.channels || !reference_divisor)
        return false;
    const auto scale =
        field == CalibrationField::current ? instance.device->current_reference_scale() : 1;
    if (reference_divisor > scale || scale % reference_divisor ||
        reference > UINT32_MAX / (scale / reference_divisor))
        return false;
    reference *= scale / reference_divisor;
    if (!channel) {
        if (!instance.device->independent_calibration()) {
            const auto request = instance.device->request_calibration(field, reference);
            if (request == CalibrationRequest::queued)
                return true;
            if (request == CalibrationRequest::rejected)
                return false;
            auto updated = instance.configuration.calibration;
            if (!instance.device->calibration_for(field, reference, updated))
                return false;
            return calibrate(updated.power, updated.voltage, updated.current, updated.frequency);
        }
        std::array<Calibration, 32> updated{};
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            updated[c] = instance.channels[c].calibration;
            if (!instance.device->calibration_for_channel(c, field, reference, updated[c]))
                return false;
        }
        return apply_channel_calibrations(updated.data(), instance.configuration.channels);
    }
    if (channel == 1) {
        const auto request = instance.device->request_calibration(field, reference);
        if (request == CalibrationRequest::queued)
            return true;
        if (request == CalibrationRequest::rejected)
            return false;
    }
    auto updated = instance.device->independent_calibration()
                       ? instance.channels[channel - 1].calibration
                       : instance.configuration.calibration;
    if (!instance.device->calibration_for_channel(channel - 1, field, reference, updated))
        return false;
    return calibrate_channel(channel, updated);
}

bool Service::calibration_value(
    std::size_t channel, CalibrationField field, std::uint32_t& value) const {
    const auto& instance = provider_;
    if (!instance.device || !channel || channel > instance.configuration.channels)
        return false;
    auto c = instance.device->independent_calibration() ? instance.channels[channel - 1].calibration
                                                        : instance.configuration.calibration;
    if (field == CalibrationField::frequency && !instance.device->default_coefficient(field) &&
        !instance.device->signed_calibration())
        return false;
    const auto* stored = coefficient(c, field);
    if (!stored)
        return false;
    value = *stored || instance.device->signed_calibration()
                ? *stored
                : instance.device->default_coefficient(field);
    return true;
}

bool Service::set_coefficient(std::size_t channel, CalibrationField field, std::uint32_t value) {
    if ((!value && !signed_calibration()))
        return false;
    auto& instance = provider_;
    if (!instance.device || !channel || channel > instance.configuration.channels)
        return false;
    auto c = instance.device->independent_calibration() ? instance.channels[channel - 1].calibration
                                                        : instance.configuration.calibration;
    auto* target = coefficient(c, field);
    if (!target)
        return false;
    if (value == 1 && !instance.device->signed_calibration()) {
        if (!instance.device->coefficient_reset_alias())
            return true;
        value = instance.device->default_coefficient(field);
        if (!value)
            return true; // Drivers without a reset alias treat payload one as a query.
    }
    *target = value;
    return calibrate_channel(channel, c);
}

bool Service::apply_channel_calibrations(const Calibration* values, std::size_t count) {
    if (storage_fault_)
        return false;
    auto& instance = provider_;
    if (!instance.device || !instance.device->independent_calibration() ||
        count != instance.configuration.channels)
        return false;
    std::array<Calibration, 32> canonical{};
    for (std::size_t c = 0; c < count; ++c) {
        canonical[c] = instance.device->canonical_calibration(values[c]);
        if (c && instance.device->shared_voltage_calibration() &&
            canonical[c].voltage != canonical[0].voltage)
            return false;
    }
    values = canonical.data();
    std::array<Calibration, 32> old{};
    std::array<std::uint8_t, 32> old_baselines{};
    for (std::size_t c = 0; c < count; ++c)
        old[c] = instance.channels[c].calibration;
    for (std::size_t c = 0; c < count; ++c)
        if (!instance.device->calibrate_channel(c, values[c])) {
            for (std::size_t rollback = 0; rollback < c; ++rollback)
                (void)instance.device->calibrate_channel(rollback, old[rollback]);
            return false;
        }
    for (std::size_t c = 0; c < count; ++c) {
        instance.channels[c].calibration = values[c];
        auto& baseline = instance.channels[c].account.baseline_fields;
        old_baselines[c] = baseline;
        baseline &= ~instance.device->calibration_counter_fields(old[c], values[c]);
    }
    dirty_ = true;
    if (save())
        return true;
    for (std::size_t c = 0; c < count; ++c) {
        instance.channels[c].calibration = old[c];
        instance.channels[c].account.baseline_fields = old_baselines[c];
        (void)instance.device->calibrate_channel(c, old[c]);
    }
    return false;
}

bool Service::set_resistive_reference(
    std::size_t channel, std::uint32_t power, std::uint32_t voltage) {
    if (storage_fault_ || !power || !voltage)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    if (!instance.device || !instance.configuration.channels ||
        channel > instance.configuration.channels)
        return false;
    const bool independent = instance.device->independent_calibration();
    if (!independent && channel > 1)
        return false;
    std::array<Calibration, 32> updated{};
    for (std::size_t c = 0; c < instance.configuration.channels; ++c)
        updated[c] = instance.channels[c].calibration;
    const std::size_t first = channel ? channel - 1 : 0;
    const std::size_t end = channel ? channel : independent ? instance.configuration.channels : 1;
    for (std::size_t c = first; c < end; ++c) {
        const auto& sample = instance.channels[c].sample;
        if (!sample.valid || !(sample.available_fields & 1) || sample.voltage <= 0)
            return false;
        const auto current =
            static_cast<long double>(sample.voltage) * power * 1000 / voltage / voltage;
        const auto active = static_cast<long double>(sample.voltage) * current / 1000;
        const auto reference_current = current * instance.device->current_reference_scale();
        if (current < 1 || current > INT32_MAX || reference_current > UINT32_MAX || active < 1 ||
            active > INT32_MAX)
            return false;
        auto& target = updated[c];
        if (!independent)
            target = instance.configuration.calibration;
        if (!instance.device->calibration_for_channel(c, CalibrationField::current,
                static_cast<std::uint32_t>(reference_current), target) ||
            !instance.device->calibration_for_channel(
                c, CalibrationField::power, static_cast<std::uint32_t>(active), target))
            return false;
    }
    // Derive every independent channel before applying or persisting any coefficient.
    if (!independent)
        return calibrate(
            updated[0].power, updated[0].voltage, updated[0].current, updated[0].frequency);
    return apply_channel_calibrations(updated.data(), instance.configuration.channels);
}

bool Service::write_primary_calibration(CommandOutput& out, CalibrationField field) const {
    const auto count = provider_.configuration.channels;
    std::uint32_t value{};
    if (!calibration_value(1, field, value))
        return false;
    const char* key = field == CalibrationField::power     ? "PowerCal"
                      : field == CalibrationField::voltage ? "VoltageCal"
                      : field == CalibrationField::current ? "CurrentCal"
                                                           : "FrequencyCal";
    if (!out.format("{\"%s\":", key) || (count > 1 && !out.write("[")))
        return false;
    for (std::size_t c = 1; c <= count; ++c) {
        if (!calibration_value(c, field, value) || (c > 1 && !out.write(",")))
            return false;
        if (signed_calibration()
                ? !out.format("%ld", static_cast<long>(static_cast<std::int32_t>(value)))
                : !out.format("%lu", static_cast<unsigned long>(value)))
            return false;
    }
    return (count < 2 || out.write("]")) && out.write("}");
}

std::uint32_t Service::maximum_coefficient(CalibrationField field) const {
    return provider_.device ? provider_.device->maximum_coefficient(field) : 0;
}

bool Service::signed_calibration() const {
    return provider_.device && provider_.device->signed_calibration();
}

std::uint32_t Service::current_reference_scale() const {
    return provider_.device ? provider_.device->current_reference_scale() : 1;
}

bool Service::primary_current_in_amperes() const {
    return provider_.device && provider_.device->primary_current_in_amperes();
}
} // namespace newmota::energy
