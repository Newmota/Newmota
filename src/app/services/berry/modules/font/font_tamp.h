/*
  font_tamp.h - tamp decompression for the embedded fonts

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

/** @file font_tamp.h
 *  @brief Declares the decompression step of the Berry font class.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decompresses one font block with a 256-byte window on the stack.
 *
 *  @param output Receives the glyphs.
 *  @param size Exact number of glyph bytes expected.
 *  @param stream Compressed block, including its tamp header.
 *  @param length Length of @p stream in bytes.
 *  @return True when exactly @p size bytes were produced.
 */
bool font_tamp_decompress(
    unsigned char* output, size_t size, const unsigned char* stream, size_t length);

#ifdef __cplusplus
}
#endif
