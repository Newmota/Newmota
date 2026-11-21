/*
  font_compress.c - host tool compressing font glyphs with tamp

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file font_compress.c
 * @brief Compresses glyphs in independent blocks and proves each one by decompressing it.
 *
 * Usage: font_compress <raw> <glyph-bytes> <block-glyphs> <output>. The output holds one
 * little-endian 16-bit stream offset per block, the end offset, and then the streams. The build
 * compiles this without lazy matching, which corrupts streams together with extended mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compressor.h"
#include "decompressor.h"

enum { WINDOW_BITS = 8, WINDOW_SIZE = 1 << WINDOW_BITS, MAX_RAW = 1 << 16, MAX_BLOCKS = 256 };

static unsigned char raw[MAX_RAW], streams[MAX_RAW * 2], restored[MAX_RAW];

static void fail(const char* message) {
    fprintf(stderr, "font_compress: %s\n", message);
    exit(1);
}

/** @brief Compresses one block to @p out, verifies it and returns the stream length. */
static size_t compress_block(
    const unsigned char* block, size_t size, unsigned char* out, size_t capacity) {
    unsigned char window[WINDOW_SIZE];
    TampConf conf = {0};
    conf.window = WINDOW_BITS;
    conf.literal = 8;
    conf.extended = 1;
    TampCompressor compressor;
    size_t written = 0, consumed = 0;
    if (tamp_compressor_init(&compressor, &conf, window) != TAMP_OK)
        fail("compressor setup failed");
    if (tamp_compressor_compress_and_flush(
            &compressor, out, capacity, &written, block, size, &consumed, false) != TAMP_OK ||
        consumed != size)
        fail("compression failed");

    TampDecompressor decompressor;
    size_t made = 0, used = 0;
    if (tamp_decompressor_init(&decompressor, NULL, window, WINDOW_BITS) != TAMP_OK)
        fail("decompressor setup failed");
    const tamp_res result =
        tamp_decompressor_decompress(&decompressor, restored, size, &made, out, written, &used);
    if (result < 0 || made != size || memcmp(restored, block, size) != 0)
        fail("stream does not decompress to its glyphs");
    return written;
}

int main(int argc, char** argv) {
    if (argc != 5)
        fail("usage: font_compress <raw> <glyph-bytes> <block-glyphs> <output>");
    FILE* input = fopen(argv[1], "rb");
    if (!input)
        fail("cannot read the glyphs");
    const size_t total = fread(raw, 1, sizeof raw, input);
    fclose(input);
    const size_t glyph_bytes = strtoul(argv[2], NULL, 10);
    const size_t block_glyphs = strtoul(argv[3], NULL, 10);
    if (!total || !glyph_bytes || !block_glyphs || total % glyph_bytes)
        fail("invalid glyph sizes");

    const size_t block_bytes = glyph_bytes * block_glyphs;
    const size_t blocks = (total + block_bytes - 1) / block_bytes;
    if (blocks > MAX_BLOCKS)
        fail("too many blocks");
    unsigned offsets[MAX_BLOCKS + 1];
    size_t length = 0;
    for (size_t block = 0; block < blocks; ++block) {
        const size_t start = block * block_bytes;
        const size_t size = total - start < block_bytes ? total - start : block_bytes;
        offsets[block] = (unsigned)length;
        length += compress_block(raw + start, size, streams + length, sizeof streams - length);
        if (length > 0xffffU)
            fail("streams exceed 16-bit offsets");
    }
    offsets[blocks] = (unsigned)length;

    FILE* output = fopen(argv[4], "wb");
    if (!output)
        fail("cannot write the output");
    for (size_t i = 0; i <= blocks; ++i) {
        fputc(offsets[i] & 0xffU, output);
        fputc(offsets[i] >> 8U, output);
    }
    if (fwrite(streams, 1, length, output) != length || fclose(output) != 0)
        fail("cannot write the output");
    return 0;
}
