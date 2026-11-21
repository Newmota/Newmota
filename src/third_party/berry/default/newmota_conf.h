/*
  newmota_conf.h - effective Berry configuration for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

// Keep these effective values explicit. Berry's COC generator scans defines
// without evaluating conditional branches, so it consumes this file after the
// upstream configuration while the C compiler includes the same overrides.
#undef BE_INTGER_TYPE
#define BE_INTGER_TYPE 1
#undef BE_USE_FILE_SYSTEM
#define BE_USE_FILE_SYSTEM 0
#undef BE_USE_PREPROCESSOR
#define BE_USE_PREPROCESSOR 0
#undef BE_USE_MEM_ALIGNED
#define BE_USE_MEM_ALIGNED 0

#undef BE_USE_STRING_MODULE
#define BE_USE_STRING_MODULE 1
#undef BE_USE_JSON_MODULE
#define BE_USE_JSON_MODULE 1
#undef BE_USE_MATH_MODULE
#define BE_USE_MATH_MODULE 1
#undef BE_USE_TIME_MODULE
#define BE_USE_TIME_MODULE 0
#undef BE_USE_OS_MODULE
#define BE_USE_OS_MODULE 0
#undef BE_USE_GLOBAL_MODULE
#define BE_USE_GLOBAL_MODULE 1
#undef BE_USE_SYS_MODULE
#define BE_USE_SYS_MODULE 1
#undef BE_USE_DEBUG_MODULE
#define BE_USE_DEBUG_MODULE 0
#undef BE_USE_GC_MODULE
#define BE_USE_GC_MODULE 1
#undef BE_USE_SOLIDIFY_MODULE
#define BE_USE_SOLIDIFY_MODULE 0
#undef BE_USE_INTROSPECT_MODULE
#define BE_USE_INTROSPECT_MODULE 1
#undef BE_USE_STRICT_MODULE
#define BE_USE_STRICT_MODULE 1
