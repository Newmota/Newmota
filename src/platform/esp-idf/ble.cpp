/*
  ble.cpp - ESP-IDF NimBLE host start, security and introspection

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble.cpp
 * @brief ESP-IDF NimBLE host start, security policy, bond restore and introspection.
 *
 * Scanning lives in ble_scan.cpp and connections in ble_connection.cpp; each
 * has its own task so a blocking connection job never delays the scan.
 */

#include "ble_internal.h"

#include "esp_bt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <cstring>

// The SDK offers no header for the RAM/NVS bond store setup; its examples declare it too.
extern "C" void ble_store_config_init(void);

namespace newmota::platform {
namespace ble_detail {
std::uint8_t own_address_type = BLE_OWN_ADDR_PUBLIC;

namespace {
bool synced_state;
}

bool synced() {
    return synced_state;
}
} // namespace ble_detail

namespace {
bool started;

void on_reset(int) {
    ble_detail::synced_state = false;
}

void on_sync() {
    if (ble_hs_id_infer_auto(0, &ble_detail::own_address_type) != 0)
        ble_detail::own_address_type = BLE_OWN_ADDR_PUBLIC;
    ble_detail::synced_state = true;
    ble_detail::scan_on_sync();
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
} // namespace

bool ble_start() {
    if (started)
        return true;
    ble_detail::scan_init();
    ble_detail::connection_init();
    if (nimble_port_init() != 0)
        return false;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Bonding with secure connections into the RAM store; scripts persist the
    // bonds themselves through the "authenticated" event and BLE.store().
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set("newmota");
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    started = true;
    return true;
}

bool ble_stop() {
    if (!started)
        return true;
    if (!ble_detail::connection_shutdown())
        return false;
    ble_detail::scan_hold(true);
    // Returns once the host task has left nimble_port_run(); the controller goes with deinit.
    if (nimble_port_stop() != 0) {
        ble_detail::scan_hold(false);
        return false;
    }
    ble_detail::synced_state = false;
    started = false;
    (void)nimble_port_deinit();
    ble_detail::scan_hold(false); // scanning resumes for its consumers after the next sync
    return true;
}

bool ble_ready() {
    return started;
}

bool ble_store_bond(const std::uint8_t* record, std::size_t size) {
    constexpr std::size_t kRecord = sizeof(ble_store_value_sec);
    if (!started || size != 2 * kRecord)
        return false;
    ble_store_value_sec ours, peer;
    std::memcpy(&ours, record, kRecord);
    std::memcpy(&peer, record + kRecord, kRecord);
    return ble_store_write_our_sec(&ours) == 0 && ble_store_write_peer_sec(&peer) == 0;
}

void ble_info(BleInfo& info) {
    info = BleInfo{};
    if (!started)
        return;
    ble_detail::connection_fill(info);
    if (ble_detail::scan_running())
        info.roles |= 1;
    info.scan_stack_free = ble_detail::scan_stack_free();
    info.dropped_advertisements = ble_scan_dropped();
    info.dropped_events = ble_connection_dropped();
#ifdef CONFIG_BT_NIMBLE_EXT_ADV
    info.version = 5;
#else
    info.version = 4;
#endif
    info.mtu = ble_att_preferred_mtu();
    info.power = static_cast<std::int8_t>(
        (static_cast<int>(esp_ble_tx_power_get(ESP_BLE_PWR_TYPE_DEFAULT)) - ESP_PWR_LVL_N0) * 3);
    int bonds = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bonds) == 0)
        info.bonds = static_cast<std::uint8_t>(bonds);
    std::uint8_t address[6]{};
    const bool random = ble_detail::own_address_type == BLE_OWN_ADDR_RANDOM ||
                        ble_detail::own_address_type == BLE_OWN_ADDR_RPA_RANDOM_DEFAULT;
    if (ble_hs_id_copy_addr(random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC, address, nullptr) == 0)
        ble_detail::reverse_address(address, info.local_address);
}
} // namespace newmota::platform
