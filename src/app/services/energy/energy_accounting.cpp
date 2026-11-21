/*
  energy_accounting.cpp - Exact whole-Wh and fractional energy arithmetic

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_accounting.cpp
 * @brief Normalized accounting preserves native 64-bit Wh counters and sub-Wh fractions.
 */
#include "services/energy/energy_types.h"

namespace newmota::energy {
bool EnergyValue::add(EnergyValue delta) {
    if (micro_wh >= 1000000 || delta.micro_wh >= 1000000)
        return false;
    const auto fraction = micro_wh + delta.micro_wh;
    const std::uint64_t carry = fraction / 1000000;
    if (delta.wh > UINT64_MAX - wh || carry > UINT64_MAX - wh - delta.wh)
        return false;
    wh += delta.wh + carry;
    micro_wh = fraction % 1000000;
    return true;
}

bool SignedEnergy::add(EnergyValue delta, bool subtract) {
    if (delta.micro_wh >= 1000000 || magnitude.micro_wh >= 1000000)
        return false;
    auto next = *this;
    if (next.negative == subtract) {
        if (!next.magnitude.add(delta))
            return false;
    } else {
        const bool larger = delta.wh > magnitude.wh ||
                            (delta.wh == magnitude.wh && delta.micro_wh > magnitude.micro_wh);
        const auto high = larger ? delta : magnitude;
        const auto low = larger ? magnitude : delta;
        const bool borrow = high.micro_wh < low.micro_wh;
        next.magnitude = {high.wh - low.wh - (borrow ? 1 : 0),
            std::uint32_t(high.micro_wh + (borrow ? 1000000 : 0) - low.micro_wh)};
        if (larger)
            next.negative = subtract;
    }
    if (!next.magnitude.wh && !next.magnitude.micro_wh)
        next.negative = false;
    *this = next;
    return true;
}

bool Account::compatibility_export(SignedEnergy& output) const {
    SignedEnergy next{exported, false};
    if (!next.add(export_adjustment.magnitude, export_adjustment.negative))
        return false;
    output = next;
    return true;
}

bool Account::compatibility_total(SignedEnergy& output) const {
    SignedEnergy next{};
    if (total_fault || !next.add(imported) || !next.add(exported, true) ||
        !next.add(total_adjustment.magnitude, total_adjustment.negative))
        return false;
    output = next;
    return true;
}

bool DirectionalEnergy::add(EnergyValue value, bool exporting) {
    return (exporting ? exported : imported).add(value);
}

namespace {
/** @brief Commits lifetime and confirmed-delta additions together, before modifying carries. */
bool advance(EnergyValue& total, EnergyValue value, bool direction, DirectionalEnergy* confirmed) {
    DirectionalEnergy next{};
    if (confirmed) {
        next = *confirmed;
        if (!next.add(value, direction))
            return false;
    }
    if (!total.add(value))
        return false;
    if (confirmed)
        *confirmed = next;
    return true;
}
} // namespace

bool Account::add_increment(std::uint64_t numerator, std::uint32_t denominator, bool direction,
    DirectionalEnergy* confirmed) {
    if (!denominator)
        return false;
    auto& carry = direction ? export_increment_remainder : import_increment_remainder;
    auto& previous_denominator =
        direction ? export_increment_denominator : import_increment_denominator;
    auto& total = direction ? exported : imported;
    if (!previous_denominator || carry >= previous_denominator)
        return false;
    // A changed coefficient rescales the sub-micro-Wh carry, losing less than one micro-Wh.
    const auto rescaled = std::uint64_t(carry) * denominator / previous_denominator;
    if (numerator > UINT64_MAX - rescaled)
        return false;
    const auto sum = numerator + rescaled, micro = sum / denominator;
    if (!advance(total, {micro / 1000000, std::uint32_t(micro % 1000000)}, direction, confirmed))
        return false;
    carry = sum % denominator;
    previous_denominator = denominator;
    return true;
}

bool Account::integrate(std::int64_t power, std::uint32_t elapsed, DirectionalEnergy* confirmed) {
    const bool direction = power < 0;
    const auto magnitude = direction ? std::uint64_t(-(power + 1)) + 1 : std::uint64_t(power);
    auto& fraction = direction ? export_remainder : import_remainder;
    auto& total = direction ? exported : imported;
    if (elapsed && magnitude > (UINT64_MAX - fraction) / elapsed)
        return false;
    const auto numerator = magnitude * elapsed + fraction, delta = numerator / 3600;
    if (!advance(total, {delta / 1000000, std::uint32_t(delta % 1000000)}, direction, confirmed))
        return false;
    fraction = numerator % 3600;
    return true;
}

bool Account::add_pulses(std::uint32_t count, std::uint64_t scale, DirectionalEnergy* confirmed) {
    if (count && scale > (UINT64_MAX - pulse_remainder) / count)
        return false;
    const auto numerator = std::uint64_t(count) * scale + pulse_remainder,
               delta = numerator / 36000;
    if (!advance(imported, {delta / 1000000, std::uint32_t(delta % 1000000)}, false, confirmed))
        return false;
    pulse_remainder = numerator % 36000;
    return true;
}

bool Account::observe_absolute(
    EnergyValue value, bool direction, bool export_delta, DirectionalEnergy* confirmed) {
    if (value.micro_wh >= 1000000)
        return false;
    auto& previous = direction ? export_baseline : import_baseline;
    auto& total = (direction || export_delta) ? exported : imported;
    auto& resets = direction ? export_resets : import_resets;
    const auto mask = direction ? 2U : 1U;
    if (baseline_fields & mask) {
        if (value.wh < previous.wh ||
            (value.wh == previous.wh && value.micro_wh < previous.micro_wh)) {
            if (resets < UINT32_MAX)
                ++resets;
        } else {
            EnergyValue delta{value.wh - previous.wh, 0};
            if (value.micro_wh < previous.micro_wh) {
                --delta.wh;
                delta.micro_wh = 1000000 + value.micro_wh - previous.micro_wh;
            } else
                delta.micro_wh = value.micro_wh - previous.micro_wh;
            if (!advance(total, delta, direction || export_delta, confirmed))
                return false;
        }
    }
    previous = value;
    baseline_fields |= mask;
    return true;
}
} // namespace newmota::energy
