/*
  edge_capture.h - Portable timestamped GPIO edge capture

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture.h
 * @brief Portable timestamped GPIO edge capture.
 */

#pragma once
#include "newmota/platform/gpio.h"
#include <cstdint>

namespace newmota::platform::edge_capture {
/** @brief Native capture outcomes; failed cleanup retains ownership until restart. */
enum class Result : std::uint8_t { success, invalid_argument, unsupported, busy, failed };
/** @brief Signal transition counted by a native capture slot. */
enum class Edge : std::uint8_t { rising, falling, both };

/** @brief Pin and timing policy for one exclusive native input capture. */
struct Configuration {
    PinId pin{};
    Edge edge{Edge::falling};
    GpioPull pull{GpioPull::none};
    std::uint32_t minimum_interval_us{};
    /** @brief Zero accepts any 32-bit period; longer gaps establish a new baseline without a
     * period. */
    std::uint32_t maximum_interval_us{};
};

/** @brief Generation-safe native capture slot identifier. */
struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Cumulative capture; callers difference snapshots without resetting IRQ state.
 *
 * count, period_count and summed_period_us wrap modulo 2^32. Callers take unsigned
 * differences between snapshots read less than one wrap apart.
 */
struct Snapshot {
    std::uint32_t count{};
    /** @brief Count of measured intervals; excludes first edges and configured idle gaps. */
    std::uint32_t period_count{};
    std::uint32_t summed_period_us{};
    std::uint64_t last_edge_us{};
    /** @brief First bounded periods inside the armed window, retained until explicitly rearmed. */
    std::uint64_t window_sum_us{};
    std::uint16_t window_count{};
    std::uint32_t last_period_us{};
    bool overflow{}, seen{};
};

/** @brief Absolute-time bounded capture window; zero limit disables collection. */
struct Window {
    std::uint64_t begin_us{}, end_us{};
    std::uint16_t limit{};
};

/** @brief Atomically rearms first-period collection without resetting cumulative counters.
 * @param handle Healthy capture owner.
 * @param configuration Inclusive start, exclusive end and maximum retained periods.
 * @return success when armed; invalid_argument for stale handles or an invalid nonempty window.
 */
Result window(Handle handle, const Window& configuration);

/** @brief Returns a native monotonic microsecond timestamp, never a millisecond approximation.
 * @return Monotonic microseconds from the platform capture clock.
 */
std::uint64_t monotonic_microseconds();
/** @brief Opens input capture after the common owner has reserved the physical pin.
 * @param configuration Pin, edge, pull, and minimum interval settings.
 * @param handle Receives the generation-safe handle on success.
 * @return Capture result; failed cleanup keeps the native slot reserved.
 */
Result open(const Configuration& configuration, Handle& handle);
/** @brief Copies all multiword counters atomically relative to native capture interrupts.
 * @param handle Open capture slot.
 * @param output Receives a coherent cumulative snapshot.
 * @return success when the handle is active, otherwise invalid_argument.
 */
Result snapshot(Handle handle, Snapshot& output);
/** @brief Disables capture and completes callback cleanup before returning success.
 * @param handle Open capture slot.
 * @return success when native resources are released; failure retains the slot.
 */
Result close(Handle handle);
} // namespace newmota::platform::edge_capture
