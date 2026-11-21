/*
  newmota_module.h - Berry global module binding for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

struct bvm;

namespace newmota {

class CommandProcessor;
class TimeService;

inline constexpr std::size_t kNewmotaCallbackCapacity = 16;

struct NewmotaPendingCallback {
    std::size_t slot;
    std::uint32_t generation;
};

/** @brief Copies due callback handles into caller-owned bounded storage.
 * @param now Current monotonic time in milliseconds.
 * @param timer_tick Whether this snapshot is being taken for a timer tick.
 * @param due Destination array with at least `kNewmotaCallbackCapacity` entries.
 * @return Number of callback handles copied to @p due.
 */
std::size_t snapshot_newmota_callbacks(
    std::uint32_t now, bool timer_tick, NewmotaPendingCallback* due);
/** @brief Dispatches a copied callback handle through the protected Berry VM. */
int dispatch_newmota_callback(bvm* vm);
/** @brief Dispatches one native timer event through the registered Berry handler. */
int dispatch_newmota_timer_event(bvm* vm);

/** @brief Binds the command processor and exposes the built-in global. */
[[nodiscard]] bool install_newmota_module(
    bvm* vm, CommandProcessor& commands, TimeService* time = nullptr);

/** @brief Removes the command processor binding before VM destruction. */
void remove_newmota_module();

} // namespace newmota
