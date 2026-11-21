/*
  pgmspace.h - Memory-mapped flash compatibility for carried audio codecs

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file pgmspace.h
 * @brief The supported SoCs read const flash data through ordinary address space.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#define PROGMEM
#define PSTR(text) (text)
#define memcpy_P(destination, source, size) memcpy(destination, source, size)

/** @brief Loads a potentially unaligned flash byte. */
static inline uint8_t pgm_read_byte(const void* address) {
    uint8_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

/** @brief Loads a potentially unaligned flash word. */
static inline uint16_t pgm_read_word(const void* address) {
    uint16_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}

/** @brief Loads a potentially unaligned flash double word. */
static inline uint32_t pgm_read_dword(const void* address) {
    uint32_t value;
    memcpy(&value, address, sizeof(value));
    return value;
}
