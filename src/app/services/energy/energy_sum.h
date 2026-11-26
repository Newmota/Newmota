/*
  energy_sum.h - Bounded exact multi-channel energy sums

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_sum.h
 * @brief Exact signed aggregation without target-specific int128 or transient cancellation
 * overflow.
 */
#pragma once
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "support/number.h"

namespace newmota::energy::detail {
/** @brief Writes one normalized decimal, rounding only the display with ties away from zero.
 * @param output Bounded writer.
 * @param minus Preserved original sign, including a nonzero value rounding to displayed zero.
 * @param whole Whole-unit magnitude.
 * @param fraction Fractional magnitude with source_places decimal digits.
 * @param source_places Precision of the exact input, at most nine.
 * @param places Requested output precision, no greater than source_places.
 * @return false on invalid precision or incomplete output. Whole-unit carry never saturates.
 */
inline bool write_decimal(CommandOutput& output, bool minus, std::uint64_t whole,
    std::uint64_t fraction, unsigned source_places, unsigned places) {
    if (places > source_places || source_places > 9)
        return false;
    std::uint64_t divisor = 1, limit = 1;
    for (unsigned i = 0; i < source_places - places; ++i)
        divisor *= 10;
    for (unsigned i = 0; i < places; ++i)
        limit *= 10;
    if (fraction >= divisor * limit)
        return false;
    fraction = divisor == 1 ? fraction : (fraction + divisor / 2) / divisor;
    bool overflow = false;
    if (fraction == limit) {
        fraction = 0;
        overflow = whole == UINT64_MAX;
        ++whole;
    }
    if (!output.write(minus ? "-" : "") ||
        !(overflow ? output.write("18446744073709551616")
                   : output.write(newmota::support::decimal(std::uint64_t(whole)).text)))
        return false;
    return !places || output.format(".%0*lu", int(places), static_cast<unsigned long>(fraction));
}

/** @brief Bounded wide magnitude for at most 64 native uint64-Wh magnitudes; no target int128
 * needed.
 */
struct WideMagnitude {
    std::uint64_t wh{};
    std::uint32_t upper{}, micro{};

    /** @brief Carries whole-Wh overflow into the bounded high limb. */
    void add(EnergyValue value) {
        const auto fraction = micro + value.micro_wh;
        const auto next = wh + value.wh;
        upper += next < wh;
        wh = next + fraction / 1000000;
        upper += wh < next;
        micro = fraction % 1000000;
    }
};

/** @brief Sums signs separately, then subtracts once so cancellation cannot overflow transiently.
 */
struct EnergySum {
    WideMagnitude positive{}, negative{};

    /** @brief Accumulates positive and negative magnitudes without transient cancellation overflow.
     */
    void add(const SignedEnergy& value) {
        (value.negative ? negative : positive).add(value.magnitude);
    }

    /** @brief Extracts a normalized signed uint64-Wh value, failing instead of narrowing high bits.
     */
    bool value(SignedEnergy& output) const {
        const bool minus = negative.upper > positive.upper ||
                           (negative.upper == positive.upper &&
                               (negative.wh > positive.wh || (negative.wh == positive.wh &&
                                                                 negative.micro > positive.micro)));
        const auto high = minus ? negative : positive, low = minus ? positive : negative;
        const auto borrow = high.micro < low.micro ? 1U : 0U;
        const auto difference = high.wh - low.wh;
        const auto upper =
            high.upper - low.upper - (high.wh < low.wh ? 1U : 0U) - (difference < borrow ? 1U : 0U);
        if (upper)
            return false;
        output = {EnergyValue{difference - borrow,
                      std::uint32_t(high.micro + borrow * 1000000 - low.micro)},
            minus};
        return true;
    }

    /** @brief Emits the normalized sum divided by 1000 using portable 32-bit limb division.
     * Energy inputs in Wh produce kWh; integer mW projection inputs produce W. The caller
     * supplies one consistent unit for every add operation.
     * @param output Bounded numeric output destination.
     * @param places Requested decimals, zero through nine; only the final display is rounded.
     * @return false on invalid precision or incomplete output.
     */
    bool write(CommandOutput& output, unsigned places = 9) const {
        const bool minus = negative.upper > positive.upper ||
                           (negative.upper == positive.upper &&
                               (negative.wh > positive.wh || (negative.wh == positive.wh &&
                                                                 negative.micro > positive.micro)));
        const auto high = minus ? negative : positive, low = minus ? positive : negative;
        const auto borrow = high.micro < low.micro ? 1U : 0U;
        const auto difference = high.wh - low.wh;
        const auto wh = difference - borrow;
        const auto upper =
            high.upper - low.upper - (high.wh < low.wh ? 1U : 0U) - (difference < borrow ? 1U : 0U);
        // Three base-2^32 limbs divided by 1000 yield a uint64 kWh quotient for <=64 bounded
        // magnitudes.
        const auto first = (std::uint64_t(upper) << 32) | (wh >> 32);
        const auto second = ((first % 1000) << 32) | (wh & UINT32_MAX);
        const auto whole = ((first / 1000) << 32) | (second / 1000);
        const auto fraction = (second % 1000) * 1000000 + high.micro + borrow * 1000000 - low.micro;
        return write_decimal(output, minus, whole, fraction, 9, places);
    }
};

} // namespace newmota::energy::detail
