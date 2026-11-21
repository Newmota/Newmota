/*
  atomic.c - atomic helpers for the ARMv5TE Beken core

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file atomic.c
 * @brief Atomic read-modify-write helpers for the ARMv5TE Beken core.
 *
 * ARMv5TE has no exclusive load/store, so GCC lowers std::atomic
 * read-modify-write operations to __atomic_* calls and the toolchain ships no
 * libatomic. The core is single, so masking interrupts makes each operation
 * atomic against tasks and interrupt handlers alike.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rtos_pub.h"

#define NEWMOTA_ATOMIC_RMW(size, type, name, expression)                                           \
    type __atomic_##name##_##size(volatile void* pointer, type value, int order) {                 \
        (void)order;                                                                               \
        volatile type* const target = (volatile type*)pointer;                                     \
        const uint32_t saved = rtos_disable_int();                                                 \
        const type old = *target;                                                                  \
        *target = (type)(expression);                                                              \
        rtos_enable_int(saved);                                                                    \
        return old;                                                                                \
    }

#define NEWMOTA_ATOMIC_SIZE(size, type)                                                            \
    NEWMOTA_ATOMIC_RMW(size, type, exchange, value)                                                \
    NEWMOTA_ATOMIC_RMW(size, type, fetch_add, old + value)                                         \
    NEWMOTA_ATOMIC_RMW(size, type, fetch_sub, old - value)                                         \
    NEWMOTA_ATOMIC_RMW(size, type, fetch_and, old& value)                                          \
    NEWMOTA_ATOMIC_RMW(size, type, fetch_or, old | value)                                          \
    NEWMOTA_ATOMIC_RMW(size, type, fetch_xor, old ^ value)                                         \
    bool __atomic_compare_exchange_##size(volatile void* pointer, void* expected, type desired,    \
        bool weak, int success, int failure) {                                                     \
        (void)weak;                                                                                \
        (void)success;                                                                             \
        (void)failure;                                                                             \
        volatile type* const target = (volatile type*)pointer;                                     \
        type* const current = (type*)expected;                                                     \
        const uint32_t saved = rtos_disable_int();                                                 \
        const type old = *target;                                                                  \
        const bool equal = old == *current;                                                        \
        if (equal)                                                                                 \
            *target = desired;                                                                     \
        rtos_enable_int(saved);                                                                    \
        if (!equal)                                                                                \
            *current = old;                                                                        \
        return equal;                                                                              \
    }

NEWMOTA_ATOMIC_SIZE(1, unsigned char)
NEWMOTA_ATOMIC_SIZE(2, unsigned short)
NEWMOTA_ATOMIC_SIZE(4, unsigned int)
