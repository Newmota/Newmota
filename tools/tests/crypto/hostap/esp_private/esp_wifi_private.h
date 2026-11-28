/* Host shim for the hostap SAE test: the OS function table that os.h names. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define OSI_FUNCS_TIME_BLOCKING 0xffffffff

typedef struct {
    void (*_timer_setfn)(void*, void*, void*);
    void (*_timer_disarm)(void*);
    void (*_timer_arm_us)(void*, uint32_t, bool);
    void (*_timer_arm)(void*, uint32_t, bool);
    void (*_timer_done)(void*);
} wifi_osi_funcs_t;
