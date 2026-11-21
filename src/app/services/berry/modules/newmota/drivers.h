/*
  drivers.h - Tasmota-style Berry driver hooks

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/**
 * @file drivers.h
 * @brief `newmota.add_driver()` instances and the native events delivered to their methods.
 *
 * Native owners only record events here; the Berry poll calls the hooks later, so a hook never
 * runs inside a button, switch or Power operation and cannot cancel it.
 */

#pragma once

#include <cstddef>
#include <cstdint>

struct bvm;

namespace newmota {

/** @brief Hook methods, in dispatch priority order after the first four tick hooks.
 * The first four bits match ScheduledTick, so a tick mask maps to hooks unchanged.
 */
enum class BerryDriverHook : std::uint8_t {
    every_50ms,
    every_100ms,
    every_250ms,
    every_second,
    any_key,
    switch_action, ///< Newmota extension: detached SetOption114 switch Actions.
    button_pressed,
    button_multi_pressed,
    set_power_handler,
    network_up,
    network_down,
    audio, ///< Tasmota's audio(cmd, idx, payload): I2S power and rate per direction.
    count,
};

/** @brief The `cmd` and `payload` strings an event may carry, as Tasmota's event subtypes.
 * Words are added with the first event that needs them.
 */
enum class BerryDriverWord : std::uint8_t {
    none, ///< Empty string.
    power,
    rate,
    tx,
    rx,
};

/** @brief One pending hook call; @p idx is Tasmota's packed integer argument. */
struct BerryDriverEvent final {
    BerryDriverHook hook{};
    BerryDriverWord cmd{};
    BerryDriverWord payload{};
    std::uint32_t idx{};
};

/** @brief Records tick hooks and network changes once per main-loop pass.
 * A Power change that no operation reported, such as Light switching itself off, is queued
 * here too.
 * @param ticks ScheduledTick bits of this pass.
 * @param power Logical state of all Power endpoints, bit 0 for Power1.
 * @param online Whether the station or Ethernet owns an address.
 */
void observe_berry_drivers(std::uint8_t ticks, std::uint32_t power, bool online);
/** @brief Queues `set_power_handler` for one Power operation, changed or not, as Tasmota's
 * SetDevicePower does.
 * @param power Logical state of all Power endpoints after the operation.
 */
void report_berry_driver_power(std::uint32_t power);
/** @brief Queues `audio` events for I2S changes since the last pass: on start of a direction
 * `audio("rate", rate, dir)` then `audio("power", 1, dir)`, on a rate change `rate`, on stop
 * `audio("power", 0, dir)`, with `dir` "tx" or "rx".
 * @param tx Whether an output endpoint is open; @p tx_rate is its sample rate.
 * @param rx Whether an input endpoint is open; @p rx_rate is its sample rate.
 */
void observe_berry_audio(bool tx, std::uint32_t tx_rate, bool rx, std::uint32_t rx_rate);
/** @brief Queues one input event while any driver is registered; overflow drops it. */
void queue_berry_driver_event(BerryDriverHook hook, std::uint32_t idx,
    BerryDriverWord cmd = BerryDriverWord::none, BerryDriverWord payload = BerryDriverWord::none);
/** @brief Reports whether a hook call is pending. */
[[nodiscard]] bool berry_drivers_pending();
/** @brief Limits the following next_berry_driver_event() calls to the events queued now, so
 * events a hook causes, such as its own Power command, wait for the next poll.
 */
void begin_berry_driver_batch();
/** @brief Removes the next pending hook call of the current batch. */
[[nodiscard]] bool next_berry_driver_event(BerryDriverEvent& event);
/** @brief Returns the number of registered drivers, in registration order. */
[[nodiscard]] std::size_t berry_driver_count();
/** @brief Calls one hook of one driver if it has one; arguments: event comptr, slot. */
int dispatch_berry_driver(bvm* vm);
/** @brief Reports whether the tick hooks should be looked up again. */
[[nodiscard]] bool berry_driver_refresh_pending();
/** @brief Looks up which tick hooks the drivers define; protected call, no arguments. */
int refresh_berry_driver_hooks(bvm* vm);
/** @brief Reports whether the last dispatched hook returned true: as in Tasmota, the event then
 * skips the remaining drivers. The native action is never cancelled.
 */
[[nodiscard]] bool berry_driver_done();
/** @brief `newmota.add_driver(instance)`. */
int add_berry_driver(bvm* vm);
/** @brief `newmota.remove_driver(instance)`. */
int remove_berry_driver(bvm* vm);
/** @brief Releases all drivers and pending events before VM destruction. */
void remove_berry_drivers(bvm* vm);

} // namespace newmota
