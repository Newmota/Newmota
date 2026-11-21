/*
  ble_disabled.c - Inert BLE ABI hooks for Beken Wi-Fi archives

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_disabled.c
 * @brief Inert BLE ABI hooks for Beken Wi-Fi archives.
 *
 * The selected Beken Wi-Fi and calibration libraries retain references to
 * BLE coordination functions even when the BLE stack is not linked. These
 * stubs satisfy those archive interfaces only while USE_BLE is disabled.
 */

#include <stdbool.h>
#include <stdint.h>

uint8_t ble_switch_mac_sleeped = 0U;
/* Match the BDK BLE task's normal-mode TX index used by its cal archive. */
uint8_t tx_pwr_idx = 16U;

/** @brief Supplies the disabled BLE archive hook without changing radio state. */
void ble_switch_rf_to_wifi(void) {}

/** @brief Reports that Bluetooth coexistence PTA is disabled. */
bool ble_coex_pta_is_on(void) {
    return false;
}

/** @brief Supplies the disabled BLE control ABI and ignores its arguments. */
uint32_t ble_ctrl(uint32_t command, void* parameter) {
    (void)command;
    (void)parameter;
    return 0U;
}

/** @brief Reports that the disabled BLE stack is not in DUT mode. */
uint32_t ble_in_dut_mode(void) {
    return 0U;
}

/** @brief Reports that the disabled BLE task is not running. */
bool ble_thread_is_up(void) {
    return false;
}

bool ble_thread_is_busy(void) {
    return false;
}

void ble_thread_exit(void) {}

void ble_entry(void) {}

void ps_recover_ble_switch_mac_status(void) {}

#if PLATFORM_BK7231T || PLATFORM_BK7252
/* The BK7231U and BK7251 Wi-Fi archives also keep the BLE 4.2 scheduler ABI. */
uint8_t ble_active = 0U;

uint8_t if_ble_sleep(void) {
    return 0U;
}

uint32_t rwip_get_current_time(void) {
    return 0U;
}

uint32_t rwip_get_next_target_time(void) {
    return 0U;
}
#endif
