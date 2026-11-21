/*
  crypto_random.cpp - ESP-IDF cryptographic randomness adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file crypto_random.cpp
 * @brief ESP-IDF cryptographic randomness adapter.
 */

#include "newmota/platform/crypto_random.h"
#if defined(ESP_PLATFORM)
#include "newmota_features.h"
#include "sdkconfig.h"
#if USE_WIFI && CONFIG_SOC_WIFI_SUPPORTED
#define NEWMOTA_CRYPTO_WIFI_RANDOM 1
#else
#define NEWMOTA_CRYPTO_WIFI_RANDOM 0
#endif
#else
// The host adapter fixture provides a fake Wi-Fi lifecycle and entropy source.
#define NEWMOTA_CRYPTO_WIFI_RANDOM 1
#endif
#if NEWMOTA_CRYPTO_WIFI_RANDOM
#include "network_internal.h"
#endif
#include "esp_random.h"

namespace newmota::platform {
CryptoRandomResult crypto_random(void* destination, std::size_t size) {
    if (!size)
        return CryptoRandomResult::success;
    if (!destination)
        return CryptoRandomResult::invalid_request;
    // esp_random has continuous hardware entropy while Wi-Fi is running.
    // Require a driver-start event; association state alone is insufficient
    // because a failed connection attempt can publish idle before radio start.
#if NEWMOTA_CRYPTO_WIFI_RANDOM
    if (wifi_driver_running()) {
        esp_fill_random(destination, size);
        return CryptoRandomResult::success;
    }
#endif
    // No entropy source is currently qualified for ESP targets without an
    // active Wi-Fi radio, so fail closed and erase the caller's buffer.
    auto* p = static_cast<volatile unsigned char*>(destination);
    for (std::size_t i = 0; i < size; ++i)
        p[i] = 0;
    return CryptoRandomResult::unavailable;
}
} // namespace newmota::platform
