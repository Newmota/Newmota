/*
  sdk_image_sha256.c - BearSSL image hash for the ESP-IDF fork without mbedTLS

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Application-side bootloader_sha256_start/data/finish when the build contains no
 * mbedtls component. The ESP-IDF fork's bootloader_support then expects the application
 * to define them (OTA, image and partition checks call them). BearSSL SHA-256 uses the
 * selected SHA provider. Contract of the replaced mbedTLS variant: every handle is
 * independent, a failed start returns NULL, and finish releases the handle, also with a
 * NULL digest.
 */
#include "bootloader_sha.h"

#include <assert.h>
#include <stdlib.h>

#include "bearssl_hash.h"
#include "newmota_crypto.h"

bootloader_sha256_handle_t bootloader_sha256_start(void)
{
    br_sha256_context *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    br_sha256_init(ctx);
    return ctx;
}

void bootloader_sha256_data(bootloader_sha256_handle_t handle, const void *data, size_t data_len)
{
    assert(handle != NULL);
    br_sha256_update((br_sha256_context *)handle, data, data_len);
}

void bootloader_sha256_finish(bootloader_sha256_handle_t handle, uint8_t *digest)
{
    assert(handle != NULL);
    br_sha256_context *ctx = (br_sha256_context *)handle;
    if (digest != NULL) {
        br_sha256_out(ctx, digest);
    }
    nm_crypto_zero(ctx, sizeof(*ctx));
    free(ctx);
}
