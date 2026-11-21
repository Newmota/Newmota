/*
  sdk_log.h - BouffaloSDK logging compatibility declarations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sdk_log.h
 * @brief BouffaloSDK logging compatibility declarations.
 *
 * The build adapter includes this header from an SDK overlay after normal
 * logging suppression. It restores the libc declarations for legacy SDK
 * sources that deliberately undefine printf and puts.
 *
 * When the SDK suppresses printf and puts, they become unevaluated `sizeof`
 * operands instead of empty expansions. No call reaches the image, but
 * arguments stay referenced, so log-only variables are not reported unused.
 */

#ifndef NEWMOTA_BOUFFALO_SDK_LOG_H
#define NEWMOTA_BOUFFALO_SDK_LOG_H

#ifdef CONFIG_LOG_DISABLE
#pragma push_macro("printf")
#pragma push_macro("puts")
#undef printf
#undef puts
/* Declare libc output first, so later stdio.h includes never see the macros. */
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Declares the C formatted-output function used by legacy SDK code. */
int printf(const char* format, ...);
/** @brief Declares the C line-output function used by legacy SDK code. */
int puts(const char* text);
/** @brief Type-only sink for suppressed SDK output; never defined or called. */
int newmota_sdk_log_discard(const char* format, ...);

#ifdef __cplusplus
}
#endif

#ifdef CONFIG_LOG_DISABLE
#pragma pop_macro("puts")
#pragma pop_macro("printf")
/* The SDK passes -Dprintf(...)= to its libraries and -Uprintf to the app. */
#ifdef printf
#undef printf
#define printf(...) ((void)sizeof(newmota_sdk_log_discard(__VA_ARGS__)))
#endif
#ifdef puts
#undef puts
#define puts(...) ((void)sizeof(newmota_sdk_log_discard(__VA_ARGS__)))
#endif
#endif

#endif

#if defined(CONFIG_LOG_DISABLE) && defined(NEWMOTA_BOUFFALO_SDK_LOG_REAPPLY)
#undef printf
#define printf(...) ((void)sizeof(newmota_sdk_log_discard(__VA_ARGS__)))
#undef puts
#define puts(...) ((void)sizeof(newmota_sdk_log_discard(__VA_ARGS__)))
#endif
