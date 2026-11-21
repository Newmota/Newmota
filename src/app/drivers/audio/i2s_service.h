/*
  i2s_service.h - Shared audio transport ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file i2s_service.h
 * @brief Generation checked audio endpoint leases shared by commands and Berry.
 */
#pragma once
#include "newmota/platform/i2s.h"
#include "core/hardware/hardware.h"

namespace newmota {
/** @brief Retains the resolved I2S template view without opening hardware. */
void initialize_i2s(const Hardware&);
} // namespace newmota

namespace newmota::i2s {
using platform::i2s::Config;
using platform::i2s::Result;
using platform::i2s::Status;
using platform::i2s::Transfer;

struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Resolves a template bus (zero based); does not acquire hardware. */
bool configured_config(Config&, platform::i2s::Direction, std::uint8_t instance = 0);
/** @brief Retains both direction profiles' pads and one exclusive native controller. */
Result reserve(const Config& input, const Config& output, Handle&);
/** @brief Checks profiles against retained pads/controller without changing native hardware.
 * Structural changes outside this reservation require closing the lease first.
 */
Result check_profiles(Handle, const Config& input, const Config& output);
/** @brief Switches the retained lease to one profile after its capture worker has joined.
 * Teardown failure quarantines the lease. Reopen failure retains its reservation.
 */
Result activate(Handle, const Config&);
/** @brief Deletes native channels while retaining controller and pad ownership. */
Result deactivate(Handle);
/** @brief Atomically claims physical pads and a compatible native endpoint. */
Result open(const Config&, Handle&);
/** @brief Stops the endpoint before releasing its claims; aborts queued TX. */
Result close(Handle);
/** @brief Reports native playback drain without waiting or stopping capture. */
bool tx_idle(Handle);
/** @brief Checks the generation and rejects closed or quarantined endpoints. */
bool active(Handle);
/** @brief Consumes immediately available complete frames. One capture worker may read;
 * the application must join it before closing or changing the lease. */
Transfer read(Handle, std::uint8_t*, std::size_t);
/** @brief Enqueues complete frames without waiting. */
Transfer write(Handle, const std::uint8_t*, std::size_t);
/** @brief Returns native transport counters and negotiated rate. */
Status status(Handle);

/** @brief Directions with an open native endpoint and their configured sample rates. */
struct Activity {
    bool tx{}, rx{};
    std::uint32_t tx_rate{}, rx_rate{};
};

/** @brief Reports which directions run now, for the Berry `audio` driver events. */
Activity activity();
/** @brief Runs bounded cooperative backend maintenance. */
void poll();
} // namespace newmota::i2s
