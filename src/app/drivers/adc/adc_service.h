/*
  adc_service.h - Fair configured ADC sampling owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc_service.h
 * @brief Fair configured ADC sampling owner.
 */

#pragma once
#include "newmota/platform/adc.h"
#include <cstddef>

namespace newmota::adc {
using platform::adc::Reference;
using platform::adc::Result;
/** @brief Global configured-input bound; no samples or driver objects are allocated per feature. */
inline constexpr std::size_t kMaximumInputs = 16;

/** @brief Generation-safe exclusively owned ADC pad. */
struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Coherent completed 32-sample window; timestamps wrap in milliseconds. */
struct Snapshot {
    std::uint32_t millivolts{}, raw{}, completed_at{}, sequence{}, errors{};
    Reference reference{Reference::nominal};
    bool valid{};
};

/** @brief Opens one input after reserving its runtime pad.
 * @param pin Native ADC-capable physical pad.
 * @param handle Receives the retained generation on success or quarantined failure.
 * @return Native route/claim result; failed cleanup retains ownership.
 */
Result open(PinId pin, Handle& handle);
/** @brief Cancels pending sampling and releases a pad only after confirmed native cleanup. */
Result close(Handle handle);
/** @brief Returns the last completed floor-mean window, never a partial accumulation. */
Result snapshot(Handle handle, Snapshot& output);
/** @brief Advances one fair-scheduled native conversion, with >=1 ms between starts.
 * @param now Wrapping monotonic application milliseconds.
 * Each window averages 32 calibrated millivolt readings before provider scaling.
 * A conversion timeout after 10 ms invalidates its window; cleanup failure quarantines it.
 */
void poll(std::uint32_t now);
} // namespace newmota::adc
