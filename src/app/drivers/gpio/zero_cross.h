/*
  zero_cross.h - Shared relay timing from captured zero-cross edges

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file zero_cross.h
 * @brief Coordinates existing relay GPIO writes without transferring output ownership.
 */
#pragma once
#include "core/hardware/hardware.h"
#include <cstddef>
#include <cstdint>

namespace newmota::zero_cross {
/** @brief Generation-safe registration for one source and up to two existing relay pads. */
struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Observable capture/timing quality; failed timing falls back to an ordinary relay write.
 */
struct Status {
    bool fresh{};
    std::uint32_t period_us{}, missed{}, lateness_us{};
};

/** @brief Records the relay hardware map before energy devices bind timing routes. */
void initialize(const Hardware& hardware);
/** @brief Claims a CHANGE/pull-up capture, with unique mappings to configured relay GPIO pads.
 * @param compensation_us Chip calculation plus mechanical switching compensation.
 * @return true when this source and its relay map were acquired; no relay output is changed.
 */
bool open(PinId input, const PinId* relays, std::size_t count, std::uint32_t compensation_us,
    Handle& handle);
/** @brief Removes timing mappings after confirmed capture cleanup; failed cleanup quarantines them.
 */
void close(Handle handle);
/** @brief Reports whether capture has a plausible half-cycle and remains fresh. */
Status status(Handle handle);

/** @brief Groups nested relay writes into one timing window per captured source.
 * The first actual write synchronizes; later writes sharing that source run in
 * the same window. Destruction records the duration even on an early error return.
 * Independent source mappings retain their own timing windows.
 */
class WriteGroup {
  public:
    WriteGroup();
    ~WriteGroup();
    WriteGroup(const WriteGroup&) = delete;
    WriteGroup& operator=(const WriteGroup&) = delete;
};

/** @brief Ends grouped timing windows before an intentional relay-policy wait.
 * A following write can synchronize again without leaving the enclosing operation.
 */
void next_window();

/** @brief Waits at most one half-cycle before an existing BasicIo relay write.
 * Returns the matching registration and records its execution-start timestamp.
 * Stale/invalid capture returns immediately and increments that route's missed count.
 */
Handle begin(PinId relay);
/** @brief Learns half the GPIO operation duration, matching upstream code-offset compensation. */
void end(Handle handle);
} // namespace newmota::zero_cross
