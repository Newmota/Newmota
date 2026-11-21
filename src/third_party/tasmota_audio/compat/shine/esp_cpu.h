/*
  esp_cpu.h - Disabled Shine diagnostic cycle counter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file esp_cpu.h
 * @brief Shine cycle instrumentation is not part of the portable encoder contract.
 */
#pragma once
#include <stdint.h>

/** @brief Returns zero for optional upstream diagnostic counters. */
static inline uint32_t esp_cpu_get_cycle_count(void) {
    return 0;
}
