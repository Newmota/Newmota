/*
  radio.h - Shared Bouffalo RF initialization

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file radio.h
 * @brief Serializes the RF initialization shared by Wi-Fi and BLE.
 */
#pragma once
#include <atomic>
extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "rfparam_adapter.h"
}

namespace newmota::platform {
/** @brief Initializes RF once without requiring either radio's startup order. */
inline bool initialize_radio() {
    static std::atomic<int> state{0};
    int expected = 0;
    if (state.compare_exchange_strong(expected, 1)) {
        state.store(rfparam_init(0, nullptr, 0) == 0 ? 2 : -1);
    } else {
        while (state.load() == 1)
            vTaskDelay(1);
    }
    return state.load() == 2;
}
} // namespace newmota::platform
