/*
  font_tamp.c - tamp decompressor for the embedded fonts

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file font_tamp.c
 * @brief Compiles the decompressor the font build stages beside the feature header.
 *
 * tamp's generic file names would collide in the flat source folders of some SDKs, so its
 * sources are included here and no other file sees its headers.
 */
#include "services/berry/modules/font/font_tamp.h"

#define TAMP_STREAM 0

#pragma GCC diagnostic ignored "-Wundef"
#include "tamp/common.c"
#include "tamp/decompressor.c"

enum { FONT_WINDOW_BITS = 8 };

bool font_tamp_decompress(
    unsigned char* output, size_t size, const unsigned char* stream, size_t length) {
    unsigned char window[1U << FONT_WINDOW_BITS];
    TampDecompressor decompressor;
    size_t made = 0, used = 0;
    if (tamp_decompressor_init(&decompressor, NULL, window, FONT_WINDOW_BITS) != TAMP_OK)
        return false;
    const tamp_res result =
        tamp_decompressor_decompress(&decompressor, output, size, &made, stream, length, &used);
    return result >= 0 && made == size;
}
