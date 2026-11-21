/*
  atomic_bl602.c - Atomic library calls for the BL602 core without LR/SC

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief The BL602 E24 core traps on LR/SC as illegal instructions, although the pinned SDK
 * compiles for rv32imafc. Newmota compiles BL602 for rv32imfc instead, as Bouffalo's own
 * Matter build does, so read-modify-write atomics become these library calls. The core is
 * single-hart and runs in machine mode: masking machine interrupts makes each operation
 * atomic in tasks and interrupt handlers alike, and nests. Memory orders are all served as
 * sequentially consistent. A missing width or operation fails at link time.
 */
#include <stdbool.h>
#include <stdint.h>

#if !defined(BL602)
#error "atomic_bl602.c is only for the BL602"
#endif
#if defined(__riscv_atomic)
#error "BL602 sources must be compiled without the A extension"
#endif

static inline unsigned long irq_save(void) {
    unsigned long status;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(status) : : "memory");
    return status;
}

static inline void irq_restore(unsigned long status) {
    if (status & 8UL)
        __asm__ volatile("csrsi mstatus, 8" : : : "memory");
}

/* Asm labels give the libcall names without redeclaring GCC's built-in functions. */
#define NM_ATOMIC_RMW(type, size, name, expr)                                                      \
    type nm_atomic_fetch_##name##_##size(volatile type* ptr, type value, int order) __asm__(       \
        "__atomic_fetch_" #name "_" #size);                                                        \
    type nm_atomic_fetch_##name##_##size(volatile type* ptr, type value, int order) {              \
        (void)order;                                                                               \
        unsigned long status = irq_save();                                                         \
        type old = *ptr;                                                                           \
        *ptr = (type)(expr);                                                                       \
        irq_restore(status);                                                                       \
        return old;                                                                                \
    }                                                                                              \
    type nm_atomic_##name##_fetch_##size(volatile type* ptr, type value, int order) __asm__(       \
        "__atomic_" #name "_fetch_" #size);                                                        \
    type nm_atomic_##name##_fetch_##size(volatile type* ptr, type value, int order) {              \
        (void)order;                                                                               \
        unsigned long status = irq_save();                                                         \
        type old = *ptr;                                                                           \
        type result = (type)(expr);                                                                \
        *ptr = result;                                                                             \
        irq_restore(status);                                                                       \
        return result;                                                                             \
    }

#define NM_ATOMIC_WIDTH(type, size)                                                                \
    type nm_atomic_exchange_##size(volatile type* ptr, type value, int order) __asm__(             \
        "__atomic_exchange_" #size);                                                               \
    type nm_atomic_exchange_##size(volatile type* ptr, type value, int order) {                    \
        (void)order;                                                                               \
        unsigned long status = irq_save();                                                         \
        type old = *ptr;                                                                           \
        *ptr = value;                                                                              \
        irq_restore(status);                                                                       \
        return old;                                                                                \
    }                                                                                              \
    bool nm_atomic_compare_exchange_##size(volatile type* ptr, type* expected, type desired,       \
        bool weak, int success, int failure) __asm__("__atomic_compare_exchange_" #size);          \
    bool nm_atomic_compare_exchange_##size(                                                        \
        volatile type* ptr, type* expected, type desired, bool weak, int success, int failure) {   \
        (void)weak;                                                                                \
        (void)success;                                                                             \
        (void)failure;                                                                             \
        unsigned long status = irq_save();                                                         \
        type old = *ptr;                                                                           \
        bool equal = old == *expected;                                                             \
        if (equal)                                                                                 \
            *ptr = desired;                                                                        \
        irq_restore(status);                                                                       \
        if (!equal)                                                                                \
            *expected = old;                                                                       \
        return equal;                                                                              \
    }                                                                                              \
    NM_ATOMIC_RMW(type, size, add, old + value)                                                    \
    NM_ATOMIC_RMW(type, size, sub, old - value)                                                    \
    NM_ATOMIC_RMW(type, size, and, (old & value))                                                  \
    NM_ATOMIC_RMW(type, size, or, old | value)                                                     \
    NM_ATOMIC_RMW(type, size, xor, old ^ value)                                                    \
    NM_ATOMIC_RMW(type, size, nand, ~(old & value))

NM_ATOMIC_WIDTH(uint8_t, 1)
NM_ATOMIC_WIDTH(uint16_t, 2)
NM_ATOMIC_WIDTH(uint32_t, 4)
