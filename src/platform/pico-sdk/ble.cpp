/*
  ble.cpp - Pico native BTstack BLE adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble.cpp
 * @brief Bounded native CYW43 BLE; calls and snapshots share the SDK async lock.
 *
 * SDK callbacks never enter Berry. The cooperative caller polls owned events.
 * The host uses RAM security storage and never claims the SDK flash TLV banks.
 */
#include "newmota/platform/ble.h"
#include "newmota/platform/ble_cache.h"
#include "ble_bond.h"

#include <algorithm>
#include <cstdlib>
#include <new>
#include <cstring>

#include "btstack.h"
#include "ble/att_db_util.h"
#include "ble/le_device_db.h"
#include "pico/btstack_cyw43.h"
#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"

/** @brief SDK architecture hook, with RAM bonds instead of implicit flash TLV storage. */
extern "C" bool btstack_cyw43_init(async_context_t* context) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
    hci_init(hci_transport_cyw43_instance(), nullptr);
    return true;
}

/** @brief Completes the SDK architecture's matching native host lifecycle. */
extern "C" void btstack_cyw43_deinit(async_context_t*) {
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    btstack_run_loop_async_context_deinit();
    btstack_run_loop_deinit();
    btstack_memory_deinit();
}

/** @brief Native RAM allocation does not reset encryption fields in a reused slot. */
extern "C" int __real_le_device_db_add(int type, bd_addr_t address, sm_key_t irk);

extern "C" int __wrap_le_device_db_add(int type, bd_addr_t address, sm_key_t irk) {
    const int index = __real_le_device_db_add(type, address, irk);
    if (index >= 0) {
        std::uint8_t random[8]{}, ltk[16]{};
        le_device_db_encryption_set(index, 0, random, ltk, 0, 0, 0, 0);
    }
    return index;
}

namespace newmota::platform {
namespace {
constexpr unsigned kServices = 4, kCharacteristics = 12, kSubscriptions = 8, kWatches = 8;
constexpr hci_con_handle_t kNoLink = HCI_CON_HANDLE_INVALID;
constexpr std::uint8_t kBase[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0};

/** @brief Serializes caller access with both polling and background CYW43 callbacks. */
struct Guard {
    Guard() { async_context_acquire_lock_blocking(cyw43_arch_async_context()); }

    ~Guard() { async_context_release_lock(cyw43_arch_async_context()); }
};

struct Item {
    BleEvent event{};
    std::uint8_t data[255]{};
};

struct Characteristic {
    std::uint8_t service{}, uuid[16]{}, value[255]{}, length{};
    std::uint16_t flags{}, handle{}, ccc{};
    bool sending{}, indication{};
    std::uint8_t sent[255]{};
    std::uint16_t sent_length{};
    btstack_context_callback_registration_t callback{};
};

struct Server {
    std::uint8_t services[kServices][16]{};
    Characteristic characteristics[kCharacteristics]{};
    unsigned service_count{}, characteristic_count{};
    bool registered{}, enabled{}, advertising{};
    std::uint8_t mode{2}, address_type{}, address[6]{}, ad[31]{}, response[31]{};
    std::uint8_t ad_length{}, response_length{};
    std::uint16_t interval_min{0x100}, interval_max{0x200}, name_handle{};
    char name[249]{"Newmota"};
    std::uint16_t name_length{7};
    std::uint8_t peer[6]{}, peer_type{};
};

struct Subscription {
    bool used{};
    std::uint16_t handle{}, uuid{};
    gatt_client_notification_t listener{};
};
namespace cache = ble_cache;

/** @brief What a cached characteristic needs to skip its discovery, including CCC bounds. */
struct CachedCharacteristic {
    std::uint16_t start, value, end;
    std::uint8_t properties;
};

enum class Phase {
    idle,
    connect,
    watch_find,
    watch_enable,
    service,
    characteristic,
    operation,
    security,
    disconnect
};

struct State {
    bool started{}, ready{}, failed{}, opened{}, running{}, completed{}, closing{}, restarting{},
        starting{}, restoring{}, off{};
    bool scanning{}, connecting{}, master{}, retried{}, overflow{}, drain{}, found{};
    unsigned consumers{}, watch_count{}, event_head{}, event_count{}, sub_index{};
    std::uint32_t advertisement_drops{}, event_drops{}, deadline{}, advertisement_time{};
    bool advertisement_present{}, advertisement_waiting{};
    BleAdvertisement advertisement{}, watches[kWatches]{};
    BleScanParameters scan_parameters{kBleScanDefaults};
    BleRequest request{};
    BleEvent completion{};
    Item events[4]{}, authenticated[4]{};
    bool authenticated_present[4]{};
    BleConnectionInfo connection{};
    hci_con_handle_t link{kNoLink};
    Phase phase{Phase::idle}, retry_phase{Phase::idle};
    std::uint16_t used{}, result_handle{}, result_uuid{};
    std::uint8_t count{}, peer_type{}, local_address[6]{};
    std::uint16_t last_error{};
    std::uint8_t last_operation{};
    std::uint32_t pin{}, startup_deadline{}, rssi_deadline{};
    gatt_client_service_t service{};
    gatt_client_characteristic_t characteristic{};
    Subscription subscriptions[kSubscriptions]{};
    // Discovery cache; link events and Service Changed indications advance the generation.
    cache::Cache<CachedCharacteristic> cache{};
    std::uint32_t cache_generation{}, lookup_generation{};
    unsigned attempts{};
    int service_index{};
    std::uint16_t service_changed{}; ///< Its handle while indicated to us, or 0.
    bool watch_listening{};
    gatt_client_characteristic_t watch_characteristic{};
    gatt_client_notification_t watch_listener{};
    Server* server{};
    sm_key_t er{}, ir{};
    btstack_packet_callback_registration_t hci{}, sm{};
    btstack_timer_source_t timer{};
} st;

/** @brief Guards scan settings that callers may change before the host starts; until then only
 *  the application loop touches them.
 */
struct Lock {
    const bool held = st.started;

    Lock() {
        if (held)
            async_context_acquire_lock_blocking(cyw43_arch_async_context());
    }

    ~Lock() {
        if (held)
            async_context_release_lock(cyw43_arch_async_context());
    }
};

std::uint32_t now() {
    return btstack_run_loop_get_time_ms();
}

bool expired(std::uint32_t deadline) {
    return static_cast<std::int32_t>(now() - deadline) >= 0;
}

std::uint16_t short_uuid(const std::uint8_t* uuid) {
    return uuid[12] | (uuid[13] << 8);
}

void uuid_native(const std::uint8_t* little, std::uint8_t* big) {
    reverse_bytes(little, big, 16);
}

/** @brief Empties the cache when the link changed since it was filled, or on request. */
void cache_forget(bool always = false) {
    st.cache.forget(st.cache_generation, always);
}

void watch_stop() {
    if (st.watch_listening)
        gatt_client_stop_listening_for_characteristic_value_updates(&st.watch_listener);
    st.watch_listening = false;
    st.service_changed = 0;
}

/** @brief A link came or went: nothing learned about the previous peer's database still holds.
 *  Native listeners outlive the link, so the watch's listener goes too.
 */
void link_changed() {
    watch_stop();
    ++st.cache_generation;
}

void post(std::uint8_t operation, std::uint16_t uuid = 0, std::uint16_t handle = 0,
    const void* data = nullptr, std::size_t length = 0, std::uint16_t error = 0) {
    if (!st.opened || st.closing)
        return;
    if (operation == 230 && length <= 255) {
        if (handle >= 4) {
            ++st.event_drops;
            return;
        }
        auto& item = st.authenticated[handle];
        item.event = {0, operation, uuid, 0, static_cast<std::uint16_t>(length)};
        if (length)
            std::memcpy(item.data, data, length);
        st.authenticated_present[handle] = true;
        return;
    }
    if (length > 255 || st.event_count == 4) {
        ++st.event_drops;
        return;
    }
    auto& item = st.events[(st.event_head + st.event_count++) % 4];
    item.event = {error, operation, uuid, handle, static_cast<std::uint16_t>(length)};
    if (length)
        std::memcpy(item.data, data, length);
}

void advertising_start();

void scan_update() {
    if (st.server && st.server->advertising && st.running && st.request.operation < 201) {
        gap_advertisements_enable(0);
        st.server->advertising = false;
    }
    if (st.server && st.server->enabled && !st.server->advertising && st.ready &&
        st.link == kNoLink && !st.connecting && !st.running && !st.closing)
        advertising_start();
    const bool wanted = st.ready && st.consumers && st.link == kNoLink && !st.connecting &&
                        !st.running && !st.closing;
    if (wanted == st.scanning)
        return;
    st.scanning = wanted;
    if (wanted)
        gap_start_scan();
    else
        gap_stop_scan();
}

void complete(BleError error) {
    if (!st.running)
        return;
    if (error != BleError::none)
        cache_forget(true); // a changed peer database must not fail the next job too
    st.completion = {static_cast<std::uint16_t>(error), st.request.operation, st.result_uuid,
        st.result_handle, st.used};
    st.last_operation = st.request.operation;
    st.last_error = static_cast<std::uint16_t>(error);
    if (st.request.one_shot && st.link != kNoLink && st.phase != Phase::disconnect) {
        st.phase = Phase::disconnect;
        st.deadline = now() + 5000;
        gap_disconnect(st.link);
        return;
    }
    st.running = false;
    st.completed = true;
    st.phase = Phase::idle;
    scan_update();
}

void append(const void* bytes, std::size_t size) {
    if (size > st.request.reply_capacity - std::min(st.used, st.request.reply_capacity)) {
        st.overflow = true;
        return;
    }
    if (size)
        std::memcpy(st.request.reply + st.used, bytes, size);
    st.used += size;
}

void append_uuid(const std::uint8_t* big, std::uint16_t uuid16) {
    std::uint8_t little[16];
    uuid_native(big, little);
    const std::uint8_t bits = uuid16 ? 16 : 128;
    append(&bits, 1);
    if (uuid16)
        append(&uuid16, 2);
    else
        append(little, 16);
}

void finish_query(BleError error = BleError::none) {
    if (st.overflow)
        error = BleError::buffer_too_small;
    complete(error);
}

void client_packet(std::uint8_t, std::uint16_t, std::uint8_t*, std::uint16_t);
void query_start();

void client_operation() {
    st.phase = Phase::operation;
    st.result_handle = st.characteristic.value_handle;
    st.result_uuid = st.characteristic.uuid16;
    std::uint8_t status = 0;
    switch (st.request.operation) {
    case 1:
        status = gatt_client_read_long_value_of_characteristic_using_value_handle(
            client_packet, st.link, st.result_handle);
        break;
    case 2:
        if (st.request.confirm) {
            std::uint16_t mtu = 23;
            gatt_client_get_mtu(st.link, &mtu);
            status = st.request.length <= mtu - 3
                         ? gatt_client_write_value_of_characteristic(client_packet, st.link,
                               st.result_handle, st.request.length,
                               const_cast<std::uint8_t*>(st.request.data))
                         : gatt_client_write_long_value_of_characteristic(client_packet, st.link,
                               st.result_handle, st.request.length,
                               const_cast<std::uint8_t*>(st.request.data));
        } else {
            status = gatt_client_write_value_of_characteristic_without_response(st.link,
                st.result_handle, st.request.length, const_cast<std::uint8_t*>(st.request.data));
            complete(status ? BleError::write_failed : BleError::none);
            return;
        }
        break;
    case 3:
    case 4: {
        unsigned slot = 0;
        while (slot < kSubscriptions &&
               (!st.subscriptions[slot].used || st.subscriptions[slot].handle != st.result_handle))
            ++slot;
        if (slot == kSubscriptions && st.request.operation == 4) {
            complete(BleError::none);
            return;
        }
        if (slot == kSubscriptions) {
            slot = 0;
            while (slot < kSubscriptions && st.subscriptions[slot].used)
                ++slot;
        }
        if (slot == kSubscriptions) {
            complete(BleError::cannot_notify);
            return;
        }
        if (st.request.operation == 4 && st.result_handle == st.service_changed) {
            // Unsubscribing from the Service Changed we watch only ends delivery to the script.
            gatt_client_stop_listening_for_characteristic_value_updates(
                &st.subscriptions[slot].listener);
            st.subscriptions[slot] = {};
            finish_query();
            return;
        }
        st.sub_index = slot;
        const auto configuration =
            st.request.operation == 4
                ? 0
                : ((st.characteristic.properties & ATT_PROPERTY_NOTIFY) ? 1 : 2);
        status = gatt_client_write_client_characteristic_configuration(
            client_packet, st.link, &st.characteristic, configuration);
        break;
    }
    default:
        complete(BleError::no_characteristic);
        return;
    }
    if (status)
        complete(st.request.operation == 1   ? BleError::cannot_read
                 : st.request.operation == 2 ? BleError::write_failed
                                             : BleError::cannot_notify);
}

bool short_form(const std::uint8_t* uuid) {
    return !std::memcmp(uuid, kBase, sizeof(kBase)) && uuid[14] == 0 && uuid[15] == 0;
}

/** @brief Whether a database change was announced since this lookup attempt began. */
bool lookup_overtaken() {
    return st.cache_generation != st.lookup_generation;
}

/** @brief Starts the next lookup attempt, or fails the job after three overtaken attempts. */
void lookup_retry(BleError error) {
    if (++st.attempts < 3)
        query_start();
    else
        complete(error == BleError::none ? BleError::no_characteristic : error);
}

void lookup_failed(BleError error) {
    if (lookup_overtaken())
        lookup_retry(error);
    else
        complete(error);
}

/** @brief Runs the operation once the characteristic's handles held throughout the lookup. */
void characteristic_ready() {
    if (lookup_overtaken())
        lookup_retry(BleError::none);
    else
        client_operation();
}

/** @brief Selects the characteristic in a cached service, asking the peer only on a miss. */
void use_service() {
    const auto range = st.cache.services[st.service_index].range;
    st.service = {};
    st.service.start_group_handle = range.start;
    st.service.end_group_handle = range.end;
    st.phase = Phase::characteristic;
    st.found = false;
    if (st.request.operation == 7) {
        const std::uint8_t zero = 0;
        append(&zero, 1);
        if (gatt_client_discover_characteristics_for_service(client_packet, st.link, &st.service))
            complete(BleError::no_characteristic);
        return;
    }
    if (const auto* cached =
            st.cache.find_characteristic(st.service_index, st.request.characteristic)) {
        st.characteristic = {};
        st.characteristic.start_handle = cached->start;
        st.characteristic.value_handle = cached->value;
        st.characteristic.end_handle = cached->end;
        st.characteristic.properties = cached->properties;
        st.characteristic.uuid16 =
            short_form(st.request.characteristic) ? short_uuid(st.request.characteristic) : 0;
        uuid_native(st.request.characteristic, st.characteristic.uuid128);
        characteristic_ready();
        return;
    }
    std::uint8_t uuid[16];
    uuid_native(st.request.characteristic, uuid);
    if (gatt_client_discover_characteristics_for_service_by_uuid128(
            client_packet, st.link, &st.service, uuid))
        complete(BleError::no_characteristic);
}

/** @brief Looks up the staged service, asking the peer only on a cache miss. */
void lookup_service() {
    const int index = st.cache.find_service(st.request.service);
    if (index >= 0) {
        st.service_index = index;
        use_service();
        return;
    }
    st.phase = Phase::service;
    st.found = false; // the watch may have found Service Changed
    std::uint8_t uuid[16];
    uuid_native(st.request.service, uuid);
    if (gatt_client_discover_primary_services_by_uuid128(client_packet, st.link, uuid))
        complete(BleError::no_service);
}

void watch_finish(cache::Watch watch) {
    cache_forget(); // a change during the check leaves an empty, unchecked cache
    st.cache.settle(watch);
    st.lookup_generation = st.cache_generation;
    lookup_service();
}

void watch_packet(std::uint8_t type, std::uint16_t, std::uint8_t* packet, std::uint16_t) {
    if (type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == GATT_EVENT_INDICATION &&
        gatt_event_indication_get_handle(packet) == st.link &&
        gatt_event_indication_get_value_handle(packet) == st.service_changed)
        ++st.cache_generation;
}

/** @brief Makes the peer indicate database changes to us, unless its database is fixed.
 *  Never pairs: its completions bypass the security retry.
 */
void watch_start() {
    if (st.service_changed != 0) {
        watch_finish(cache::Watch::valid);
        return;
    }
    st.phase = Phase::watch_find;
    st.found = false;
    if (gatt_client_discover_characteristics_for_handle_range_by_uuid16(
            client_packet, st.link, 1, 0xFFFF, cache::kServiceChangedUuid))
        watch_finish(cache::Watch::failed);
}

void watch_step(std::uint8_t status) {
    if (st.phase == Phase::watch_find) {
        if (status) {
            watch_finish(cache::Watch::failed);
            return;
        }
        if (!st.found) {
            watch_finish(cache::Watch::valid);
            return;
        }
        st.phase = Phase::watch_enable;
        // Set before the write: the first indication may follow at once.
        st.service_changed = st.watch_characteristic.value_handle;
        gatt_client_listen_for_characteristic_value_updates(
            &st.watch_listener, watch_packet, st.link, &st.watch_characteristic);
        st.watch_listening = true;
        if (!gatt_client_write_client_characteristic_configuration(client_packet, st.link,
                &st.watch_characteristic, GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_INDICATION))
            return;
    } else if (!status) {
        watch_finish(cache::Watch::rediscover);
        return;
    }
    watch_stop();
    watch_finish(cache::Watch::failed);
}

void query_start() {
    st.used = 0;
    st.count = 0;
    st.found = false;
    st.overflow = false;
    st.deadline = now() + 10000;
    st.lookup_generation = st.cache_generation;
    if (st.request.has_arg1 && st.request.operation <= 4 && st.request.arg1 > 0 &&
        st.request.arg1 <= 65535) {
        // Discover declarations so CCC lookup is bounded by the next characteristic,
        // even when Berry selects a value handle directly.
        st.service = {};
        st.service.start_group_handle = 1;
        st.service.end_group_handle = 65535;
        st.phase = Phase::characteristic;
        const auto status =
            gatt_client_discover_characteristics_for_service(client_packet, st.link, &st.service);
        if (status)
            complete(BleError::no_characteristic);
        return;
    }
    if (st.request.operation == 6) {
        st.phase = Phase::service;
        const std::uint8_t zero = 0;
        append(&zero, 1);
        if (gatt_client_discover_primary_services(client_packet, st.link))
            complete(BleError::no_service);
        return;
    }
    cache_forget();
    if (st.cache.unchecked())
        watch_start();
    else
        lookup_service();
}

void client_packet(std::uint8_t type, std::uint16_t, std::uint8_t* packet, std::uint16_t) {
    if (type != HCI_EVENT_PACKET)
        return;
    const auto event = hci_event_packet_get_type(packet);
    // All client events consumed here begin with the connection handle.
    if (gatt_event_query_complete_get_handle(packet) != st.link)
        return;
    if (event == GATT_EVENT_NOTIFICATION || event == GATT_EVENT_INDICATION) {
        // Both events have the same value-handle, length and payload layout.
        const auto handle = gatt_event_notification_get_value_handle(packet);
        for (auto& sub : st.subscriptions)
            if (sub.used && sub.handle == handle) {
                if (event == GATT_EVENT_INDICATION && sub.uuid == cache::kServiceChangedUuid)
                    ++st.cache_generation;
                post(103, sub.uuid, handle, gatt_event_notification_get_value(packet),
                    gatt_event_notification_get_value_length(packet));
                break;
            }
        return;
    }
    if (!st.running || st.closing || st.phase == Phase::disconnect)
        return;
    if (event == GATT_EVENT_SERVICE_QUERY_RESULT) {
        gatt_client_service_t service;
        gatt_event_service_query_result_get_service(packet, &service);
        if (st.request.operation == 6) {
            if (st.count == 255)
                st.overflow = true;
            else {
                append_uuid(service.uuid128, service.uuid16);
                ++st.count;
            }
        } else if (!st.found) {
            st.service = service;
            st.found = true;
        }
    } else if (event == GATT_EVENT_CHARACTERISTIC_QUERY_RESULT) {
        gatt_client_characteristic_t characteristic;
        gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
        if (st.phase == Phase::watch_find) {
            if (!st.found) {
                st.watch_characteristic = characteristic;
                st.found = true;
            }
        } else if (st.request.operation == 7) {
            if (st.count == 255)
                st.overflow = true;
            else {
                append_uuid(characteristic.uuid128, characteristic.uuid16);
                const std::uint8_t properties = characteristic.properties;
                append(&properties, 1);
                append(&characteristic.value_handle, 2);
                ++st.count;
            }
        } else if (!st.found &&
                   (!st.request.has_arg1 || characteristic.value_handle == st.request.arg1)) {
            st.characteristic = characteristic;
            st.found = true;
        }
    } else if (event == GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT) {
        append(gatt_event_long_characteristic_value_query_result_get_value(packet),
            gatt_event_long_characteristic_value_query_result_get_value_length(packet));
    } else if (event == GATT_EVENT_QUERY_COMPLETE) {
        const auto status = gatt_event_query_complete_get_att_status(packet);
        if (st.phase == Phase::watch_find || st.phase == Phase::watch_enable) {
            watch_step(status);
            return;
        }
        if ((status == ATT_ERROR_INSUFFICIENT_AUTHENTICATION ||
                status == ATT_ERROR_INSUFFICIENT_ENCRYPTION) &&
            !st.retried) {
            st.retried = true;
            st.retry_phase = st.phase;
            st.phase = Phase::security;
            st.deadline = now() + 30000;
            sm_request_pairing(st.link);
            return;
        }
        if (status) {
            complete(st.request.operation == 1   ? BleError::cannot_read
                     : st.request.operation == 2 ? BleError::write_failed
                                                 : BleError::cannot_notify);
            return;
        }
        if (st.phase == Phase::service && st.request.operation != 6) {
            if (!st.found) {
                lookup_failed(BleError::no_service);
                return;
            }
            st.service_index = st.cache.add_service(
                st.request.service, {st.service.start_group_handle, st.service.end_group_handle});
            use_service();
        } else if (st.phase == Phase::characteristic && st.request.operation != 7) {
            if (!st.found) {
                lookup_failed(BleError::no_characteristic);
                return;
            }
            if (!st.request.has_arg1)
                st.cache.add_characteristic(st.service_index, st.request.characteristic,
                    {st.characteristic.start_handle, st.characteristic.value_handle,
                        st.characteristic.end_handle,
                        static_cast<std::uint8_t>(st.characteristic.properties)});
            characteristic_ready();
        } else {
            if (st.request.operation == 7 && lookup_overtaken()) {
                lookup_retry(BleError::none);
                return;
            }
            if (st.request.operation == 6 || st.request.operation == 7) {
                if (st.request.reply_capacity)
                    st.request.reply[0] = st.count;
            }
            if (st.request.operation == 3 || st.request.operation == 4) {
                auto& sub = st.subscriptions[st.sub_index];
                if (sub.used)
                    gatt_client_stop_listening_for_characteristic_value_updates(&sub.listener);
                sub.used = st.request.operation == 3;
                sub.handle = st.result_handle;
                sub.uuid = st.result_uuid;
                if (sub.used)
                    gatt_client_listen_for_characteristic_value_updates(
                        &sub.listener, client_packet, st.link, &st.characteristic);
            }
            finish_query();
        }
    }
}

using Bond = pico_ble::BondRecord;
constexpr char kRevision[] = NEWMOTA_PICO_BLE_SDK_REVISION;
static_assert(sizeof(kRevision) == 41);

void export_bond() {
    const int index = sm_le_device_index(st.link);
    if (index < 0 || !gap_bonded(st.link))
        return;
    Bond bond{};
    std::memcpy(bond.magic, "PBK1", 4);
    std::memcpy(bond.revision, kRevision, 40);
    gap_local_bd_addr(bond.local);
    if (st.server) {
        bond.own_type = st.server->address_type;
        std::memcpy(bond.own_address, st.server->address, 6);
    }
    int type, size, authenticated, authorized, sc;
    le_device_db_info(index, &type, bond.peer, bond.irk);
    bond.peer_type = type;
    std::uint16_t ediv;
    le_device_db_encryption_get(
        index, &ediv, bond.random, bond.ltk, &size, &authenticated, &authorized, &sc);
    little_endian_store_16(bond.ediv, 0, ediv);
    bond.key_size = size;
    bond.authenticated = authenticated;
    bond.authorized = authorized;
    bond.secure_connections = sc;
    std::memcpy(bond.er, st.er, 16);
    std::memcpy(bond.ir, st.ir, 16);
    post(230, 0, static_cast<std::uint16_t>(index), &bond, sizeof(bond));
}

std::uint16_t read_value(hci_con_handle_t, std::uint16_t handle, std::uint16_t offset,
    std::uint8_t* buffer, std::uint16_t capacity) {
    if (!st.server || !st.server->enabled)
        return 0;
    if (st.server->name_handle == handle)
        return att_read_callback_handle_blob(reinterpret_cast<std::uint8_t*>(st.server->name),
            st.server->name_length, offset, buffer, capacity);
    for (auto& c : st.server->characteristics) {
        if (!c.handle)
            continue;
        if (c.handle == handle) {
            if (buffer)
                post(221, short_uuid(c.uuid), handle);
            return att_read_callback_handle_blob(c.value, c.length, offset, buffer, capacity);
        }
        if (c.handle + 1 == handle && (c.flags & 0x30))
            return att_read_callback_handle_little_endian_16(c.ccc, offset, buffer, capacity);
    }
    return 0;
}

int write_value(hci_con_handle_t, std::uint16_t handle, std::uint16_t transaction,
    std::uint16_t offset, std::uint8_t* bytes, std::uint16_t length) {
    if (!st.server || !st.server->enabled)
        return ATT_ERROR_WRITE_NOT_PERMITTED;
    if (transaction != ATT_TRANSACTION_MODE_NONE)
        return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    if (offset)
        return ATT_ERROR_INVALID_OFFSET;
    for (auto& c : st.server->characteristics) {
        if (!c.handle)
            continue;
        if (c.handle == handle) {
            if (length > sizeof(c.value))
                return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
            if (length)
                std::memcpy(c.value, bytes, length);
            c.length = length;
            post(222, short_uuid(c.uuid), handle, bytes, length);
            return 0;
        }
        if (c.handle + 1 == handle && (c.flags & 0x30)) {
            if (length != 2)
                return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
            const auto ccc = little_endian_read_16(bytes, 0);
            const auto permitted = ((c.flags & 0x10) ? 1 : 0) | ((c.flags & 0x20) ? 2 : 0);
            if (ccc & ~permitted)
                return ATT_ERROR_VALUE_NOT_ALLOWED;
            c.ccc = ccc;
            post(static_cast<std::uint8_t>(223 + ccc), short_uuid(c.uuid), c.handle);
            return 0;
        }
    }
    return ATT_ERROR_INVALID_HANDLE;
}

void send_value(void* context) {
    auto& c = *static_cast<Characteristic*>(context);
    if (!st.server || !st.server->enabled || st.link == kNoLink || st.closing) {
        c.sending = false;
        return;
    }
    const auto status = c.indication ? att_server_indicate(st.link, c.handle, c.sent, c.sent_length)
                                     : att_server_notify(st.link, c.handle, c.sent, c.sent_length);
    if (!c.indication || status) {
        c.sending = false;
        const int result = status;
        post(229, short_uuid(c.uuid), c.handle, &result, sizeof(result));
    }
}

bool server_allocate() {
    if (st.server)
        return true;
    st.server = new (std::nothrow) Server;
    return st.server != nullptr;
}

bool register_server() {
    auto& server = *st.server;
    if (server.registered) {
        server.enabled = true;
        return true;
    }
    att_db_util_init();
    // Device Name is prepared locally; Berry never runs inside an ATT request.
    att_db_util_add_service_uuid16(0x1800);
    server.name_handle = att_db_util_add_characteristic_uuid16(0x2a00,
        ATT_PROPERTY_READ | ATT_PROPERTY_DYNAMIC, ATT_SECURITY_NONE, ATT_SECURITY_NONE, nullptr, 0);
    for (unsigned s = 0; s < server.service_count; ++s) {
        std::uint8_t uuid[16];
        uuid_native(server.services[s], uuid);
        att_db_util_add_service_uuid128(uuid);
        for (unsigned i = 0; i < server.characteristic_count; ++i) {
            auto& c = server.characteristics[i];
            if (c.service != s)
                continue;
            uuid_native(c.uuid, uuid);
            const auto read = (c.flags & 0x400)   ? ATT_SECURITY_AUTHENTICATED
                              : (c.flags & 0x200) ? ATT_SECURITY_ENCRYPTED
                                                  : ATT_SECURITY_NONE;
            const auto write = (c.flags & 0x2000)   ? ATT_SECURITY_AUTHENTICATED
                               : (c.flags & 0x1000) ? ATT_SECURITY_ENCRYPTED
                                                    : ATT_SECURITY_NONE;
            c.handle = att_db_util_add_characteristic_uuid128(
                uuid, (c.flags & 0x3f) | ATT_PROPERTY_DYNAMIC, read, write, nullptr, 0);
            if (!c.handle) {
                st.failed = true;
                return false;
            }
        }
    }
    att_set_db(att_db_util_get_address());
    server.registered = true;
    server.enabled = true;
    return true;
}

void advertising_start() {
    auto& s = *st.server;
    if (!st.ready || !s.enabled || st.link != kNoLink || st.closing)
        return;
    std::uint8_t peer[6];
    std::memcpy(peer, s.peer, 6);
    gap_advertisements_set_params(s.interval_min, s.interval_max,
        s.mode == 0   ? (s.response_length ? 2 : 3)
        : s.mode == 1 ? 1
                      : 0,
        s.peer_type, peer, 7, 0);
    gap_advertisements_set_data(s.ad_length, s.ad);
    gap_scan_response_set_data(s.response_length, s.response);
    gap_advertisements_enable(1);
    s.advertising = true;
}

BleError server_job() {
    if (!server_allocate())
        return BleError::no_service;
    auto& s = *st.server;
    const auto& r = st.request;
    if (r.operation != 201 && r.operation != 202 && r.operation != 211)
        return BleError::no_characteristic;
    if (r.operation == 201 || r.operation == 202) {
        if (r.length > 31)
            return BleError::buffer_too_small;
        if (!register_server())
            return BleError::no_service;
        if (r.operation == 201) {
            if (r.length)
                std::memcpy(s.ad, r.data, r.length);
            s.ad_length = r.length;
        } else {
            if (r.length)
                std::memcpy(s.response, r.data, r.length);
            s.response_length = r.length;
        }
        std::memcpy(s.peer, r.address, 6);
        s.peer_type = r.address_type & 1;
        advertising_start();
        return BleError::none;
    }
    if (r.length > 255)
        return BleError::buffer_too_small;
    unsigned service = 0;
    while (service < s.service_count && std::memcmp(s.services[service], r.service, 16))
        ++service;
    if (service == s.service_count) {
        if (s.registered || service == kServices)
            return BleError::no_service;
        std::memcpy(s.services[s.service_count++], r.service, 16);
    }
    unsigned index = 0;
    while (index < s.characteristic_count &&
           (s.characteristics[index].service != service ||
               std::memcmp(s.characteristics[index].uuid, r.characteristic, 16)))
        ++index;
    const auto flags =
        static_cast<std::uint16_t>(r.has_arg1 ? r.arg1 : 2 | 0x10 | 0x20 | (r.confirm ? 8 : 4));
    if (index == s.characteristic_count) {
        if (s.registered || index == kCharacteristics)
            return BleError::no_characteristic;
        if (flags & (0x40 | 0x80 | 0x100 | 0x800 | 0x4000 | 0x8000))
            return BleError::cannot_write;
        auto& c = s.characteristics[index];
        c.service = service;
        c.flags = flags;
        std::memcpy(c.uuid, r.characteristic, 16);
        ++s.characteristic_count;
    } else if (r.has_arg1 && flags != s.characteristics[index].flags) {
        return BleError::no_characteristic;
    }
    auto& c = s.characteristics[index];
    if (c.sending)
        return BleError::cannot_notify;
    if (r.length)
        std::memcpy(c.value, r.data, r.length);
    c.length = r.length;
    st.result_handle = c.handle;
    st.result_uuid = short_uuid(c.uuid);
    if (!s.registered || !c.ccc || st.link == kNoLink)
        return BleError::none;
    c.indication = (c.ccc & 1) == 0;
    c.sent_length = c.length;
    std::memcpy(c.sent, c.value, c.length);
    c.callback.callback = send_value;
    c.callback.context = &c;
    c.sending = true;
    const auto status = c.indication
                            ? att_server_request_to_send_indication(&c.callback, st.link)
                            : att_server_request_to_send_notification(&c.callback, st.link);
    if (status) {
        c.sending = false;
        return BleError::cannot_notify;
    }
    return BleError::none;
}

void server_clear() {
    if (!st.server)
        return;
    // Callback registrations are retained until disconnect drains the native queues.
    // Local identity, name and advertising policy survive VM teardown and bonds.
    auto& server = *st.server;
    for (auto& c : server.characteristics)
        c = Characteristic{};
    std::memset(server.services, 0, sizeof(server.services));
    server.service_count = 0;
    server.characteristic_count = 0;
    server.registered = false;
    server.enabled = false;
    server.advertising = false;
    server.name_handle = 0;
    server.ad_length = 0;
    server.response_length = 0;
    std::memset(server.ad, 0, sizeof(server.ad));
    std::memset(server.response, 0, sizeof(server.response));
    att_db_util_init();
    att_set_db(att_db_util_get_address());
}

void packet_handler(std::uint8_t type, std::uint16_t, std::uint8_t* packet, std::uint16_t) {
    if (type != HCI_EVENT_PACKET)
        return;
    const auto event = hci_event_packet_get_type(packet);
    switch (event) {
    case BTSTACK_EVENT_STATE:
        st.ready = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
        if (st.ready) {
            st.starting = false;
            st.restoring = false;
            gap_local_bd_addr(st.local_address);
        }
        if (!st.ready)
            st.scanning = false;
        if (btstack_event_state_get_state(packet) == HCI_STATE_OFF && st.restarting) {
            st.restarting = false;
            hci_power_control(HCI_POWER_ON);
        }
        if (st.ready && st.server && st.server->enabled)
            advertising_start();
        scan_update();
        break;
    case GAP_EVENT_ADVERTISING_REPORT: {
        if (!st.scanning)
            break;
        std::uint8_t address[6];
        gap_event_advertising_report_get_address(packet, address);
        const auto address_type = gap_event_advertising_report_get_address_type(packet);
        bool watched = st.watch_count == 0;
        for (unsigned i = 0; i < st.watch_count; ++i)
            if (st.watches[i].address_type == address_type &&
                !std::memcmp(st.watches[i].address, address, 6))
                watched = true;
        if (!watched)
            break;
        const auto length =
            std::min<unsigned>(31, gap_event_advertising_report_get_data_length(packet));
        const auto report_type = gap_event_advertising_report_get_advertising_event_type(packet);
        if (st.advertisement_present) {
            if (st.advertisement_waiting && report_type == 4 &&
                st.advertisement.address_type == address_type &&
                !std::memcmp(st.advertisement.address, address, 6)) {
                std::memcpy(st.advertisement.payload + st.advertisement.length,
                    gap_event_advertising_report_get_data(packet), length);
                st.advertisement.length += length;
                st.advertisement_waiting = false;
            } else
                ++st.advertisement_drops;
            break;
        }
        std::memcpy(st.advertisement.address, address, 6);
        st.advertisement.address_type = address_type;
        st.advertisement.rssi = gap_event_advertising_report_get_rssi(packet);
        st.advertisement.length = length;
        std::memcpy(
            st.advertisement.payload, gap_event_advertising_report_get_data(packet), length);
        st.advertisement_present = true;
        st.advertisement_waiting =
            st.scan_parameters.active && (report_type == 0 || report_type == 2);
        st.advertisement_time = now() + 40;
        break;
    }
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_BONDING_DELETED &&
            gap_subevent_bonding_deleted_get_handle(packet) == st.link) {
            // SM is about to refill an existing same-peer slot. Its omitted key
            // fields must not retain the previous pairing's remote LTK metadata.
            const auto index = gap_subevent_bonding_deleted_get_index(packet);
            if (index < le_device_db_max_count()) {
                std::uint8_t random[8]{}, ltk[16]{};
                le_device_db_encryption_set(index, 0, random, ltk, 0, 0, 0, 0);
            }
        } else if (hci_event_gap_meta_get_subevent_code(packet) ==
                   GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
            const auto status = gap_subevent_le_connection_complete_get_status(packet);
            const bool master = gap_subevent_le_connection_complete_get_role(packet) == 0;
            if (status && !master) {
                if (st.server)
                    st.server->advertising = false;
                // Directed advertising timeout is a peripheral event, never a
                // completion for an unrelated outbound connection attempt.
                break;
            }
            st.connecting = false;
            if (status) {
                if (st.closing) {
                    server_clear();
                    st.closing = false;
                }
                if (st.running && st.phase == Phase::disconnect) {
                    st.running = false;
                    st.completed = true;
                    st.phase = Phase::idle;
                } else if (st.running)
                    complete(BleError::no_connect);
                scan_update();
                break;
            }
            st.link = gap_subevent_le_connection_complete_get_connection_handle(packet);
            link_changed();
            st.master = gap_subevent_le_connection_complete_get_role(packet) == 0;
            st.peer_type = gap_subevent_le_connection_complete_get_peer_address_type(packet);
            st.connection = {};
            st.connection.present = true;
            st.connection.master = st.master;
            gap_subevent_le_connection_complete_get_peer_address(
                packet, st.connection.peer_address);
            std::memcpy(st.connection.peer_id_address, st.connection.peer_address, 6);
            st.connection.mtu = 23;
            if (st.server)
                st.server->advertising = false;
            if (st.closing) {
                gap_disconnect(st.link);
                break;
            }
            if (!st.master) {
                std::uint8_t little[6];
                reverse_bytes(st.connection.peer_address, little, 6);
                post(227, 0, 0, little, 6);
            }
            if (st.running && st.phase == Phase::connect) {
                if (std::memcmp(st.request.address, st.connection.peer_address, 6) ||
                    (st.request.address_type & 1) != (st.peer_type & 1))
                    complete(BleError::no_connect);
                else
                    query_start();
            }
            scan_update();
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) != st.link)
            break;
        st.link = kNoLink;
        st.connection = {};
        link_changed();
        for (auto& sub : st.subscriptions) {
            if (sub.used)
                gatt_client_stop_listening_for_characteristic_value_updates(&sub.listener);
            sub = {};
        }
        if (st.server)
            for (auto& c : st.server->characteristics) {
                c.ccc = 0;
                c.sending = false;
            }
        if (!st.master)
            post(228);
        if (st.running) {
            if (st.phase == Phase::disconnect) {
                st.running = false;
                st.completed = !st.closing;
                st.last_operation = st.completion.operation;
                st.last_error = st.completion.error;
                st.phase = Phase::idle;
            } else
                complete(BleError::disconnected);
        } else if (!st.closing && st.master) {
            const std::uint16_t reason = hci_event_disconnection_complete_get_reason(packet);
            post(5, 0, 0, nullptr, 0, reason);
        }
        if (st.closing) {
            server_clear();
            st.closing = false;
        }
        scan_update();
        break;
    case GAP_EVENT_RSSI_MEASUREMENT:
        if (gap_event_rssi_measurement_get_con_handle(packet) == st.link) {
            const auto rssi = static_cast<std::int8_t>(gap_event_rssi_measurement_get_rssi(packet));
            if (rssi != 127) {
                st.connection.rssi = rssi;
                st.connection.has_rssi = true;
            }
        }
        break;
    case SM_EVENT_PAIRING_STARTED:
        if (sm_event_pairing_started_get_handle(packet) == st.link &&
            le_device_db_count() == le_device_db_max_count() && sm_le_device_index(st.link) < 0)
            sm_bonding_decline(st.link);
        break;
    case SM_EVENT_JUST_WORKS_REQUEST:
        if (sm_event_just_works_request_get_handle(packet) == st.link && !st.closing)
            sm_just_works_confirm(st.link);
        break;
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
        if (sm_event_numeric_comparison_request_get_handle(packet) == st.link && !st.closing)
            sm_numeric_comparison_confirm(st.link);
        break;
    case SM_EVENT_PASSKEY_INPUT_NUMBER:
        if (sm_event_passkey_input_number_get_handle(packet) == st.link && !st.closing)
            sm_passkey_input(st.link, st.pin);
        break;
    case SM_EVENT_PAIRING_COMPLETE:
    case SM_EVENT_REENCRYPTION_COMPLETE: {
        const auto handle = event == SM_EVENT_PAIRING_COMPLETE
                                ? sm_event_pairing_complete_get_handle(packet)
                                : sm_event_reencryption_complete_get_handle(packet);
        if (handle != st.link)
            break;
        const auto status = event == SM_EVENT_PAIRING_COMPLETE
                                ? sm_event_pairing_complete_get_status(packet)
                                : sm_event_reencryption_complete_get_status(packet);
        if (!status)
            export_bond();
        if (st.running && st.phase == Phase::security) {
            if (status)
                complete(BleError::disconnected);
            else if (st.retry_phase == Phase::operation) {
                if (st.request.operation == 1) {
                    st.used = 0;
                    st.overflow = false;
                }
                client_operation();
            } else
                query_start();
        }
        break;
    }
    case ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE:
        if (att_event_handle_value_indication_complete_get_conn_handle(packet) != st.link)
            break;
        if (st.server) {
            const auto handle =
                att_event_handle_value_indication_complete_get_attribute_handle(packet);
            for (auto& c : st.server->characteristics)
                if (c.handle == handle) {
                    c.sending = false;
                    const int status =
                        att_event_handle_value_indication_complete_get_status(packet);
                    post(229, short_uuid(c.uuid), handle, &status, sizeof(status));
                }
        }
        break;
    default:
        break;
    }
}

void tick(btstack_timer_source_t* timer) {
    if (st.link != kNoLink && !st.closing && expired(st.rssi_deadline)) {
        gap_read_rssi(st.link);
        st.rssi_deadline = now() + 2000;
    }
    if (st.starting && expired(st.startup_deadline)) {
        st.failed = true;
        st.opened = false;
    }
    if (st.advertisement_waiting && expired(st.advertisement_time))
        st.advertisement_waiting = false;
    if ((st.running || st.closing) && expired(st.deadline)) {
        if (st.phase == Phase::disconnect || st.closing) {
            // Preserve storage for late native callbacks; fail closed until reboot.
            st.failed = true;
            st.opened = false;
        } else {
            st.completion = {static_cast<std::uint16_t>(BleError::notify_timeout),
                st.request.operation, st.result_uuid, st.result_handle, 0};
            if (st.connecting)
                gap_connect_cancel();
            if (st.link != kNoLink)
                gap_disconnect(st.link);
            st.phase = Phase::disconnect;
            st.deadline = now() + 5000;
        }
    }
    if (!st.failed) {
        btstack_run_loop_set_timer(timer, 20);
        btstack_run_loop_add_timer(timer);
    }
}
} // namespace

bool ble_start() {
    if (!cyw43_arch_async_context())
        return false;
    Guard guard;
    if (st.started && st.off) {
        // The host stays initialized; only the controller was powered down.
        st.off = false;
        st.starting = true;
        st.startup_deadline = now() + 15000;
        if (hci_get_state() == HCI_STATE_OFF)
            return hci_power_control(HCI_POWER_ON) == 0;
        st.restarting = true; // still halting: power on once it reports off
        return true;
    }
    if (st.started)
        return !st.failed;
    st.started = true;
    st.starting = true;
    st.startup_deadline = now() + 15000;
    l2cap_init();
    gatt_client_init();
    sm_init();
    // Use the SDK's native random source; no independent software DRBG.
    for (unsigned i = 0; i < 4; ++i) {
        const auto er = get_rand_32(), ir = get_rand_32();
        std::memcpy(st.er + 4 * i, &er, 4);
        std::memcpy(st.ir + 4 * i, &ir, 4);
    }
    sm_set_er(st.er);
    sm_set_ir(st.ir);
    sm_set_io_capabilities(IO_CAPABILITY_KEYBOARD_DISPLAY);
    sm_set_authentication_requirements(
        SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_SECURE_CONNECTION);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_secure_connections_only_mode(false);
    st.hci.callback = packet_handler;
    hci_add_event_handler(&st.hci);
    st.sm.callback = packet_handler;
    sm_add_event_handler(&st.sm);
    att_db_util_init();
    att_server_init(att_db_util_get_address(), read_value, write_value);
    att_server_register_packet_handler(packet_handler);
    gap_set_scan_parameters(
        st.scan_parameters.active, st.scan_parameters.interval, st.scan_parameters.window);
    gap_set_scan_duplicate_filter(0);
    st.timer.process = tick;
    btstack_run_loop_set_timer(&st.timer, 20);
    btstack_run_loop_add_timer(&st.timer);
    if (hci_power_control(HCI_POWER_ON)) {
        st.failed = true;
        return false;
    }
    return true;
}

bool ble_stop() {
    if (!st.started)
        return true;
    ble_connection_close();
    Guard guard;
    // Halting disconnects any link; the CYW43 Wi-Fi side is not touched.
    st.off = true;
    st.ready = false;
    st.starting = false;
    st.restarting = false;
    st.restoring = false;
    st.scanning = false;
    return hci_power_control(HCI_POWER_OFF) == 0;
}

bool ble_ready() {
    if (!st.started)
        return false;
    Guard guard;
    return st.ready && !st.failed;
}

void ble_scan_set(BleScanConsumer consumer, bool on) {
    if (!st.started)
        return;
    Guard guard;
    if (on)
        st.consumers |= static_cast<unsigned>(consumer);
    else
        st.consumers &= ~static_cast<unsigned>(consumer);
    scan_update();
}

bool ble_scan_configure(const BleScanParameters& parameters) {
    if (!ble_scan_valid(parameters))
        return false;
    Lock lock;
    st.scan_parameters = parameters;
    // The host restarts a running scan with them; before ble_start() it receives them there.
    if (st.started)
        gap_set_scan_parameters(parameters.active, parameters.interval, parameters.window);
    return true;
}

BleScanParameters ble_scan_parameters() {
    Lock lock;
    return st.scan_parameters;
}

bool ble_scan_watch(const std::uint8_t* address, std::uint8_t type) {
    if (!address || type > 3)
        return false;
    Lock lock;
    for (unsigned i = 0; i < st.watch_count; ++i)
        if (st.watches[i].address_type == type && !std::memcmp(st.watches[i].address, address, 6))
            return true;
    if (st.watch_count == kWatches)
        return false;
    auto& watch = st.watches[st.watch_count++];
    std::memcpy(watch.address, address, 6);
    watch.address_type = type;
    return true;
}

void ble_scan_unwatch() {
    Lock lock;
    st.watch_count = 0;
}

bool ble_scan_watched(std::size_t index, std::uint8_t* address, std::uint8_t& address_type) {
    Lock lock;
    if (index >= st.watch_count)
        return false;
    std::memcpy(address, st.watches[index].address, 6);
    address_type = st.watches[index].address_type;
    return true;
}

bool ble_scan_pending() {
    if (!st.started)
        return false;
    Guard guard;
    return st.advertisement_present && !st.advertisement_waiting;
}

bool ble_scan_take(BleAdvertisement& advertisement) {
    if (!st.started)
        return false;
    Guard guard;
    if (!st.advertisement_present || st.advertisement_waiting)
        return false;
    advertisement = st.advertisement;
    st.advertisement_present = false;
    return true;
}

std::uint32_t ble_scan_dropped() {
    if (!st.started)
        return 0;
    Guard guard;
    return st.advertisement_drops;
}

bool ble_connection_open() {
    if (!st.started)
        return false;
    Guard guard;
    if (st.off || st.failed || st.closing)
        return false;
    st.opened = true;
    return true;
}

void ble_connection_close() {
    if (!st.started)
        return;
    Guard guard;
    st.opened = false;
    st.completed = false;
    st.event_count = 0;
    st.event_head = 0;
    for (auto& present : st.authenticated_present)
        present = false;
    st.consumers &= ~static_cast<unsigned>(BleScanConsumer::berry);
    st.advertisement_present = false;
    if (st.server) {
        st.server->enabled = false;
        gap_advertisements_enable(0);
        st.server->advertising = false;
    }
    st.running = false;
    st.request = {};
    st.pin = 0;
    sm_use_fixed_passkey_in_display_role(0);
    if (st.link != kNoLink || st.connecting) {
        st.closing = true;
        st.deadline = now() + 5000;
        if (st.connecting)
            gap_connect_cancel();
        if (st.link != kNoLink)
            gap_disconnect(st.link);
    } else
        server_clear();
    scan_update();
}

bool ble_connection_busy() {
    if (!st.started)
        return false;
    Guard guard;
    return st.running || st.completed || st.closing;
}

bool ble_connection_request(const BleRequest& request) {
    if (!st.started)
        return false;
    Guard guard;
    if (!st.opened || st.failed || !st.ready || st.running || st.completed || st.closing ||
        (request.length && !request.data) || (request.reply_capacity && !request.reply))
        return false;
    st.request = request;
    st.running = true;
    st.used = 0;
    st.result_handle = 0;
    st.result_uuid = 0;
    st.retried = false;
    st.attempts = 0;
    st.overflow = false;
    st.pin = request.pin;
    sm_use_fixed_passkey_in_display_role(st.pin);
    st.deadline = now() + 10000;
    scan_update();
    if (request.operation >= 201) {
        complete(server_job());
        return true;
    }
    if (request.operation == 5) {
        if (st.link == kNoLink) {
            complete(BleError::none);
            return true;
        }
        st.completion = {0, 5, 0, 0, 0};
        st.phase = Phase::disconnect;
        gap_disconnect(st.link);
        return true;
    }
    if (request.operation < 1 || request.operation > 7) {
        complete(BleError::no_characteristic);
        return true;
    }
    if (st.link != kNoLink) {
        if (std::memcmp(request.address, st.connection.peer_address, 6) ||
            (request.address_type & 1) != (st.peer_type & 1))
            complete(BleError::no_connect);
        else
            query_start();
    } else {
        st.phase = Phase::connect;
        st.connecting = true;
        std::uint8_t address[6];
        std::memcpy(address, request.address, 6);
        const auto status =
            gap_connect(address, static_cast<bd_addr_type_t>(request.address_type & 1));
        if (status) {
            st.connecting = false;
            complete(BleError::no_connect);
        }
    }
    return true;
}

bool ble_connection_pending() {
    if (!st.started)
        return false;
    Guard guard;
    bool authenticated = false;
    for (const auto present : st.authenticated_present)
        authenticated |= present;
    return st.completed || (!st.running && (st.event_count || authenticated));
}

bool ble_connection_poll(BleEvent& event, std::uint8_t* payload, std::size_t capacity) {
    if (!st.started)
        return false;
    Guard guard;
    if (st.completed) {
        event = st.completion;
        st.completed = false;
        return true;
    }
    unsigned bond = 0;
    while (bond < 4 && !st.authenticated_present[bond])
        ++bond;
    if (st.running || (!st.event_count && bond == 4))
        return false;
    auto& item = bond < 4 ? st.authenticated[bond] : st.events[st.event_head];
    event = item.event;
    if (bond < 4)
        st.authenticated_present[bond] = false;
    else {
        st.event_head = (st.event_head + 1) % 4;
        --st.event_count;
    }
    if (event.length > capacity || (event.length && !payload)) {
        event.error = static_cast<std::uint16_t>(BleError::buffer_too_small);
        event.length = 0;
    } else if (event.length)
        std::memcpy(payload, item.data, event.length);
    return true;
}

std::uint32_t ble_connection_dropped() {
    if (!st.started)
        return 0;
    Guard guard;
    return st.event_drops;
}

bool ble_configure(const BleRequest& r) {
    if (!st.started)
        return false;
    Guard guard;
    if (st.failed || st.running || st.closing || !server_allocate() || (r.length && !r.data))
        return false;
    auto& s = *st.server;
    if (r.operation == 231) {
        if (st.link != kNoLink || st.connecting || le_device_db_count())
            return false;
        if (r.length != 0 && r.length != 6)
            return false;
        std::uint8_t native[6]{};
        if (r.length)
            reverse_bytes(r.data, native, 6);
        if (r.length && !pico_ble::valid_identity(native, 1))
            return false;
        gap_advertisements_enable(0);
        s.advertising = false;
        s.address_type = r.length == 6;
        if (r.length) {
            reverse_bytes(r.data, s.address, 6);
            gap_random_address_set(s.address);
        } else {
            std::memset(s.address, 0, 6);
            gap_random_address_set_mode(GAP_RANDOM_ADDRESS_TYPE_OFF);
        }
        return true;
    }
    if (r.operation == 232) {
        if (r.length != 5 || r.data[0] > 2)
            return false;
        const auto min = little_endian_read_16(r.data, 1), max = little_endian_read_16(r.data, 3);
        if (min < 0x20 || max > 0x4000 || min > max)
            return false;
        s.mode = r.data[0];
        s.interval_min = min;
        s.interval_max = max;
        if (s.advertising)
            advertising_start();
        return true;
    }
    if (r.operation == 233) {
        if (r.length > 248)
            return false;
        if (r.length)
            std::memcpy(s.name, r.data, r.length);
        s.name[r.length] = 0;
        s.name_length = r.length;
        return true;
    }
    return false;
}

bool ble_store_bond(const std::uint8_t* bytes, std::size_t size) {
    if (!st.started || !bytes || size != sizeof(Bond))
        return false;
    Guard guard;
    if ((!st.ready && !st.restoring) || st.failed || st.link != kNoLink || st.connecting ||
        st.running || st.closing)
        return false;
    Bond bond;
    std::memcpy(&bond, bytes, size);
    std::uint8_t local[6];
    std::memcpy(local, st.local_address, 6);
    const std::uint8_t own_type = st.server ? st.server->address_type : 0;
    const std::uint8_t zero[6]{};
    if (!pico_ble::valid_record(
            bond, kRevision, local, own_type, st.server ? st.server->address : zero))
        return false;
    int target = -1;
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        int type;
        std::uint8_t peer[6];
        le_device_db_info(i, &type, peer, nullptr);
        if (type == bond.peer_type && !std::memcmp(peer, bond.peer, 6)) {
            target = i;
            break;
        }
    }
    if (le_device_db_count() &&
        (std::memcmp(st.er, bond.er, 16) || std::memcmp(st.ir, bond.ir, 16)))
        return false;
    if (target >= 0) {
        // Duplicate restores are idempotent; never silently replace an existing key.
        std::uint16_t ediv;
        std::uint8_t random[8], ltk[16], irk[16];
        int ks, a, u, sc;
        le_device_db_info(target, nullptr, nullptr, irk);
        le_device_db_encryption_get(target, &ediv, random, ltk, &ks, &a, &u, &sc);
        return !std::memcmp(irk, bond.irk, 16) && !std::memcmp(ltk, bond.ltk, 16) &&
               !std::memcmp(random, bond.random, 8) &&
               ediv == little_endian_read_16(bond.ediv, 0) && ks == bond.key_size &&
               a == bond.authenticated && u == bond.authorized && sc == bond.secure_connections;
    }
    target = le_device_db_add(bond.peer_type, bond.peer, bond.irk);
    if (target < 0)
        return false;
    const bool roots_changed = std::memcmp(st.er, bond.er, 16) || std::memcmp(st.ir, bond.ir, 16);
    std::memcpy(st.er, bond.er, 16);
    std::memcpy(st.ir, bond.ir, 16);
    sm_set_er(st.er);
    sm_set_ir(st.ir);
    // HCI_WORKING makes SM derive its IRK/DHK again from the restored roots.
    if (roots_changed) {
        st.restarting = true;
        st.starting = true;
        st.restoring = true;
        st.startup_deadline = now() + 15000;
        st.ready = false;
        hci_power_control(HCI_POWER_OFF);
    }
    le_device_db_encryption_set(target, little_endian_read_16(bond.ediv, 0), bond.random, bond.ltk,
        bond.key_size, bond.authenticated, bond.authorized, bond.secure_connections);
    return true;
}

void ble_info(BleInfo& info) {
    info = {};
    info.roles = 15;
    info.version = 5;
    info.mtu = 512;
    if (!st.started)
        return;
    Guard guard;
    info.ready = st.ready && !st.failed;
    info.pending = st.running || st.completed;
    gap_local_bd_addr(info.local_address);
    if (st.server && st.server->address_type)
        std::memcpy(info.local_address, st.server->address, 6);
    info.bonds = le_device_db_count();
    info.last_operation = st.last_operation;
    info.last_error = st.last_error;
    info.dropped_events = st.event_drops;
    info.dropped_advertisements = st.advertisement_drops;
    if (st.link != kNoLink) {
        st.connection.encrypted = gap_encryption_key_size(st.link) != 0;
        st.connection.authenticated = gap_authenticated(st.link);
        st.connection.bonded = gap_bonded(st.link);
        st.connection.mtu = att_server_get_mtu(st.link);
        const int index = sm_le_device_index(st.link);
        if (index >= 0) {
            le_device_db_info(index, nullptr, st.connection.peer_id_address, st.connection.irk);
            st.connection.has_irk = false;
            for (const auto byte : st.connection.irk)
                st.connection.has_irk |= byte != 0;
        }
        info.connection = st.connection;
    }
}
} // namespace newmota::platform
