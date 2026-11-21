/*
  bdk_compat.c - small BDK compatibility hooks owned by the Beken adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file bdk_compat.c
 * @brief Small BDK compatibility hooks owned by the Beken adapter.
 */

#include "include.h"

/* No crtbegin object is linked by the BDK startup. C++ static destructors
 * register against this image handle when its constructor table runs.
 */
void* __dso_handle = &__dso_handle;

/*
 * The stock BDK app startup unconditionally initializes its legacy CLI.  The
 * Newmota console is installed through the platform adapter instead, so the
 * command table and its large UART task are not part of this image.  Keep the
 * hook weak: a future target profile may provide the native CLI explicitly.
 */
__attribute__((weak)) int cli_init(void) {
    return 0;
}
