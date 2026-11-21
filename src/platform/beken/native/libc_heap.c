/*
  libc_heap.c - Beken libc allocation bridge backed by FreeRTOS

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file libc_heap.c
 * @brief Beken libc allocation bridge backed by FreeRTOS.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"

/* Preserve the target's eight-byte malloc alignment and retain the requested
 * size for realloc. BDK os_realloc copies the NEW size from the old buffer,
 * which overreads on growth; libc allocations must not use that helper.
 * These headers belong only to the libc allocation family (including newlib
 * _r entry points). SDK os_malloc/pvPortMalloc allocations keep their ABI.
 */
typedef union {
    size_t size;
    uint64_t alignment;
} Allocation;

void* __wrap_malloc(size_t size) {
    if (size > SIZE_MAX - sizeof(Allocation)) {
        errno = ENOMEM;
        return NULL;
    }
    Allocation* block = pvPortMalloc(sizeof(Allocation) + size);
    if (!block) {
        errno = ENOMEM;
        return NULL;
    }
    block->size = size;
    return block + 1;
}

void __wrap_free(void* pointer) {
    if (pointer) {
        vPortFree((Allocation*)pointer - 1);
    }
}

void* __wrap_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    const size_t total = count * size;
    void* pointer = __wrap_malloc(total);
    if (pointer) {
        memset(pointer, 0, total);
    }
    return pointer;
}

void* __wrap_realloc(void* pointer, size_t size) {
    if (!pointer) {
        return __wrap_malloc(size);
    }
    if (!size) {
        __wrap_free(pointer);
        return NULL;
    }
    Allocation* block = (Allocation*)pointer - 1;
    if (size <= block->size) {
        block->size = size;
        return pointer;
    }
    void* replacement = __wrap_malloc(size);
    if (replacement) {
        memcpy(replacement, pointer, block->size);
        __wrap_free(pointer);
    }
    return replacement;
}

struct _reent;

void* __wrap__malloc_r(struct _reent* context, size_t size) {
    (void)context;
    return __wrap_malloc(size);
}

void __wrap__free_r(struct _reent* context, void* pointer) {
    (void)context;
    __wrap_free(pointer);
}

void* __wrap__calloc_r(struct _reent* context, size_t count, size_t size) {
    (void)context;
    return __wrap_calloc(count, size);
}

void* __wrap__realloc_r(struct _reent* context, void* pointer, size_t size) {
    (void)context;
    return __wrap_realloc(pointer, size);
}
