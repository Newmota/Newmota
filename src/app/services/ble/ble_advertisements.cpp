/*
  ble_advertisements.cpp - Shared advertisement delivery to native and script consumers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_advertisements.cpp
 * @brief Fan-out of the single platform scan slot to the native and script consumers.
 */
#include "services/ble/ble_advertisements.h"

namespace newmota::ble {
namespace {
AdvertisementSink native_sink{};
bool script_wanted{}, script_held{};
platform::BleAdvertisement script_copy{};
std::uint32_t script_dropped{};
} // namespace

void set_native_consumer(AdvertisementSink sink) {
    native_sink = sink;
    script_held = false; // the script reads the platform slot again without a native consumer
    platform::ble_scan_set(platform::BleScanConsumer::native, sink != nullptr);
}

void set_script_consumer(bool on) {
    script_wanted = on;
    script_held = false;
    platform::ble_scan_set(platform::BleScanConsumer::berry, on);
}

void poll_advertisements() {
    if (native_sink == nullptr)
        return;
    platform::BleAdvertisement advertisement;
    // The slot holds one report; taking it frees the scan task for the next.
    if (!platform::ble_scan_take(advertisement))
        return;
    native_sink(advertisement);
    if (!script_wanted)
        return;
    if (script_held) {
        ++script_dropped;
        return;
    }
    script_copy = advertisement;
    script_held = true;
}

bool script_advertisement_pending() {
    return native_sink != nullptr ? script_held : platform::ble_scan_pending();
}

bool take_script_advertisement(platform::BleAdvertisement& advertisement) {
    if (native_sink == nullptr)
        return platform::ble_scan_take(advertisement);
    if (!script_held)
        return false;
    advertisement = script_copy;
    script_held = false;
    return true;
}

std::uint32_t script_advertisements_dropped() {
    return script_dropped;
}
} // namespace newmota::ble
