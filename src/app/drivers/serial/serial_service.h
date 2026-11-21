/*
  serial_service.h - Shared serial service interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file serial_service.h
 * @brief Declares the shared, generation-safe application serial service.
 */

#pragma once
#include "newmota/platform/serial.h"
#include "newmota_features.h"

namespace newmota::serial {
using platform::serial::Config;
using platform::serial::Result;
using platform::serial::Status;
using platform::serial::Transfer;
/** @brief Number of fixed service slots available for native UART ownership. */
inline constexpr std::size_t kPortCount = 4;

/** @brief Names one open service slot and rejects stale handles after slot reuse. */
struct Handle {
    std::uint32_t generation{};
    std::uint8_t slot{};
};

/** @brief Validates and opens an exclusively owned native UART port.
 * @param configuration Native UART settings and pad assignment.
 * @param handle Receives the generation-safe handle on success.
 * @return Native configuration result.
 */
Result open(const Config& configuration, Handle& handle);
/** @brief Opens only matching active configured GPS roles with generation-safe claims.
 * Requires RX/TX, 8N1, no inversion and the RX binding's declared baud.
 */
Result open_gps(const Config& configuration, Handle& handle);
#if USE_TUYA_MCU
/** @brief Opens the template's unique active Tuya Rx/Tx bindings with generation-safe claims. */
Result open_tuya(const Config& configuration, Handle& handle);
#endif
/** @brief Reconfigures an idle UART on the same pads without releasing their claims.
 * A failed reopen attempts the old configuration. Uncertain cleanup or failed
 * restoration quarantines the existing owner and retains every pad reservation.
 * @param handle Healthy retained UART owner.
 * @param configuration New framing/rate with the same RX/TX pads.
 * @return busy until TX drains; success only when the new framing/rate is attached.
 */
Result reconfigure(Handle handle, const Config& configuration);
/** @brief Requests an idle TTL UART's GPIO-backed break without releasing pad claims.
 * serial::poll holds TX low, then idle high, then restores the original native
 * UART framing. Each phase accepts 1..1000 ms. RX is discarded while suspended.
 * New port opens/reconfigurations wait until restoration to reserve the controller.
 * RS-485 enable configurations are rejected; uncertain cleanup/restore quarantines.
 * @param handle Healthy idle UART with an owned TX pad and no RS-485 enables.
 * @param low_ms Minimum TX-low duration, 1..1000 milliseconds.
 * @param idle_ms Minimum TX-high duration before native restoration, 1..1000 milliseconds.
 * @return success means queued; break_pending remains true until restoration.
 * Repeating a request while pending returns busy. active becomes false on fault.
 */
Result request_break(Handle handle, std::uint32_t low_ms, std::uint32_t idle_ms);
/** @brief Reports a healthy handle awaiting asynchronous break/idle/UART restoration.
 * @param handle Retained UART owner to inspect.
 * @return true while low/high timing or restoration is pending; inspect active for faults.
 */
bool break_pending(Handle handle);
/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Handle);
/** @brief Schedules nonblocking cleanup once the current TX burst drains. */
Result request_close(Handle);
/** @brief Rejects closed, quarantined, and stale-generation handles. */
bool active(Handle);
/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Handle);
/** @brief Peeks or consumes currently buffered bytes without waiting for input.
 * @param handle Open port handle.
 * @param output Destination for copied RX bytes.
 * @param capacity Maximum number of bytes to copy.
 * @param peek Leaves copied bytes in the RX ring when true.
 * @return Transfer status and byte count.
 */
Transfer read(Handle handle, std::uint8_t* output, std::size_t capacity, bool peek = false);
/** @brief Holds RS-485 DE through all partial writes of one application-owned frame. */
Result begin_tx(Handle);
/** @brief Ends the frame; DE lowers only after the final accepted stop bit drains. */
Result end_tx(Handle);
/** @brief Accepts a bounded FIFO burst and reports the accepted byte count.
 * @param handle Open port handle.
 * @param data Bytes to enqueue.
 * @param size Number of bytes in data.
 * @return Transfer status and accepted byte count.
 */
Transfer write_some(Handle handle, const std::uint8_t* data, std::size_t size);
/** @brief Claims, configures, or releases an idle RS-485 enable pad. */
Result config_tx_en(Handle, PinId);
/** @brief Configures an optional separate receiver enable; active_level enables RX. */
Result config_rx_en(Handle, PinId, bool active_level = false);
/** @brief Reads native FIFO/shift-register emptiness without ending a burst or changing DE. */
bool transmitter_empty(Handle);
/** @brief Requires an ended burst and transmitter drain through the final stop bit. */
bool tx_idle(Handle);
/** @brief Waits for wire drain under a wrapping millisecond deadline. */
Result drain(Handle, std::uint32_t deadline_ms);
/** @brief Discards queued RX bytes without changing port configuration. */
Result discard_rx(Handle);
/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Handle);
/** @brief Reports whether any port is open; the main loop then skips its idle sleep. */
bool any_active();
// Called each app iteration so RS485 DE drops only after the final stop bit.
/** @brief Completes pending RS-485 enable transitions from the application loop. */
void poll();
} // namespace newmota::serial
