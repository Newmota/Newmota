/*
  cmsis_fft.cpp - CMSIS-DSP complex FFT on the RP2350 and AmebaD for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file cmsis_fft.cpp
 * @brief Runs CMSIS-DSP's mixed-radix float FFT from its constant tables.
 *
 * Only the three sizes the spectrum analyzer uses are referenced, so the linker drops the
 * library's other tables.
 */
#include "newmota/platform/fft.h"

#include "arm_const_structs.h"

namespace newmota::platform::fft {
void forward(float* const data, const float* const, const std::size_t n) {
    const auto& instance = n == 512U    ? arm_cfft_sR_f32_len512
                           : n == 1024U ? arm_cfft_sR_f32_len1024
                                        : arm_cfft_sR_f32_len2048;
    arm_cfft_f32(&instance, data, 0, 1);
}
} // namespace newmota::platform::fft
