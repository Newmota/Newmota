/*
  edge_capture_service.h - Exclusive application GPIO edge capture ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture_service.h
 * @brief Exclusive application GPIO edge capture ownership.
 */

#pragma once
#include "newmota/platform/edge_capture.h"

namespace newmota::edges {
using platform::edge_capture::Configuration;
using platform::edge_capture::Handle;
using platform::edge_capture::Result;
using platform::edge_capture::Snapshot;
using platform::edge_capture::Window;
/** @brief Maintains native wrapping clocks even while no capture instance is active.
 * Call once on each cooperative application iteration.
 */
void poll();
/** @brief Claims an unassigned pad and opens its native interrupt capture.
 * @param configuration Requested pin and capture policy.
 * @param handle Receives the generation-safe handle on success.
 * @return Native capture result.
 */
Result open(const Configuration& configuration, Handle& handle);
/** @brief Rearms a bounded period window through the retained native capture owner.
 * @param handle Healthy generation-safe capture.
 * @param configuration Absolute microsecond interval and first-period limit.
 * @return Native capture result; cumulative counters remain unchanged.
 */
Result window(Handle handle, const Window& configuration);
/** @brief Reads a coherent cumulative snapshot from an owned handle.
 * @param handle Open capture handle.
 * @param output Receives the captured edge counters and timestamps.
 * @return Native capture result.
 */
Result snapshot(Handle handle, Snapshot& output);
/** @brief Releases the pad only after confirmed native callback cleanup.
 * @param handle Open capture handle.
 * @return success when the pin claim and native slot are released.
 */
Result close(Handle handle);
} // namespace newmota::edges
