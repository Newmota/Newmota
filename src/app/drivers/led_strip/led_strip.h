/*
  led_strip.h - Bounded portable LED strip editing interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.h
 * @brief Bounded portable LED strip editing interface.
 */

#pragma once
#include "newmota/platform/led_strip.h"

namespace newmota::led_strip {
using platform::led_strip::Order;
using platform::led_strip::Pixel;
using platform::led_strip::Result;
inline constexpr std::size_t kCapacity = 4;

struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

struct Config {
    PinId pin{};
    std::size_t count{};
    Order order{Order::grb};
    bool reverse{};
    bool rgbw{};
    bool white_first{};
    bool managed{};
};

/** @brief Opens one strip slot and returns a generation-checked handle.
 * @param config Pin, pixel count, ordering, and ownership options.
 * @param handle Receives the active slot handle on success.
 * @return The native open result.
 */
Result open(const Config&, Handle&);
/** @brief Reports whether a handle still identifies an active strip. */
bool active(Handle);
/** @brief Reports whether a strip is edited through its registered owner. */
bool managed(Handle);
/** @brief Edits one pixel in the owner's managed framebuffer. */
using Editor = Result (*)(void*, std::size_t, Pixel, std::size_t);
/** @brief Receives one notification when a managed strip operation faults. */
using FaultHandler = void (*)(void*);
/** @brief Installs owner callbacks used for managed edits and fault reporting. */
Result set_editor(Handle, void*, Editor, FaultHandler = nullptr);
/** @brief Makes room for more pixels while preserving the handle and the current pixels.
 *
 * Waits for a running frame and restarts the native port. Any failure other than `failed` leaves
 * the strip working at its old size.
 */
Result reserve(Handle, std::size_t count);
/** @brief Reconfigures a strip within its reserved size while preserving the active handle. */
Result configure(Handle, const Config&);
/** @brief Queues a complete frame; accepted wire bytes stay immutable until drained. */
Result frame(Handle, const Pixel*, std::size_t);
/** @brief Returns whether the native frame transfer is still in progress. */
Result busy(Handle, bool&);
/** @brief Changes one pixel in the current frame. */
Result set(Handle, std::size_t index, Pixel);
/** @brief Fills the current frame with one pixel value. */
Result fill(Handle, Pixel);
/** @brief Encodes and submits the current frame to the native adapter. */
Result show(Handle);
/** @brief Closes an idle strip and releases its slot. */
Result close(Handle);
/** @brief Requests close after any active native transfer has drained. */
Result request_close(Handle);
/** @brief Advances strip transfers and delivers deferred fault notifications. */
void poll();
} // namespace newmota::led_strip
