/*
  sdk_errno.c - Global errno required by the AmebaD SDK libraries

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/**
 * @file
 * @brief Defines the global errno that lwIP sockets and the newlib stubs link against.
 *
 * The SDK only provides this symbol from its nonblock_connect example, which Newmota does not
 * build. Do not include errno.h here: its errno macro would hide this definition.
 */

int errno = 0;
