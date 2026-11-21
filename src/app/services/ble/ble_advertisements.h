/*
  ble_advertisements.h - Shared advertisement delivery to native and script consumers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_advertisements.h
 * @brief The one place that takes advertisements from the platform scan slot.
 *
 * The native consumer sees every advertisement as soon as the loop takes it. The script
 * consumer gets a held copy, because Berry may not read while a connection job owns its
 * buffer; reports arriving while the copy is held are counted as script drops. Without a
 * native consumer the script reads the platform slot directly, as before.
 */
#pragma once
#include "newmota/platform/ble.h"
#include <cstdint>

namespace newmota::ble {
/** @brief Receives one advertisement in the cooperative loop; must not keep the reference. */
using AdvertisementSink = void (*)(const platform::BleAdvertisement&);

/** @brief Sets or clears (nullptr) the native consumer and its scan demand. */
void set_native_consumer(AdvertisementSink sink);
/** @brief Sets or clears the script consumer's scan demand; clearing drops a held copy. */
void set_script_consumer(bool on);
/** @brief Takes waiting advertisements for the native consumer; call every loop iteration. */
void poll_advertisements();
/** @brief Reports whether take_script_advertisement() would return one. */
[[nodiscard]] bool script_advertisement_pending();
/** @brief Takes the advertisement waiting for the script. @return False when there is none. */
[[nodiscard]] bool take_script_advertisement(platform::BleAdvertisement& advertisement);
/** @brief Advertisements the script missed because its copy was still held. */
[[nodiscard]] std::uint32_t script_advertisements_dropped();
} // namespace newmota::ble
