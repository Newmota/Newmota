/*
  supplicant_random.c - hardware random numbers for the Beken Wi-Fi supplicant

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Replaces the prebuilt supplicant's os_get_random() (libsupplicant os_none.o), which
 * fills the buffer from libc rand(): never seeded at boot, reseeded by any srand() caller,
 * and 31 bits per word. It supplies WPA2 nonces, SAE scalars and masks and OWE keys. The
 * link wraps the symbol (-Wl,--wrap=os_get_random) because os_none.o is linked for its
 * other functions anyway; this reads the hardware TRNG, as crypto_random.cpp does.
 */
#include <limits.h>
#include <stddef.h>
#include <string.h>

int BkRandomNumberRead(void* inBuffer, int inByteCount);

int __wrap_os_get_random(unsigned char* buf, size_t len);

int __wrap_os_get_random(unsigned char* buf, size_t len) {
    if (len == 0U) {
        return 0;
    }
    if (buf == NULL) {
        return -1;
    }
    if (len > (size_t)INT_MAX || BkRandomNumberRead(buf, (int)len) != 0) {
        memset(buf, 0, len);
        return -1;
    }
    return 0;
}
