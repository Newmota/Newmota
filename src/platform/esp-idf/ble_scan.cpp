/*
  ble_scan.cpp - ESP-IDF NimBLE scan task

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_scan.cpp
 * @brief ESP-IDF NimBLE scan task and advertisement slot.
 *
 * The task only decides when the radio scans: while a consumer wants it, no link
 * exists and nothing holds it. Reports are copied by the host's callback into a
 * single slot; a scannable advertisement waits briefly so its scan response can
 * be appended, the way the legacy driver presented it.
 */

#include "ble_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace newmota::platform {
namespace {
constexpr std::uint32_t kStackBytes = 2048;
constexpr std::size_t kWatchLimit = 8;
constexpr TickType_t kMergeWindow = pdMS_TO_TICKS(30);

enum class Slot : std::uint8_t { empty, merging, ready };

struct Scan {
    TaskHandle_t task; ///< Created on first demand and kept: host callbacks may wake it.
    std::atomic<std::uint8_t> consumers;
    std::atomic<int> links;
    std::atomic<bool> hold;
    std::atomic<bool> running;
    std::atomic<bool> dirty; ///< The watch list or parameters changed; restart to apply them.
    std::atomic<std::uint32_t> dropped;
    BleScanParameters parameters{kBleScanDefaults};
    ble_addr_t watch[kWatchLimit];
    std::uint8_t watch_count;
    Slot slot;
    TickType_t since;
    BleAdvertisement advertisement;
};

Scan scan{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief Promotes an advertisement that waited long enough; the lock is held. */
[[nodiscard]] bool ready_locked() {
    if (scan.slot == Slot::merging && xTaskGetTickCount() - scan.since >= kMergeWindow)
        scan.slot = Slot::ready;
    return scan.slot == Slot::ready;
}

void report(const ble_gap_disc_desc& report) {
    const bool response = report.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    std::uint8_t address[6];
    ble_detail::reverse_address(report.addr.val, address);
    const std::size_t length = std::min<std::size_t>(report.length_data, 31);
    portENTER_CRITICAL(&lock);
    auto& slot = scan.advertisement;
    switch (scan.slot) {
    case Slot::ready:
        ++scan.dropped;
        break;
    case Slot::merging:
        if (response && std::memcmp(slot.address, address, 6) == 0 &&
            slot.address_type == report.addr.type) {
            std::memcpy(slot.payload + slot.length, report.data, length);
            slot.length = static_cast<std::uint8_t>(slot.length + length);
        } else {
            ++scan.dropped; // the new report waits for the next free slot
        }
        scan.slot = Slot::ready;
        break;
    case Slot::empty:
        std::memcpy(slot.address, address, 6);
        slot.address_type = report.addr.type;
        slot.rssi = report.rssi;
        slot.length = static_cast<std::uint8_t>(length);
        std::memcpy(slot.payload, report.data, length);
        scan.since = xTaskGetTickCount();
        scan.slot =
            scan.parameters.active && (report.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                                          report.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND)
                ? Slot::merging
                : Slot::ready;
        break;
    }
    portEXIT_CRITICAL(&lock);
}

void wake() {
    if (scan.task != nullptr)
        xTaskNotifyGive(scan.task);
}

int on_scan_event(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        report(event->disc);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        scan.running = false; // the radio ended it; the task restarts it if still wanted
        wake();
    }
    return 0;
}

[[nodiscard]] bool wanted() {
    return scan.consumers != 0 && !scan.hold && scan.links == 0 && ble_detail::synced();
}

void stop() {
    if (!scan.running)
        return;
    (void)ble_gap_disc_cancel();
    scan.running = false;
    vTaskDelay(pdMS_TO_TICKS(20)); // the host needs a moment before it accepts another scan
}

void apply() {
    if (scan.running && (!wanted() || scan.dirty))
        stop();
    if (!wanted() || scan.running)
        return;
    ble_gap_disc_params parameters{};
    parameters.filter_duplicates = 0;
    ble_addr_t watch[kWatchLimit];
    portENTER_CRITICAL(&lock);
    const std::uint8_t count = scan.watch_count;
    std::memcpy(watch, scan.watch, sizeof(watch));
    parameters.passive = scan.parameters.active ? 0 : 1;
    parameters.itvl = scan.parameters.interval;
    parameters.window = scan.parameters.window;
    scan.dirty = false;
    portEXIT_CRITICAL(&lock);
    parameters.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
    if (count != 0 && ble_gap_wl_set(watch, count) == 0)
        parameters.filter_policy = BLE_HCI_SCAN_FILT_USE_WL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (ble_gap_disc(ble_detail::own_address_type, BLE_HS_FOREVER, &parameters, on_scan_event,
                nullptr) == 0) {
            scan.running = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void task(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        apply();
    }
}
} // namespace

namespace ble_detail {
void scan_init() {
    // A restarted host has no links and no scan; consumers and the watch list remain.
    scan.links = 0;
    scan.running = false;
    scan.dirty = true;
}

void scan_on_sync() {
    wake();
}

void scan_links(int delta) {
    scan.links += delta;
    wake();
}

void scan_hold(bool hold) {
    scan.hold = hold;
    if (hold && scan.running) {
        (void)ble_gap_disc_cancel(); // synchronous: callers need the radio free now
        scan.running = false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    wake();
}

bool scan_running() {
    return scan.running;
}

std::uint16_t scan_stack_free() {
    return scan.task == nullptr
               ? 0
               : static_cast<std::uint16_t>(uxTaskGetStackHighWaterMark(scan.task));
}
} // namespace ble_detail

void ble_scan_set(BleScanConsumer consumer, bool on) {
    const auto bit = static_cast<std::uint8_t>(consumer);
    if (on)
        scan.consumers |= bit;
    else
        scan.consumers &= static_cast<std::uint8_t>(~bit);
    if (scan.consumers != 0 && scan.task == nullptr)
        (void)xTaskCreate(task, "ble_scan", kStackBytes, nullptr, 1, &scan.task);
    wake();
}

bool ble_scan_configure(const BleScanParameters& parameters) {
    if (!ble_scan_valid(parameters))
        return false;
    portENTER_CRITICAL(&lock);
    scan.parameters = parameters;
    scan.dirty = true;
    portEXIT_CRITICAL(&lock);
    wake();
    return true;
}

BleScanParameters ble_scan_parameters() {
    portENTER_CRITICAL(&lock);
    const BleScanParameters parameters = scan.parameters;
    portEXIT_CRITICAL(&lock);
    return parameters;
}

bool ble_scan_watch(const std::uint8_t* address, std::uint8_t address_type) {
    ble_addr_t entry{};
    entry.type = address_type;
    ble_detail::reverse_address(address, entry.val);
    bool known = false, ok = true;
    portENTER_CRITICAL(&lock);
    for (std::uint8_t i = 0; i < scan.watch_count; ++i)
        if (scan.watch[i].type == entry.type && std::memcmp(scan.watch[i].val, entry.val, 6) == 0)
            known = true;
    if (!known) {
        if (scan.watch_count < kWatchLimit) {
            scan.watch[scan.watch_count++] = entry;
            scan.dirty = true;
        } else {
            ok = false;
        }
    }
    portEXIT_CRITICAL(&lock);
    wake();
    return ok;
}

void ble_scan_unwatch() {
    portENTER_CRITICAL(&lock);
    scan.watch_count = 0;
    scan.dirty = true;
    portEXIT_CRITICAL(&lock);
    wake();
}

bool ble_scan_watched(std::size_t index, std::uint8_t* address, std::uint8_t& address_type) {
    portENTER_CRITICAL(&lock);
    const bool present = index < scan.watch_count;
    ble_addr_t entry{};
    if (present)
        entry = scan.watch[index];
    portEXIT_CRITICAL(&lock);
    if (present) {
        ble_detail::reverse_address(entry.val, address);
        address_type = entry.type;
    }
    return present;
}

bool ble_scan_pending() {
    portENTER_CRITICAL(&lock);
    const bool pending = ready_locked();
    portEXIT_CRITICAL(&lock);
    return pending;
}

bool ble_scan_take(BleAdvertisement& advertisement) {
    portENTER_CRITICAL(&lock);
    const bool pending = ready_locked();
    if (pending) {
        advertisement = scan.advertisement;
        scan.slot = Slot::empty;
    }
    portEXIT_CRITICAL(&lock);
    return pending;
}

std::uint32_t ble_scan_dropped() {
    return scan.dropped;
}
} // namespace newmota::platform
