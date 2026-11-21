/*
  esp_heap_caps.h - Allocation shim for the carried Shine encoder

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file esp_heap_caps.h
 * @brief Keeps native SDK allocation details outside the portable Shine core.
 */
#pragma once
#include <stdlib.h>
/** @brief Supplies a checked arena reserved before the carried initializer runs. */
void* newmota_shine_allocate(size_t);
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_32BIT 0

/** @brief Allocates ordinary word-aligned memory; no PSRAM requirement. */
static inline void* heap_caps_malloc(size_t size, unsigned) {
    return newmota_shine_allocate(size);
}

/** @brief Allocates ordinary word-aligned memory for the carried source signature. */
static inline void* heap_caps_malloc_prefer(size_t size, unsigned, unsigned) {
    return newmota_shine_allocate(size);
}
