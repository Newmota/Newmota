/*
  ble_support.cpp - AmebaD BLE storage and trace ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_support.cpp
 * @brief Native host hooks that never claim vendor flash or a trace UART.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "wifi_conf.h"
#include "wifi_constants.h"
}

/** @brief The native controller calibration requires an initialized Wi-Fi radio. */
extern "C" bool newmota_amebad_ble_radio_ready() {
    return wifi_is_up(RTW_STA_INTERFACE) || wifi_is_up(RTW_AP_INTERFACE);
}

namespace {
constexpr std::size_t kStorageSize = 4096;
std::uint8_t storage[kStorageSize];
std::uint8_t valid[kStorageSize / 32];

bool valid_range(const void* data, std::uint16_t offset, std::uint16_t size) {
    return data != nullptr && size != 0 && (offset & 3) == 0 && (size & 3) == 0 &&
           offset <= kStorageSize && size <= kStorageSize - offset;
}
} // namespace

/** @brief Realtek's FTL ABI backed exclusively by RAM, with absent words reported. */
extern "C" std::uint32_t ftl_load_from_storage(
    void* data, std::uint16_t offset, std::uint16_t size) {
    if (!valid_range(data, offset, size))
        return 1;
    taskENTER_CRITICAL();
    for (std::size_t word = offset / 4; word < (offset + size) / 4; ++word)
        if ((valid[word / 8] & (1U << (word % 8))) == 0) {
            memset(data, 0xFF, size);
            taskEXIT_CRITICAL();
            return 1;
        }
    memcpy(data, storage + offset, size);
    taskEXIT_CRITICAL();
    return 0;
}

/** @brief Stores bounded native records in RAM; never writes the vendor FTL addresses. */
extern "C" std::uint32_t ftl_save_to_storage(void* data, std::uint16_t offset, std::uint16_t size) {
    if (!valid_range(data, offset, size))
        return 1;
    taskENTER_CRITICAL();
    memcpy(storage + offset, data, size);
    for (std::size_t word = offset / 4; word < (offset + size) / 4; ++word)
        valid[word / 8] |= 1U << (word % 8);
    taskEXIT_CRITICAL();
    return 0;
}

/** @brief Discards native binary trace frames; no auxiliary UART or pads are acquired. */
extern "C" bool trace_print(void*, std::uint16_t) {
    return true;
}
