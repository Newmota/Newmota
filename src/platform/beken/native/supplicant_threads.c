/*
  supplicant_threads.c - stack size of the Beken supplicant's PSK thread

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief The prebuilt supplicant (libsupplicant wpa_psk_cache.o) derives the WPA2 PSK in
 * wpa_psk_cal_thread, created with a fixed 1024-byte stack. With the BearSSL backend,
 * pbkdf2_sha1() needs about 1.3 KB there (pbkdf2_sha1 496, br_hmac_out 304, br_sha1_out 112,
 * br_sha1_round 368, from -fstack-usage); mbedTLS kept its hash contexts on the heap. The link
 * wraps rtos_create_thread (-Wl,--wrap=rtos_create_thread, USE_WIFI_WPA3_BEARSSL only) and
 * raises the stack of that one thread, matched by its entry function. The thread deletes
 * itself after the derivation.
 */
#include "rtos_pub.h"

#define NEWMOTA_PSK_THREAD_STACK_SIZE 2048U

void wpa_psk_cal_thread(beken_thread_arg_t arg);

OSStatus __real_rtos_create_thread(beken_thread_t* thread, uint8_t priority, const char* name,
    beken_thread_function_t function, uint32_t stack_size, beken_thread_arg_t arg);
OSStatus __wrap_rtos_create_thread(beken_thread_t* thread, uint8_t priority, const char* name,
    beken_thread_function_t function, uint32_t stack_size, beken_thread_arg_t arg);

OSStatus __wrap_rtos_create_thread(beken_thread_t* thread, uint8_t priority, const char* name,
    beken_thread_function_t function, uint32_t stack_size, beken_thread_arg_t arg) {
    if (function == (beken_thread_function_t)wpa_psk_cal_thread &&
        stack_size < NEWMOTA_PSK_THREAD_STACK_SIZE) {
        stack_size = NEWMOTA_PSK_THREAD_STACK_SIZE;
    }
    return __real_rtos_create_thread(thread, priority, name, function, stack_size, arg);
}
