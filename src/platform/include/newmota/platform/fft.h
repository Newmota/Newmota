/*
  fft.h - Platform complex FFT for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file fft.h
 * @brief A complex FFT from a DSP library, built only with USE_ESP_DSP or USE_CMSIS_DSP.
 *
 * Both flags are derived: the spectrum analyzer uses esp-dsp's assembly kernels on an ESP32-S3
 * or P4 and CMSIS-DSP on the Cortex-M33 of the RP2350 and AmebaD. Other builds keep the
 * application's portable transform; there is no runtime fallback between them.
 */
#pragma once

#include <cstddef>

namespace newmota::platform::fft {
/** @brief Alignment, in bytes, of the data and twiddle buffers. */
constexpr std::size_t kAlignment = 16;

/** @brief Fills the n-float twiddle table that esp-dsp's forward() needs for @p n points. */
void prepare(float* twiddle, std::size_t n);

/**
 * @brief Transforms @p n interleaved complex points (re, im) in place, in natural order.
 *
 * With esp-dsp, @p n is a power of two from 16 to 4096. With CMSIS-DSP it is 512, 1024 or 2048,
 * and @p twiddle is unused because the library's tables are constants. Both buffers are aligned
 * to kAlignment.
 */
void forward(float* data, const float* twiddle, std::size_t n);
} // namespace newmota::platform::fft
