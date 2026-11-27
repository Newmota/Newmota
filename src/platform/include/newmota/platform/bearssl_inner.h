/*
  bearssl_inner.h - BearSSL's internal header for Newmota sources

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file
 * @brief Includes BearSSL's inner.h without its -Wundef warnings.
 * inner.h tests undefined target and compiler macros with #if. Newmota sources include
 * this header instead, so -Wundef stays on for their own code.
 */
#pragma once

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif

#include "inner.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
