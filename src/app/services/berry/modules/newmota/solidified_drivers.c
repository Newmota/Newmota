/*
  solidified_drivers.c - Berry driver classes built in by an environment's solidify list

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "berry.h"

#include "solidified_drivers.h"

/** @brief Sets each built-in driver class as an ordinary global, so a loaded file may replace it.
 */
void newmota_install_solidified_drivers(bvm* vm) {
#define NEWMOTA_INSTALL_DRIVER(name)                                                               \
    be_pushntvclass(vm, &be_class_##name);                                                         \
    be_setglobal(vm, #name);                                                                       \
    be_pop(vm, 1);
    NEWMOTA_SOLIDIFIED_DRIVERS(NEWMOTA_INSTALL_DRIVER)
#undef NEWMOTA_INSTALL_DRIVER
}
