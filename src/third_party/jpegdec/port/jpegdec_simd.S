/*
  jpegdec_simd.S - JPEGDEC colour-conversion SIMD for the ESP-IDF target

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Assembles the unchanged JPEGDEC SIMD files for the target with the defines jpegdec_port.cpp sets,
  so the C++ side and the assembly agree. Other targets assemble to nothing.
*/
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define ARDUINO_ARCH_ESP32
#include "s3_simd_420.S"
#include "s3_simd_444.S"
#elif CONFIG_IDF_TARGET_ESP32P4
#define ARDUINO_ESP32P4_DEV
#include "p4_simd_420.S"
#include "p4_simd_444.S"
#endif
