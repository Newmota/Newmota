/*
  jpegdec_port.cpp - Builds JPEGDEC on ESP-IDF without Arduino for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file jpegdec_port.cpp
 * @brief Compiles the unchanged JPEGDEC sources with the SIMD path Arduino would select.
 *
 * JPEGDEC keys its ESP32 colour-conversion SIMD on Arduino board defines. They are defined
 * here only, for this translation unit, from the ESP-IDF target; jpegdec_simd.S does the same
 * for the assembly. The S3 path also reads esp-dsp's dsps_fft2r_platform.h from the vendored
 * esp-dsp.
 */
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define ARDUINO_ARCH_ESP32
#elif CONFIG_IDF_TARGET_ESP32P4
#define ARDUINO_ESP32P4_DEV
#endif

#include "JPEGDEC.cpp"
