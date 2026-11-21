/*
  fft.cpp - esp-dsp complex FFT on the ESP32-S3 and P4 for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file fft.cpp
 * @brief Calls esp-dsp's radix-2 kernel for the target with a caller-owned twiddle table.
 *
 * esp-dsp's own dsps_fft2r_init_fc32() keeps one table in globals for every caller. prepare()
 * builds the same table into the caller's buffer instead: the twiddles in bit-reversed order,
 * as the kernels read them.
 */
#include "newmota/platform/fft.h"

#include "sdkconfig.h"
extern "C" {
#include "dsps_fft2r.h"
}

#if !CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_IDF_TARGET_ESP32P4
#error "USE_ESP_DSP is derived for the ESP32-S3 and P4 only"
#endif

namespace newmota::platform::fft {
void prepare(float* const twiddle, const std::size_t n) {
    const int points = static_cast<int>(n);
    (void)dsps_gen_w_r2_fc32(twiddle, points);
    (void)dsps_bit_rev_fc32_ansi(twiddle, points >> 1);
}

void forward(float* const data, const float* const twiddle, const std::size_t n) {
    const int points = static_cast<int>(n);
    // The kernels only read the table; their prototypes lack the const.
    auto* const table = const_cast<float*>(twiddle);
#if CONFIG_IDF_TARGET_ESP32S3
    (void)dsps_fft2r_fc32_aes3_(data, points, table);
#else
    (void)dsps_fft2r_fc32_arp4_(data, points, table);
#endif
    (void)dsps_bit_rev2r_fc32(data, points);
}
} // namespace newmota::platform::fft
