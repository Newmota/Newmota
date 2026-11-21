/*
  entry.cpp - Raspberry Pi Pico application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/app.h"

#include "boot_select.h"
#include "memory_info.h"

/** @brief Transfers the Pico SDK application entry point into Newmota. */
int main() {
    newmota::platform::pico_detail::initialize_stack_watermark();
    // Honor a sticky Safeboot request before any service starts, while only
    // clocks and XIP are active; chaining this early keeps USB, LWIP, and
    // the watchdog untouched by the abandoned image.
    newmota::platform::pico_detail::honor_sticky_boot_request();
    newmota::run();
}
