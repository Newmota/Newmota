/*
  edge_capture_native.h - Beken C boundary for native edge interrupts

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture_native.h
 * @brief Beken C boundary for native edge interrupts.
 */

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Disables interrupts while returning the exact prior interrupt mask.
 * @return Native interrupt mask to pass unchanged to newmota_beken_edge_unlock().
 */
uint32_t newmota_beken_edge_lock(void);
/** @brief Restores the prior interrupt mask, including from ISR context.
 * @param state Value previously returned by newmota_beken_edge_lock().
 */
void newmota_beken_edge_unlock(uint32_t state);
/** @brief Reads the SDK calendar's free-running microsecond timestamp.
 * @return Monotonic microseconds from the native calendar.
 */
uint64_t newmota_beken_edge_time(void);
/** @brief Registers a per-pin rising/falling callback under the caller's interrupt guard.
 * @param pin Native GPIO number.
 * @param rising Nonzero to capture rising edges.
 * @param handler Native callback receiving the triggering GPIO number.
 */
void newmota_beken_edge_start(unsigned pin, int rising, void (*handler)(unsigned char));
/** @brief Disables this pin, removes its callback and acknowledges stale pending state.
 * @param pin Native GPIO number to stop.
 */
void newmota_beken_edge_stop(unsigned pin);
#ifdef __cplusplus
}
#endif
