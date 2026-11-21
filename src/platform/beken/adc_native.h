/*
  adc_native.h - Beken ADC C boundary

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc_native.h
 * @brief Beken ADC C boundary.
 */

#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Maps an external ADC pad to its SDK channel, or returns -1. */
int newmota_beken_adc_channel(unsigned pin);
/** @brief Starts a single retained SDK conversion; returns 1 for contention. */
int newmota_beken_adc_start(unsigned pin);
/** @brief Consumes one completed SDK conversion; 1 means pending, -1 means failed. */
int newmota_beken_adc_finish(uint32_t* raw, uint32_t* millivolts);
/** @brief Stops interrupts/controller before the static SDK descriptor can be reused. */
int newmota_beken_adc_cancel(void);
#ifdef __cplusplus
}
#endif
