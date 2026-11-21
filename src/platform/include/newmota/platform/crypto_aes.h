/*
  crypto_aes.h - Synchronous hardware AES boundary

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Complete AES block runs; selected providers never fall back to software.
 */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Largest run the BearSSL modes pass in one call; providers size engine buffers by it. */
#define NEWMOTA_AES_RUN_BLOCKS 16
/**
 * @brief Encrypt or decrypt a run of independent blocks (ECB) using the complete hardware key
 * schedule. The engine is acquired and the key loaded once per run, not per block.
 * @param key Raw key, with no alignment requirement.
 * @param key_len Key length in bytes: 16, 24, or 32.
 * @param decrypt Nonzero to decrypt.
 * @param input @p num complete input blocks; may equal output, must not overlap it otherwise.
 * @param output @p num complete results. Hardware failure must stop execution before returning.
 * @param num Number of blocks, at least 1.
 */
void newmota_aes_blocks(const void* key, size_t key_len, int decrypt, const unsigned char* input,
    unsigned char* output, size_t num);
#ifdef __cplusplus
}
#endif
