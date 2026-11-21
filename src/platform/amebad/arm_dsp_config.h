/*
  arm_dsp_config.h - CMSIS-DSP settings for the AmebaD SDK

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file arm_dsp_config.h
 * @brief Supplies what the SDK's older CMSIS-Core headers lack for CMSIS-DSP.
 *
 * CMSIS-DSP includes this file first when ARM_DSP_CUSTOM_CONFIG is set.
 */
#pragma once

#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
