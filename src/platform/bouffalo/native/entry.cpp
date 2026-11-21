/*
  entry.cpp - Bouffalo application entry point for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file entry.cpp
 * @brief Native board initialization, C++ constructors and FreeRTOS entry.
 */

#include "newmota/app.h"

#include "FreeRTOS.h"
#include "task.h"

#include <cstdint>

/** @brief Initializes the selected Bouffalo board. */
extern "C" void board_init();
/** @brief Runs C++ global constructors retained by the selected linker script. */
extern "C" void __libc_init_array();

extern "C" {
/** @brief Supplies the image handle used by static destructor registration. */
void* __dso_handle = &__dso_handle;
}

namespace {

// FreeRTOS expresses this depth in 32-bit StackType_t words: 8 KiB total.
constexpr std::uint16_t kTaskStackDepth = 2048;

/** @brief Runs Newmota after the Bouffalo FreeRTOS scheduler starts. */
void NewmotaTask(void*) {
    newmota::run();
}

} // namespace

/** @brief Initializes the board and starts Newmota under FreeRTOS. */
int main() {
    board_init();
    // Board startup registers the native heaps used by allocating constructors.
    __libc_init_array();
    if (xTaskCreate(NewmotaTask, "newmota", kTaskStackDepth, nullptr, 10, nullptr) != pdPASS) {
        return 1;
    }
    vTaskStartScheduler();
    while (true) {}
}
