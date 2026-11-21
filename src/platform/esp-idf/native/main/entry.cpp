/*
  entry.cpp - ESP-IDF application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/app.h"

/** @brief Transfers the ESP-IDF main task into the Newmota runtime. */
extern "C" void app_main() {
    newmota::run();
}
