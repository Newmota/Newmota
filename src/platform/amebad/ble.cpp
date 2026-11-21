/*
  ble.cpp - AmebaD native Realtek GAP/GATT BLE adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble.cpp
 * @brief Bounded legacy BLE over the pinned Realtek host and controller.
 *
 * After host setup, native calls run on the GAP message task. A separate worker sequences
 * blocking client jobs; callbacks only copy bounded data and signal that worker.
 * One total link is admitted, in either role. Registered service storage lives
 * until reboot because this SDK has no service removal API.
 */
#include "newmota/platform/ble.h"
#include "newmota/platform/ble_cache.h"
#include "ble_bond.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "app_msg.h"
#include "bte.h"
#include "gap.h"
#include "gap_adv.h"
#include "gap_bond_le.h"
#include "gap_config.h"
#include "gap_conn_le.h"
#include "gap_le.h"
#include "gap_msg.h"
#include "gap_scan.h"
#include "gap_storage_le.h"
#include "profile_client.h"
#include "profile_server.h"
#include "rtk_coex.h"

/** @brief Native startup precondition; the controller shares Wi-Fi calibration. */
bool newmota_amebad_ble_radio_ready();
}

#if !F_BT_LE_GAP_CENTRAL_SUPPORT || !F_BT_LE_GAP_PERIPHERAL_SUPPORT ||                             \
    !F_BT_LE_GATT_CLIENT_SUPPORT || !F_BT_LE_GATT_SERVER_SUPPORT || F_BT_LE_5_0_AE_ADV_SUPPORT ||  \
    F_BT_LE_5_0_AE_SCAN_SUPPORT || F_BT_LE_PRIVACY_SUPPORT
#error "AmebaD BLE requires the audited native combined-role legacy GAP/GATT archive"
#endif

namespace newmota::platform {
namespace {
constexpr unsigned kServices = 4, kCharacteristics = 12, kSubscriptions = 8, kWatches = 8;
constexpr TickType_t kTimeout = pdMS_TO_TICKS(5000);
constexpr char kRevision[] = NEWMOTA_BLE_SDK_REVISION;
static_assert(sizeof(kRevision) == 41);
using amebad_ble::BondRecord;
constexpr std::uint8_t kBase[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0, 0, 0, 0};

/** @brief Protects copied snapshots only; never contains native calls or waits. */
struct Guard {
    Guard() { taskENTER_CRITICAL(); }

    ~Guard() { taskEXIT_CRITICAL(); }
};

struct Event {
    BleEvent event;
    std::uint8_t data[255];
};

struct Watch {
    std::uint8_t address[6], type;
};

struct Subscription {
    std::uint16_t handle, uuid;
};

namespace cache = ble_cache;

/** @brief What a cached characteristic needs to skip its discovery. */
struct CachedCharacteristic {
    std::uint16_t handle;
    std::uint8_t properties;
};

struct Characteristic {
    std::uint8_t uuid[16], service, length;
    std::uint16_t flags, index, ccc, handle;
    std::uint8_t value[255];
};

struct Service {
    std::uint8_t uuid[16], id, count;
    std::uint16_t start;
    T_ATTRIB_APPL attributes[1 + 3 * kCharacteristics];
};

/** @brief Lazy server allocation; registered tables may never move or be freed. */
struct Server {
    Service services[kServices];
    Characteristic characteristics[kCharacteristics];
    std::uint8_t service_count, characteristic_count, mode;
    std::uint16_t interval_min, interval_max;
    std::uint8_t peer[6], peer_type;
    bool frozen, registered, registration_failed, enabled;
};

/** @brief Synchronous call closure, borrowed only until the message task signals done. */
struct Call {
    void (*run)(void*);
    void* context;
};

struct State {
    enum class Pending {
        none,
        connect,
        discovery,
        read,
        write,
        notification,
        service,
        advertising,
        security,
        address
    };
    std::atomic<bool> started{}, ready{}, failed{}, opened{}, running{}, completed{}, closing{};
    std::atomic<bool> linked{}, connecting{}, waiting{}, hold{}, quit{};
    std::atomic<bool> notification{}, notified{};
    std::atomic<bool> authenticated_pending{};
    std::atomic<Pending> pending{Pending::none};
    std::atomic<unsigned> consumers{};
    std::atomic<std::uint32_t> event_drops{}, scan_drops{};
    std::atomic<std::uint32_t> cache_generation{}; ///< Advanced when cached handles may be stale.
    std::atomic<std::uint16_t> service_changed{};  ///< Its handle while indicated to us, or 0.
    TaskHandle_t pump{}, worker{};
    QueueHandle_t calls{}, events{}, io{}, native_events{};
    SemaphoreHandle_t call_mutex{}, called{}, signal{};
    BleRequest request{};
    BleEvent completion{};
    T_GAP_DEV_STATE device{};
    T_CLIENT_ID client{0xFF};
    std::uint8_t connection{};
    std::uint8_t registering_service{0xFF};
    bool scan_transition{}, scan_target{};
    bool advertising_transition{};
    BleConnectionInfo connection_info{};
    std::uint8_t local[6]{};
    std::uint8_t public_address[6]{}, own_type{};
    std::uint32_t pin{};
    std::uint8_t pair_mode{GAP_PAIRING_MODE_PAIRABLE};
    bool had_bond{}, bond_added{}, encrypted{}, authentication_complete{}, preserve_link{};
    TickType_t rssi_time{};
    BondRecord authentication{};
    BleAdvertisement advertisement{};
    bool advertisement_present{}, advertisement_waiting{};
    TickType_t advertisement_time{};
    Watch watches[kWatches]{};
    unsigned watch_count{};
    BleScanParameters scan_parameters{kBleScanDefaults};
    bool scan_dirty{}; ///< Parameters changed; restart a running scan.
    Subscription subscriptions[kSubscriptions]{};
    Server* server{};
    int status{};
    std::uint8_t last_operation{};
    std::uint16_t last_error{};
    // One worker owns procedure state; only its active callback may fill the output.
    bool listing{}, found{}, overflow{}, watching{}, boundary{};
    std::uint8_t wanted[16]{}; ///< The characteristic a non-listing discovery looks for.
    cache::Cache<CachedCharacteristic> cache{};
    std::uint16_t first{}, last{}, handle{}, ccc{}, properties{};
    std::uint32_t next{};
    std::uint8_t count{};
    std::uint8_t notification_value[255]{};
    std::uint16_t notification_size{};
    std::uint8_t* output{};
    std::size_t used{}, capacity{};
} st;

void reverse(const std::uint8_t* from, std::uint8_t* to) {
    for (unsigned i = 0; i < 6; ++i)
        to[i] = from[5 - i];
}

std::uint16_t short_uuid(const std::uint8_t* uuid) {
    return static_cast<std::uint16_t>(uuid[12] | (uuid[13] << 8));
}

bool short_form(const std::uint8_t* uuid) {
    return std::memcmp(uuid, kBase, 12) == 0 && uuid[14] == 0 && uuid[15] == 0;
}

/** @brief Executes native calls on the message task, keeping closure storage alive. */
template <class Function> auto invoke(Function function) -> decltype(function()) {
    using Result = decltype(function());

    struct Context {
        Function& function;
        Result result{};
    } context{function};

    Call call{[](void* pointer) {
                  auto& value = *static_cast<Context*>(pointer);
                  value.result = value.function();
              },
        &context};
    xSemaphoreTake(st.call_mutex, portMAX_DELAY);
    xQueueSend(st.calls, &call, portMAX_DELAY);
    xSemaphoreTake(st.called, portMAX_DELAY);
    xSemaphoreGive(st.call_mutex);
    return context.result;
}

void post(std::uint8_t operation, std::uint16_t uuid = 0, std::uint16_t handle = 0,
    const void* data = nullptr, std::size_t size = 0) {
    if (!st.opened || st.failed)
        return;
    if (size > 255) {
        ++st.event_drops;
        return;
    }
    Event event{{0, operation, uuid, handle, static_cast<std::uint16_t>(size)}, {}};
    if (size != 0)
        std::memcpy(event.data, data, size);
    if (xQueueSend(st.events, &event, 0) != pdTRUE)
        ++st.event_drops;
}

void append(const void* bytes, std::size_t size) {
    if (size == 0)
        return;
    if (size > st.capacity - st.used) {
        st.overflow = true;
        return;
    }
    std::memcpy(st.output + st.used, bytes, size);
    st.used += size;
}

void append_uuid(const std::uint8_t* uuid) {
    const std::uint8_t size = short_form(uuid) ? 2 : 16;
    const std::uint8_t bits = size * 8;
    append(&bits, 1);
    append(size == 2 ? uuid + 12 : uuid, size);
}

void finish(int status, State::Pending pending = State::Pending::none) {
    if (!st.waiting || (pending != State::Pending::none && st.pending != pending))
        return;
    st.status = status;
    st.waiting = false;
    xSemaphoreGive(st.signal);
}

void begin(State::Pending pending) {
    xSemaphoreTake(st.signal, 0);
    st.status = -1;
    st.pending = pending;
    st.waiting = true;
}

bool await(TickType_t timeout = kTimeout) {
    if (xSemaphoreTake(st.signal, timeout) != pdTRUE) {
        // Suppress every late callback before relinquishing the caller's buffer.
        invoke([] {
            st.waiting = false;
            return true;
        });
        return false;
    }
    return st.status == 0;
}

/** @brief Resolves the active peer through the native key manager, including saved identity. */
T_LE_KEY_ENTRY* peer_bond() {
    if (!st.linked)
        return nullptr;
    std::uint8_t address[6], type;
    if (!le_get_conn_addr(st.connection, address, &type))
        return nullptr;
    return le_find_key_entry(address, static_cast<T_GAP_REMOTE_ADDR_TYPE>(type));
}

/** @brief A full key pool never admits pairing that could evict a different peer. */
bool can_pair() {
    return le_get_bond_dev_num() == 0 || peer_bond() != nullptr;
}

void pairable(bool allowed) {
    const std::uint8_t mode = allowed ? GAP_PAIRING_MODE_PAIRABLE : GAP_PAIRING_MODE_NO_PAIRING;
    if (st.pair_mode != mode) {
        gap_set_param(GAP_PARAM_BOND_PAIRING_MODE, 1, const_cast<std::uint8_t*>(&mode));
        st.pair_mode = mode;
    }
}

void export_bond() {
    if (!st.opened || !st.encrypted || !st.authentication_complete ||
        (!st.had_bond && !st.bond_added))
        return;
    auto* entry = peer_bond();
    if (entry == nullptr || le_get_dev_bond_info_len() != amebad_ble::kNativeBondSize)
        return;
    BondRecord record{};
    std::memcpy(record.magic, "ADK1", 4);
    std::memcpy(record.revision, kRevision, 40);
    record.mask[0] = amebad_ble::kBondMask;
    record.local_type = st.own_type;
    std::memcpy(record.local_address, st.local, 6);
    if (!le_get_dev_bond_info(entry, record.keys))
        return;
    amebad_ble::canonicalize(record.keys);
    if (!amebad_ble::valid_record(record, kRevision, st.own_type, st.local))
        return;
    st.authentication = record;
    st.authenticated_pending = true;
}

void refresh_security() {
    BleConnectionInfo info;
    {
        Guard guard;
        info = st.connection_info;
    }
    T_GAP_SEC_LEVEL level{};
    const bool sampled = le_bond_get_sec_level(st.connection, &level) == GAP_CAUSE_SUCCESS;
    info.encrypted = sampled && level != GAP_SEC_LEVEL_NO;
    info.authenticated =
        sampled && (level == GAP_SEC_LEVEL_AUTHEN || level == GAP_SEC_LEVEL_SC_AUTHEN);
    info.bonded = false;
    info.has_irk = false;
    std::memset(info.irk, 0, sizeof(info.irk));
    auto* entry = peer_bond();
    std::uint8_t keys[amebad_ble::kNativeBondSize]{};
    if (entry != nullptr && le_get_dev_bond_info_len() == sizeof(keys) &&
        le_get_dev_bond_info(entry, keys)) {
        amebad_ble::canonicalize(keys);
        info.bonded = amebad_ble::valid_keys(keys, st.own_type);
        if (info.bonded && keys[95] == 1) {
            info.has_irk = true;
            std::memcpy(info.irk, keys + 72, 16);
            reverse(keys + 88, info.peer_id_address);
        }
    }
    st.encrypted = info.encrypted;
    Guard guard;
    st.connection_info = info;
}

bool secure() {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        begin(State::Pending::security);
        const bool started = invoke([] {
            if (!can_pair())
                return false;
            pairable(true);
            return le_bond_pair(st.connection) == GAP_CAUSE_SUCCESS;
        });
        if (!started) {
            st.waiting = false;
            return false;
        }
        if (await(pdMS_TO_TICKS(30000)))
            return true;
        if (attempt != 0 || st.status != (HCI_ERR | HCI_ERR_KEY_MISSING))
            return false;
        invoke([] {
            auto* entry = peer_bond();
            if (entry != nullptr)
                le_bond_delete_by_bd(entry->remote_bd.addr,
                    static_cast<T_GAP_REMOTE_ADDR_TYPE>(entry->remote_bd.remote_bd_type));
            st.had_bond = false;
            st.bond_added = false;
            return true;
        });
    }
    return false;
}

template <class Function>
bool procedure(Function function, State::Pending pending = State::Pending::discovery) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        begin(pending);
        const auto cause = invoke(function);
        if (cause != GAP_CAUSE_SUCCESS) {
            st.waiting = false;
            return false;
        }
        if (await())
            return true;
        if (attempt != 0 || st.watching ||
            (st.status != (ATT_ERR | ATT_ERR_INSUFFICIENT_AUTHEN) &&
                st.status != (ATT_ERR | ATT_ERR_INSUFFICIENT_ENCRYPT) &&
                st.status != (ATT_ERR | ATT_ERR_INSUFFICIENT_KEY_SIZE)) ||
            !secure())
            return false;
    }
    return false;
}

/** @brief A link came or went: nothing learned about the previous peer's database still holds. */
void link_changed() {
    st.service_changed = 0;
    ++st.cache_generation;
}

bool owns_connection(std::uint8_t connection) {
    return !st.failed && st.linked && st.connection == connection;
}

void discovery_state(std::uint8_t connection, T_DISCOVERY_STATE state) {
    if (!owns_connection(connection))
        return;
    if (state == DISC_STATE_FAILED)
        finish(-1, State::Pending::discovery);
    else if (state == DISC_STATE_SRV_DONE || state == DISC_STATE_CHAR_DONE ||
             state == DISC_STATE_CHAR_UUID16_DONE || state == DISC_STATE_CHAR_UUID128_DONE ||
             state == DISC_STATE_CHAR_DESCRIPTOR_DONE)
        finish(0, State::Pending::discovery);
}

void discovery_result(
    std::uint8_t connection, T_DISCOVERY_RESULT_TYPE type, T_DISCOVERY_RESULT_DATA data) {
    if (!owns_connection(connection) || !st.waiting || st.pending != State::Pending::discovery)
        return;
    std::uint8_t uuid[16];
    std::memcpy(uuid, kBase, 16);
    std::uint16_t first = 0, last = 0, handle = 0, properties = 0;
    bool service = false, characteristic = false;
    switch (type) {
    case DISC_RESULT_ALL_SRV_UUID16:
        first = data.p_srv_uuid16_disc_data->att_handle;
        last = data.p_srv_uuid16_disc_data->end_group_handle;
        std::memcpy(uuid + 12, &data.p_srv_uuid16_disc_data->uuid16, 2);
        service = true;
        break;
    case DISC_RESULT_ALL_SRV_UUID128:
        first = data.p_srv_uuid128_disc_data->att_handle;
        last = data.p_srv_uuid128_disc_data->end_group_handle;
        std::memcpy(uuid, data.p_srv_uuid128_disc_data->uuid128, 16);
        service = true;
        break;
    case DISC_RESULT_SRV_DATA:
        if (!st.found) {
            st.found = true;
            st.first = data.p_srv_disc_data->att_handle;
            st.last = data.p_srv_disc_data->end_group_handle;
        }
        return;
    case DISC_RESULT_CHAR_UUID16:
    case DISC_RESULT_BY_UUID16_CHAR:
        first = data.p_char_uuid16_disc_data->decl_handle;
        handle = data.p_char_uuid16_disc_data->value_handle;
        properties = data.p_char_uuid16_disc_data->properties;
        std::memcpy(uuid + 12, &data.p_char_uuid16_disc_data->uuid16, 2);
        characteristic = true;
        break;
    case DISC_RESULT_CHAR_UUID128:
    case DISC_RESULT_BY_UUID128_CHAR:
        first = data.p_char_uuid128_disc_data->decl_handle;
        handle = data.p_char_uuid128_disc_data->value_handle;
        properties = data.p_char_uuid128_disc_data->properties;
        std::memcpy(uuid, data.p_char_uuid128_disc_data->uuid128, 16);
        characteristic = true;
        break;
    case DISC_RESULT_CHAR_DESC_UUID16: {
        const auto descriptor = data.p_char_desc_uuid16_disc_data->uuid16;
        if (descriptor >= 0x2800 && descriptor <= 0x2803) {
            st.boundary = true; // the next declaration ends this characteristic
        } else if (descriptor == GATT_UUID_CHAR_CLIENT_CONFIG && !st.found && !st.boundary) {
            st.ccc = data.p_char_desc_uuid16_disc_data->handle;
            st.found = true;
        }
        return;
    }
    default:
        return;
    }
    if (st.listing) {
        if (st.count == 255) {
            st.overflow = true;
            return;
        }
        append_uuid(uuid);
        if (characteristic) {
            const auto byte = static_cast<std::uint8_t>(properties);
            append(&byte, 1);
            append(&handle, 2);
        }
        ++st.count;
    } else if (service && !st.found) {
        st.first = first;
        st.last = last;
        st.found = true;
    } else if (characteristic) {
        // Unfiltered enumeration also finds the next declaration, bounding CCC search.
        if (first > st.handle && first < st.next)
            st.next = first;
        if (!st.found && std::memcmp(uuid, st.wanted, 16) == 0) {
            st.handle = handle;
            st.properties = properties;
            st.found = true;
        }
    }
}

void read_result(std::uint8_t connection, std::uint16_t cause, std::uint16_t, std::uint16_t size,
    std::uint8_t* value) {
    if (!owns_connection(connection))
        return;
    if (st.waiting && st.pending == State::Pending::read && cause == 0)
        append(value, size);
    finish(cause, State::Pending::read);
}

void write_result(std::uint8_t connection, T_GATT_WRITE_TYPE type, std::uint16_t,
    std::uint16_t cause, std::uint8_t) {
    if (!owns_connection(connection) || type != GATT_WRITE_TYPE_REQ)
        return;
    finish(cause, State::Pending::write);
}

T_APP_RESULT notification(std::uint8_t connection, bool notify, std::uint16_t handle,
    std::uint16_t size, std::uint8_t* value) {
    if (!owns_connection(connection))
        return APP_RESULT_SUCCESS;
    const Subscription* selected = nullptr;
    for (const auto& subscription : st.subscriptions)
        if (subscription.handle == handle)
            selected = &subscription;
    // Service Changed may also arrive on a configuration the peer kept from an earlier bond or
    // that a script subscribed. Erring towards forgetting costs one rediscovery.
    if (handle == st.service_changed ||
        (!notify && (selected == nullptr || selected->uuid == cache::kServiceChangedUuid)))
        ++st.cache_generation;
    for (const auto& subscription : st.subscriptions)
        if (subscription.handle == handle) {
            post(103, subscription.uuid, handle, value, size);
            if (st.notification && !st.notified && handle == st.handle) {
                st.notification_size = size;
                if (size != 0 && size <= sizeof(st.notification_value))
                    std::memcpy(st.notification_value, value, size);
                st.notified = true;
                finish(0, State::Pending::notification);
            }
            break;
        }
    if (!notify)
        client_attr_ind_confirm(st.connection);
    return APP_RESULT_SUCCESS;
}

const T_FUN_CLIENT_CBS kClientCallbacks{
    discovery_state, discovery_result, read_result, write_result, notification, nullptr};

Characteristic* local_characteristic(std::uint8_t service, std::uint16_t index) {
    if (st.server != nullptr)
        for (unsigned i = 0; i < st.server->characteristic_count; ++i) {
            auto& characteristic = st.server->characteristics[i];
            if (st.server->services[characteristic.service].id == service &&
                characteristic.index == index)
                return &characteristic;
        }
    return nullptr;
}

T_APP_RESULT server_read(std::uint8_t connection, T_SERVER_ID service, std::uint16_t index,
    std::uint16_t offset, std::uint16_t* size, std::uint8_t** value) {
    auto* characteristic = local_characteristic(service, index);
    if (!st.opened || !owns_connection(connection) || characteristic == nullptr)
        return APP_RESULT_ATTR_NOT_FOUND;
    if (offset > characteristic->length)
        return APP_RESULT_INVALID_OFFSET;
    *size = characteristic->length - offset;
    *value = characteristic->value + offset;
    post(221, short_uuid(characteristic->uuid), characteristic->handle);
    return APP_RESULT_SUCCESS;
}

T_APP_RESULT server_write(std::uint8_t connection, T_SERVER_ID service, std::uint16_t index,
    T_WRITE_TYPE type, std::uint16_t size, std::uint8_t* value, P_FUN_WRITE_IND_POST_PROC*) {
    auto* characteristic = local_characteristic(service, index);
    if (!st.opened || !owns_connection(connection) || characteristic == nullptr)
        return APP_RESULT_ATTR_NOT_FOUND;
    if (type != WRITE_REQUEST && type != WRITE_WITHOUT_RESPONSE)
        return APP_RESULT_REJECT;
    if (size > 255)
        return APP_RESULT_INVALID_VALUE_SIZE;
    if (size != 0)
        std::memcpy(characteristic->value, value, size);
    characteristic->length = static_cast<std::uint8_t>(size);
    post(222, short_uuid(characteristic->uuid), characteristic->handle, value, size);
    return APP_RESULT_SUCCESS;
}

void server_ccc(
    std::uint8_t connection, T_SERVER_ID service, std::uint16_t index, std::uint16_t bits) {
    if (!owns_connection(connection))
        return;
    auto* characteristic = local_characteristic(service, index - 1);
    if (characteristic == nullptr)
        return;
    characteristic->ccc = bits & 3;
    post(static_cast<std::uint8_t>(223 + (bits & 3)), short_uuid(characteristic->uuid),
        characteristic->handle);
}

T_APP_RESULT server_event(T_SERVER_ID, void* pointer) {
    auto& event = *static_cast<T_SERVER_APP_CB_DATA*>(pointer);
    if (event.eventId == PROFILE_EVT_SRV_REG_AFTER_INIT_COMPLETE) {
        const auto& result = event.event_data.server_reg_after_init_result;
        if (result.service_id == st.registering_service)
            finish(result.cause, State::Pending::service);
    } else if (event.eventId == PROFILE_EVT_SEND_DATA_COMPLETE) {
        auto& result = event.event_data.send_data_result;
        if (!owns_connection(result.conn_id))
            return APP_RESULT_SUCCESS;
        auto* characteristic = local_characteristic(result.service_id, result.attrib_idx);
        if (characteristic != nullptr) {
            const int status = result.cause;
            post(229, short_uuid(characteristic->uuid), characteristic->handle, &status,
                sizeof(status));
        }
    }
    return APP_RESULT_SUCCESS;
}

void handle_io(const T_IO_MSG& message) {
    if (message.type != IO_MSG_TYPE_BT_STATUS)
        return;
    T_LE_GAP_MSG event{};
    std::memcpy(&event, &message.u.param, sizeof(message.u.param));
    switch (message.subtype) {
    case GAP_MSG_LE_DEV_STATE_CHANGE: {
        const auto previous_advertising = st.device.gap_adv_state;
        st.device = event.msg_data.gap_dev_state_change.new_state;
        if ((st.scan_target && st.device.gap_scan_state == GAP_SCAN_STATE_SCANNING) ||
            (!st.scan_target && st.device.gap_scan_state == GAP_SCAN_STATE_IDLE) ||
            event.msg_data.gap_dev_state_change.cause != 0)
            st.scan_transition = false;
        if (st.device.gap_adv_state == GAP_ADV_STATE_ADVERTISING ||
            (previous_advertising != st.device.gap_adv_state &&
                st.device.gap_adv_state == GAP_ADV_STATE_IDLE))
            st.advertising_transition = false;
        if (st.pending == State::Pending::advertising &&
            previous_advertising != st.device.gap_adv_state &&
            (st.device.gap_adv_state == GAP_ADV_STATE_ADVERTISING ||
                event.msg_data.gap_dev_state_change.cause != 0))
            finish(event.msg_data.gap_dev_state_change.cause, State::Pending::advertising);
        if (st.device.gap_init_state == GAP_INIT_STATE_STACK_READY && !st.ready) {
            gap_get_param(GAP_PARAM_BD_ADDR, st.local);
            std::memcpy(st.public_address, st.local, 6);
            st.ready = true;
        }
        break;
    }
    case GAP_MSG_LE_CONN_STATE_CHANGE: {
        auto& connection = event.msg_data.gap_conn_state_change;
        if (connection.new_state == GAP_CONN_STATE_CONNECTED) {
            T_GAP_CONN_INFO info{};
            if (!le_get_conn_info(connection.conn_id, &info)) {
                le_disconnect(connection.conn_id);
                break;
            }
            if (st.linked) {
                if (st.connection != connection.conn_id)
                    le_disconnect(connection.conn_id);
                break;
            }
            std::uint8_t expected[6];
            reverse(st.request.address, expected);
            const bool outbound = st.connecting && info.role == GAP_LINK_ROLE_MASTER &&
                                  info.remote_bd_type == st.request.address_type &&
                                  std::memcmp(expected, info.remote_bd, 6) == 0;
            const bool accepted =
                !st.connecting && info.role == GAP_LINK_ROLE_SLAVE && st.server != nullptr &&
                st.server->enabled &&
                (st.server->mode != 1 || (info.remote_bd_type == st.server->peer_type &&
                                             std::memcmp(info.remote_bd, st.server->peer, 6) == 0));
            if (!st.opened || st.failed || st.closing || (!outbound && !accepted)) {
                le_disconnect(connection.conn_id);
                break;
            }
            st.connection = connection.conn_id;
            st.linked = true;
            link_changed();
            st.connecting = false;
            st.had_bond = peer_bond() != nullptr;
            st.bond_added = false;
            st.encrypted = false;
            st.authentication_complete = false;
            st.rssi_time = xTaskGetTickCount() - pdMS_TO_TICKS(1000);
            pairable(can_pair());
            {
                Guard guard;
                st.connection_info = {};
                st.connection_info.present = true;
                st.connection_info.master = info.role == GAP_LINK_ROLE_MASTER;
                st.connection_info.mtu = 23;
                reverse(info.remote_bd, st.connection_info.peer_address);
                reverse(info.remote_bd, st.connection_info.peer_id_address);
            }
            refresh_security();
            if (accepted)
                post(227, 0, 0, info.remote_bd, 6);
            if (!st.opened || st.closing)
                le_disconnect(st.connection);
            else
                finish(0, State::Pending::connect);
        } else if (connection.new_state == GAP_CONN_STATE_DISCONNECTED) {
            if ((!st.linked && !st.connecting) ||
                (st.linked && connection.conn_id != st.connection))
                break;
            const bool was_linked = st.linked.exchange(false);
            link_changed();
            st.connecting = false;
            st.encrypted = false;
            st.authentication_complete = false;
            st.had_bond = false;
            st.bond_added = false;
            pairable(le_get_bond_dev_num() == 0);
            bool master;
            {
                Guard guard;
                master = st.connection_info.master;
                st.connection_info = {};
            }
            for (auto& subscription : st.subscriptions)
                subscription = {};
            if (st.server != nullptr)
                for (auto& characteristic : st.server->characteristics)
                    characteristic.ccc = 0;
            if (was_linked && !master) {
                post(228);
            } else if (was_linked && !st.running && st.opened) {
                Event disconnected{{connection.disc_cause, 5, 0, 0, 0}, {}};
                if (xQueueSend(st.events, &disconnected, 0) != pdTRUE)
                    ++st.event_drops;
            }
            finish(st.request.operation == 5 ? 0 : -1);
        }
        break;
    }
    case GAP_MSG_LE_CONN_MTU_INFO: {
        if (!owns_connection(event.msg_data.gap_conn_mtu_info.conn_id))
            break;
        Guard guard;
        st.connection_info.mtu = event.msg_data.gap_conn_mtu_info.mtu_size;
        break;
    }
    case GAP_MSG_LE_BOND_JUST_WORK:
        le_bond_just_work_confirm(event.msg_data.gap_bond_just_work_conf.conn_id,
            st.opened && owns_connection(event.msg_data.gap_bond_just_work_conf.conn_id) &&
                    can_pair()
                ? GAP_CFM_CAUSE_ACCEPT
                : GAP_CFM_CAUSE_REJECT);
        break;
    case GAP_MSG_LE_BOND_PASSKEY_INPUT:
        le_bond_passkey_input_confirm(event.msg_data.gap_bond_passkey_input.conn_id, st.pin,
            st.opened && owns_connection(event.msg_data.gap_bond_passkey_input.conn_id) &&
                    can_pair()
                ? GAP_CFM_CAUSE_ACCEPT
                : GAP_CFM_CAUSE_REJECT);
        break;
    case GAP_MSG_LE_BOND_USER_CONFIRMATION:
        // Match ESP's noninteractive numeric-confirmation policy.
        le_bond_user_confirm(event.msg_data.gap_bond_user_conf.conn_id,
            st.opened && owns_connection(event.msg_data.gap_bond_user_conf.conn_id) && can_pair()
                ? GAP_CFM_CAUSE_ACCEPT
                : GAP_CFM_CAUSE_REJECT);
        break;
    case GAP_MSG_LE_BOND_PASSKEY_DISPLAY:
        le_bond_passkey_display_confirm(event.msg_data.gap_bond_passkey_display.conn_id,
            st.opened && owns_connection(event.msg_data.gap_bond_passkey_display.conn_id) &&
                    can_pair()
                ? GAP_CFM_CAUSE_ACCEPT
                : GAP_CFM_CAUSE_REJECT);
        break;
    case GAP_MSG_LE_AUTHEN_STATE_CHANGE: {
        const auto& authentication = event.msg_data.gap_authen_state;
        if (!owns_connection(authentication.conn_id) ||
            authentication.new_state != GAP_AUTHEN_STATE_COMPLETE)
            break;
        refresh_security();
        st.authentication_complete = authentication.status == 0;
        if (authentication.status == 0)
            export_bond();
        finish(authentication.status, State::Pending::security);
        break;
    }
    case GAP_MSG_LE_BOND_OOB_INPUT:
        le_bond_oob_input_confirm(event.msg_data.gap_bond_oob_input.conn_id, GAP_CFM_CAUSE_REJECT);
        break;
    default:
        break;
    }
}

T_APP_RESULT gap_callback(std::uint8_t type, void* pointer) {
    auto& callback = *static_cast<T_LE_CB_DATA*>(pointer);
    if (type == GAP_MSG_LE_SET_RAND_ADDR) {
        finish(callback.p_le_set_rand_addr_rsp->cause, State::Pending::address);
        return APP_RESULT_SUCCESS;
    }
    if (type == GAP_MSG_LE_READ_RSSI) {
        const auto& result = *callback.p_le_read_rssi_rsp;
        if (owns_connection(result.conn_id)) {
            if (result.cause == 0) {
                Guard guard;
                st.connection_info.has_rssi = true;
                st.connection_info.rssi = result.rssi;
            }
        }
        return APP_RESULT_SUCCESS;
    }
    if (type == GAP_MSG_LE_BOND_MODIFY_INFO) {
        const auto& change = *callback.p_le_bond_modify_info;
        if (st.linked && change.type == LE_BOND_ADD && change.p_entry == peer_bond()) {
            st.bond_added = true;
            refresh_security();
            export_bond();
        }
        pairable(st.linked ? can_pair() : le_get_bond_dev_num() == 0);
        return APP_RESULT_SUCCESS;
    }
    if (type != GAP_MSG_LE_SCAN_INFO)
        return APP_RESULT_SUCCESS;
    auto& report = *static_cast<T_LE_CB_DATA*>(pointer)->p_le_scan_info;
    if (report.data_len > 31)
        return APP_RESULT_SUCCESS;
    Guard guard;
    bool watched = st.watch_count == 0;
    for (unsigned i = 0; i < st.watch_count; ++i)
        watched |= st.watches[i].type == report.remote_addr_type &&
                   std::memcmp(st.watches[i].address, report.bd_addr, 6) == 0;
    if (!watched)
        return APP_RESULT_SUCCESS;
    auto& advertisement = st.advertisement;
    std::uint8_t address[6];
    reverse(report.bd_addr, address);
    if (st.advertisement_present) {
        if (st.advertisement_waiting && report.adv_type == GAP_ADV_EVT_TYPE_SCAN_RSP &&
            advertisement.address_type == report.remote_addr_type &&
            std::memcmp(advertisement.address, address, 6) == 0) {
            std::memcpy(advertisement.payload + advertisement.length, report.data, report.data_len);
            advertisement.length += report.data_len;
            st.advertisement_waiting = false;
        } else {
            ++st.scan_drops;
        }
        return APP_RESULT_SUCCESS;
    }
    if (report.adv_type == GAP_ADV_EVT_TYPE_SCAN_RSP)
        return APP_RESULT_SUCCESS;
    std::memcpy(advertisement.address, address, 6);
    advertisement.address_type = report.remote_addr_type;
    advertisement.rssi = report.rssi;
    advertisement.length = report.data_len;
    std::memcpy(advertisement.payload, report.data, report.data_len);
    st.advertisement_present = true;
    st.advertisement_waiting =
        st.scan_parameters.active && (report.adv_type == GAP_ADV_EVT_TYPE_UNDIRECTED ||
                                         report.adv_type == GAP_ADV_EVT_TYPE_SCANNABLE);
    st.advertisement_time = xTaskGetTickCount();
    return APP_RESULT_SUCCESS;
}

/** @brief Hands the current scan parameters to the host before a scan starts. */
void apply_scan_parameters() {
    BleScanParameters scan;
    {
        Guard guard;
        scan = st.scan_parameters;
        st.scan_dirty = false;
    }
    std::uint8_t mode = scan.active ? GAP_SCAN_MODE_ACTIVE : GAP_SCAN_MODE_PASSIVE;
    le_scan_set_param(GAP_PARAM_SCAN_MODE, 1, &mode);
    le_scan_set_param(GAP_PARAM_SCAN_INTERVAL, 2, &scan.interval);
    le_scan_set_param(GAP_PARAM_SCAN_WINDOW, 2, &scan.window);
}

[[nodiscard]] bool scan_dirty() {
    Guard guard;
    return st.scan_dirty;
}

/** @brief Reconciles scanning without blocking the shared Wi-Fi or caller task. */
void reconcile() {
    if (!st.ready || st.failed)
        return;
    const bool scan = st.consumers != 0 && !st.hold && !st.linked && !st.connecting;
    if (st.linked && xTaskGetTickCount() - st.rssi_time >= pdMS_TO_TICKS(1000)) {
        st.rssi_time = xTaskGetTickCount();
        le_read_rssi(st.connection);
    }
    if (!st.scan_transition) {
        const bool scanning = st.device.gap_scan_state == GAP_SCAN_STATE_SCANNING;
        const bool restart = scanning && scan_dirty();
        if (scan && st.device.gap_scan_state == GAP_SCAN_STATE_IDLE) {
            apply_scan_parameters();
            st.scan_transition = le_scan_start() == GAP_CAUSE_SUCCESS;
        } else if ((!scan || restart) && scanning) {
            st.scan_transition = le_scan_stop() == GAP_CAUSE_SUCCESS;
        }
        st.scan_target = scan && !restart;
    }
    if (!st.hold && !st.linked && !st.connecting && st.opened && st.server != nullptr &&
        st.server->enabled && !st.advertising_transition &&
        st.device.gap_adv_state == GAP_ADV_STATE_IDLE)
        st.advertising_transition = le_adv_start() == GAP_CAUSE_SUCCESS;
}

void pump(void*) {
    if (!gap_start_bt_stack(st.native_events, st.io, 32))
        st.failed = true;
    for (;;) {
        std::uint8_t event;
        if (xQueueReceive(st.native_events, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (event == EVENT_IO_TO_APP) {
                T_IO_MSG message{};
                if (xQueueReceive(st.io, &message, 0) == pdTRUE)
                    handle_io(message);
            } else {
                gap_handle_msg(event);
            }
        }
        Call call;
        if (xQueueReceive(st.calls, &call, 0) == pdTRUE) {
            call.run(call.context);
            xSemaphoreGive(st.called);
        }
        if (st.quit)
            break;
        reconcile();
    }
    st.pump = nullptr;
    vTaskDelete(nullptr);
}

bool wait_idle(bool advertising = false) {
    const auto end = xTaskGetTickCount() + kTimeout;
    do {
        const bool idle = invoke([=] {
            return advertising ? st.device.gap_adv_state == GAP_ADV_STATE_IDLE
                               : st.device.gap_scan_state == GAP_SCAN_STATE_IDLE;
        });
        if (idle)
            return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (static_cast<std::int32_t>(end - xTaskGetTickCount()) > 0);
    return false;
}

bool disconnect() {
    if (!st.linked && !st.connecting)
        return true;
    if (st.linked)
        invoke([] { return le_disconnect(st.connection); });
    // Do not reuse an ID after a failed drain. Permanently fail this owner on
    // timeout, retaining native storage and discarding callbacks until reboot.
    const auto started = xTaskGetTickCount();
    while (st.linked || st.connecting) {
        if (xTaskGetTickCount() - started >= kTimeout * 2) {
            st.failed = true;
            invoke([] {
                st.waiting = false;
                st.output = nullptr;
                return true;
            });
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool connect(const BleRequest& request) {
    if (st.linked) {
        std::uint8_t address[6];
        std::uint8_t type;
        const bool ok = invoke([&] { return le_get_conn_addr(st.connection, address, &type); });
        std::uint8_t display[6];
        reverse(address, display);
        const bool matches =
            ok && type == request.address_type && std::memcmp(display, request.address, 6) == 0;
        st.preserve_link = !matches;
        return matches;
    }
    if (request.address_type > 1 || !wait_idle())
        return false;
    if (invoke([] { return st.device.gap_adv_state != GAP_ADV_STATE_IDLE; })) {
        invoke([] { return le_adv_stop(); });
        if (!wait_idle(true))
            return false;
    }
    std::uint8_t address[6];
    reverse(request.address, address);
    begin(State::Pending::connect);
    st.connecting = true;
    const auto cause = invoke([&] {
        pairable(le_get_bond_dev_num() == 0 ||
                 le_find_key_entry(address,
                     static_cast<T_GAP_REMOTE_ADDR_TYPE>(request.address_type)) != nullptr);
        T_GAP_LE_CONN_REQ_PARAM parameters{96, 48, 24, 40, 0, 400, 0, 0};
        le_set_conn_param(GAP_CONN_PARAM_1M, &parameters);
        return le_connect(GAP_PHYS_CONN_INIT_1M_BIT, address,
            static_cast<T_GAP_REMOTE_ADDR_TYPE>(request.address_type),
            static_cast<T_GAP_LOCAL_ADDR_TYPE>(st.own_type), 3000);
    });
    if (cause != GAP_CAUSE_SUCCESS) {
        st.connecting = false;
        st.waiting = false;
        return false;
    }
    if (!await() || !st.linked)
        return false;
    return invoke([&] {
        std::uint8_t actual[6], type;
        return le_get_conn_addr(st.connection, actual, &type) && type == request.address_type &&
               std::memcmp(actual, address, 6) == 0;
    });
}

bool discover_service(const BleRequest& request, bool listing) {
    st.found = false;
    st.listing = listing;
    return procedure([&] {
        if (listing)
            return client_all_primary_srv_discovery(st.connection, st.client);
        if (short_form(request.service))
            return client_by_uuid_srv_discovery(
                st.connection, st.client, short_uuid(request.service));
        return client_by_uuid128_srv_discovery(
            st.connection, st.client, const_cast<std::uint8_t*>(request.service));
    });
}

bool discover_characteristics(bool listing, const std::uint8_t* wanted = nullptr) {
    if (wanted != nullptr)
        std::memcpy(st.wanted, wanted, 16);
    st.found = false;
    st.listing = listing;
    st.handle = 0;
    st.next = 65536;
    return procedure(
        [] { return client_all_char_discovery(st.connection, st.client, st.first, st.last); });
}

/** @brief Empties the cache when the link changed since it was filled, or on request. */
void cache_forget(bool always = false) {
    st.cache.forget(st.cache_generation, always);
}

/** @brief Makes the peer indicate database changes to us, unless its database is fixed.
 *  Never pairs.
 */
cache::Watch watch_database() {
    if (st.service_changed != 0)
        return cache::Watch::valid;
    std::uint8_t uuid[16];
    std::memcpy(uuid, kBase, 16);
    uuid[12] = static_cast<std::uint8_t>(cache::kServiceChangedUuid);
    uuid[13] = static_cast<std::uint8_t>(cache::kServiceChangedUuid >> 8);
    std::memcpy(st.wanted, uuid, 16);
    st.watching = true;
    st.listing = false;
    st.found = false;
    st.handle = 0;
    st.next = 65536;
    auto watch = cache::Watch::failed;
    if (procedure([] {
            return client_by_uuid_char_discovery(
                st.connection, st.client, 1, 0xFFFF, cache::kServiceChangedUuid);
        })) {
        if (!st.found) {
            watch = cache::Watch::valid;
        } else {
            const auto handle = st.handle;
            // Its CCC follows the value directly; a short range keeps the search to one exchange.
            const auto end = static_cast<std::uint16_t>(std::min(0xFFFF, handle + 8));
            st.found = false;
            st.boundary = false;
            st.ccc = 0;
            if (handle < end && procedure([&] {
                    return client_all_char_descriptor_discovery(
                        st.connection, st.client, handle + 1, end);
                }) &&
                st.found) {
                std::uint16_t bits = 2; // indications
                // Set before the write: the first indication may follow at once.
                st.service_changed = handle;
                if (procedure(
                        [&] {
                            return client_attr_write(st.connection, st.client, GATT_WRITE_TYPE_REQ,
                                st.ccc, 2, reinterpret_cast<std::uint8_t*>(&bits));
                        },
                        State::Pending::write))
                    watch = cache::Watch::rediscover;
                else
                    st.service_changed = 0;
            }
        }
    }
    st.watching = false;
    return watch;
}

/** @brief Settles once per link whether cached entries may be used; drops them if not. */
void cache_check() {
    cache_forget();
    if (!st.cache.unchecked())
        return;
    const auto watch = watch_database();
    cache_forget(); // a change during the check leaves an empty, unchecked cache
    st.cache.settle(watch);
}

/** @brief Selects a service range in st.first/st.last, asking the peer only on a cache miss.
 *  @return Its cache index, or -1.
 */
int find_service(const BleRequest& request) {
    cache_check();
    int index = st.cache.find_service(request.service);
    if (index < 0) {
        if (!discover_service(request, false) || !st.found)
            return -1;
        index = st.cache.add_service(request.service, {st.first, st.last});
    }
    st.first = st.cache.services[index].range.start;
    st.last = st.cache.services[index].range.end;
    return index;
}

/** @brief Op 7 once, discarding any reply from an earlier attempt. */
BleError list_characteristics_once(const BleRequest& request) {
    st.used = 0;
    st.count = 0;
    st.overflow = false;
    if (find_service(request) < 0)
        return BleError::no_service;
    const std::uint8_t zero = 0;
    append(&zero, 1);
    const bool ok = discover_characteristics(true);
    st.output[0] = st.count;
    return ok ? BleError::none : BleError::no_characteristic;
}

/** @brief Selects the staged characteristic in st.handle and st.properties once. */
BleError resolve_once(const BleRequest& request, BleEvent& event) {
    const int index = find_service(request);
    if (index < 0)
        return BleError::no_service;
    if (request.has_arg1) {
        if (request.arg1 <= st.first || request.arg1 > st.last)
            return BleError::no_characteristic;
        st.handle = static_cast<std::uint16_t>(request.arg1);
        // Read the declaration before the supplied value handle to validate its properties.
        std::uint8_t declaration[19]{};
        auto* output = st.output;
        const auto capacity = st.capacity;
        st.output = declaration;
        st.capacity = sizeof(declaration);
        st.used = 0;
        const bool ok =
            st.handle > 1 &&
            procedure([] { return client_attr_read(st.connection, st.client, st.handle - 1); },
                State::Pending::read);
        const auto size = st.used;
        st.output = output;
        st.capacity = capacity;
        st.used = 0;
        if (!ok || (size != 5 && size != 19) ||
            (declaration[1] | (declaration[2] << 8)) != st.handle)
            return BleError::no_characteristic;
        st.properties = declaration[0];
        const unsigned uuid_offset = size == 5 ? 3 : 15;
        event.uuid = static_cast<std::uint16_t>(
            declaration[uuid_offset] | (declaration[uuid_offset + 1] << 8));
        st.next = static_cast<std::uint32_t>(st.last) + 1;
    } else if (const auto* cached = st.cache.find_characteristic(index, request.characteristic)) {
        st.handle = cached->handle;
        st.properties = cached->properties;
    } else if (!discover_characteristics(false, request.characteristic) || !st.found) {
        return BleError::no_characteristic;
    } else {
        st.cache.add_characteristic(
            index, request.characteristic, {st.handle, static_cast<std::uint8_t>(st.properties)});
    }
    return BleError::none;
}

BleError client_job(const BleRequest& request, BleEvent& event) {
    if (!connect(request))
        return BleError::no_connect;
    if (request.operation == 6) {
        st.count = 0;
        const std::uint8_t zero = 0;
        append(&zero, 1);
        const bool ok = discover_service(request, true);
        st.output[0] = st.count;
        return ok ? BleError::none : BleError::no_service;
    }
    const auto generation = [] { return st.cache_generation.load(); };
    if (request.operation == 7)
        return cache::settled(generation, [&] { return list_characteristics_once(request); });
    const BleError resolved =
        cache::settled(generation, [&] { return resolve_once(request, event); });
    if (resolved != BleError::none)
        return resolved;
    event.handle = st.handle;
    if (!request.has_arg1)
        event.uuid = short_uuid(request.characteristic);
    if (request.operation == 1) {
        if ((st.properties & GATT_CHAR_PROP_READ) == 0)
            return BleError::cannot_read;
        return procedure([] { return client_attr_read(st.connection, st.client, st.handle); },
                   State::Pending::read)
                   ? BleError::none
                   : BleError::cannot_read;
    }
    if (request.operation == 2) {
        const bool confirmed =
            request.confirm || (st.properties & GATT_CHAR_PROP_WRITE_NO_RSP) == 0;
        const auto type = confirmed ? GATT_WRITE_TYPE_REQ : GATT_WRITE_TYPE_CMD;
        if ((st.properties & (confirmed ? GATT_CHAR_PROP_WRITE : GATT_CHAR_PROP_WRITE_NO_RSP)) == 0)
            return BleError::cannot_write;
        if (request.length > 512)
            return BleError::buffer_too_small;
        if (!confirmed)
            return invoke([&] {
                return client_attr_write(st.connection, st.client, GATT_WRITE_TYPE_CMD, st.handle,
                    request.length, const_cast<std::uint8_t*>(request.data));
            }) == GAP_CAUSE_SUCCESS
                       ? BleError::none
                       : BleError::write_failed;
        return procedure(
                   [&] {
                       return client_attr_write(st.connection, st.client, type, st.handle,
                           request.length, const_cast<std::uint8_t*>(request.data));
                   },
                   State::Pending::write)
                   ? BleError::none
                   : BleError::write_failed;
    }
    const bool enable = request.operation == 3;
    Subscription* slot = nullptr;
    for (auto& subscription : st.subscriptions)
        if (subscription.handle == st.handle)
            slot = &subscription;
    if (slot == nullptr && !enable)
        return BleError::none;
    if (slot == nullptr)
        for (auto& subscription : st.subscriptions)
            if (subscription.handle == 0) {
                slot = &subscription;
                break;
            }
    if (slot == nullptr || (st.properties & (GATT_CHAR_PROP_NOTIFY | GATT_CHAR_PROP_INDICATE)) == 0)
        return BleError::cannot_notify;
    if (!enable && st.handle == st.service_changed) {
        // Unsubscribing from the Service Changed we watch only ends delivery to the script.
        invoke([&] {
            *slot = {};
            return true;
        });
        return BleError::none;
    }
    const auto handle = st.handle;
    const auto properties = st.properties;
    // Enumerate every declaration after this value before searching descriptors.
    st.listing = false;
    st.found = true;
    st.next = 65536;
    if (!procedure(
            [] { return client_all_char_discovery(st.connection, st.client, st.first, st.last); }))
        return BleError::cannot_notify;
    st.found = false;
    st.boundary = false;
    st.ccc = 0;
    const auto end = static_cast<std::uint16_t>(std::min<unsigned>(st.last, st.next - 1));
    if (handle >= end)
        return BleError::cannot_notify;
    if (!procedure([&] {
            return client_all_char_descriptor_discovery(st.connection, st.client, handle + 1, end);
        }) ||
        !st.found)
        return BleError::cannot_notify;
    std::uint16_t bits = enable ? ((properties & GATT_CHAR_PROP_NOTIFY) != 0 ? 1 : 2) : 0;
    const auto previous = *slot;
    invoke([&] {
        if (enable)
            *slot = Subscription{handle, event.uuid};
        // A peer may notify before its CCC write response reaches the worker.
        st.notification = enable && request.one_shot;
        st.notified = false;
        return true;
    });
    const auto write_ccc = [&] {
        return client_attr_write(st.connection, st.client,
            request.confirm ? GATT_WRITE_TYPE_REQ : GATT_WRITE_TYPE_CMD, st.ccc, 2,
            reinterpret_cast<std::uint8_t*>(&bits));
    };
    const bool ok = request.confirm ? procedure(write_ccc, State::Pending::write)
                                    : invoke(write_ccc) == GAP_CAUSE_SUCCESS;
    if (!ok) {
        invoke([&] {
            *slot = previous;
            st.notification = false;
            return true;
        });
        return BleError::cannot_notify;
    }
    if (!enable)
        invoke([&] {
            *slot = {};
            return true;
        });
    if (enable && request.one_shot) {
        begin(State::Pending::notification);
        invoke([] {
            if (st.notified)
                finish(0, State::Pending::notification);
            return true;
        });
        const bool received = await();
        invoke([] {
            st.notification = false;
            return true;
        });
        if (!received)
            return BleError::notify_timeout;
        if (st.notification_size > sizeof(st.notification_value))
            return BleError::buffer_too_small;
        append(st.notification_value, st.notification_size);
    }
    return BleError::none;
}

Server* ensure_server() {
    if (st.server == nullptr) {
        auto* server = static_cast<Server*>(std::calloc(1, sizeof(Server)));
        if (server != nullptr) {
            server->mode = 2;
            Guard guard;
            st.server = server;
        }
    }
    return st.server;
}

std::uint32_t permissions(std::uint16_t flags) {
    std::uint32_t result = 0;
    if ((flags & 2) != 0)
        result |= (flags & 0x400) != 0   ? GATT_PERM_READ_AUTHEN_MITM_REQ
                  : (flags & 0x200) != 0 ? GATT_PERM_READ_ENCRYPTED_REQ
                                         : GATT_PERM_READ;
    if ((flags & 12) != 0)
        result |= (flags & 0x2000) != 0   ? GATT_PERM_WRITE_AUTHEN_MITM_REQ
                  : (flags & 0x1000) != 0 ? GATT_PERM_WRITE_ENCRYPTED_REQ
                                          : GATT_PERM_WRITE;
    return result;
}

/** @brief Builds immutable native tables before the first advertising request. */
bool register_services() {
    auto& server = *st.server;
    if (server.frozen)
        return server.registered && !server.registration_failed;
    server.frozen = true; // Partial registration cannot safely be rolled back in this SDK.
    for (unsigned s = 0; s < server.service_count; ++s) {
        auto& service = server.services[s];
        service.start = static_cast<std::uint16_t>(0x100 + s * 0x100);
        auto& primary = service.attributes[service.count++];
        primary.flags = ATTRIB_FLAG_LE;
        primary.type_value[0] = 0;
        primary.type_value[1] = 0x28;
        primary.value_len = short_form(service.uuid) ? 2 : 16;
        primary.p_value_context = short_form(service.uuid) ? service.uuid + 12 : service.uuid;
        primary.permissions = GATT_PERM_READ;
        for (unsigned c = 0; c < server.characteristic_count; ++c) {
            auto& characteristic = server.characteristics[c];
            if (characteristic.service != s)
                continue;
            auto& declaration = service.attributes[service.count++];
            declaration.flags = ATTRIB_FLAG_VALUE_INCL;
            declaration.type_value[0] = 3;
            declaration.type_value[1] = 0x28;
            declaration.type_value[2] = characteristic.flags & 0x3F;
            declaration.value_len = 1;
            declaration.permissions = GATT_PERM_READ;
            characteristic.index = service.count;
            characteristic.handle = service.start + characteristic.index;
            auto& value = service.attributes[service.count++];
            value.flags = ATTRIB_FLAG_VALUE_APPL |
                          (short_form(characteristic.uuid) ? 0 : ATTRIB_FLAG_UUID_128BIT);
            std::memcpy(value.type_value,
                short_form(characteristic.uuid) ? characteristic.uuid + 12 : characteristic.uuid,
                short_form(characteristic.uuid) ? 2 : 16);
            value.permissions = permissions(characteristic.flags);
            if ((characteristic.flags & 0x30) != 0) {
                auto& ccc = service.attributes[service.count++];
                ccc.flags = ATTRIB_FLAG_VALUE_INCL | ATTRIB_FLAG_CCCD_APPL;
                ccc.type_value[0] = 2;
                ccc.type_value[1] = 0x29;
                ccc.value_len = 2;
                ccc.permissions = GATT_PERM_READ | GATT_PERM_WRITE;
            }
        }
        begin(State::Pending::service);
        const bool submitted = invoke([&] {
            const T_FUN_GATT_SERVICE_CBS callbacks{server_read, server_write, server_ccc};
            const bool added = server_add_service_by_start_handle(&service.id,
                reinterpret_cast<std::uint8_t*>(service.attributes),
                service.count * sizeof(T_ATTRIB_APPL), callbacks, service.start);
            st.registering_service = service.id;
            return added;
        });
        if (!submitted || !await()) {
            server.registration_failed = true;
            return false;
        }
    }
    server.registered = true;
    return true;
}

BleError server_characteristic(const BleRequest& request, BleEvent& event) {
    auto* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    if (request.length > 255)
        return BleError::buffer_too_small;
    const auto flags = static_cast<std::uint16_t>(
        request.has_arg1 ? request.arg1 : 2 | 0x10 | 0x20 | (request.confirm ? 8 : 4));
    // Authorization callbacks and signed/reliable writes have no synchronous SDK contract here.
    if ((request.has_arg1 && (request.arg1 < 0 || request.arg1 > 0xFFFF)) ||
        (flags & (0x40 | 0x80 | 0x100 | 0x800 | 0x4000 | 0x8000)) != 0)
        return BleError::cannot_write;
    unsigned s = 0;
    while (s < server->service_count &&
           std::memcmp(server->services[s].uuid, request.service, 16) != 0)
        ++s;
    if (s == server->service_count) {
        if (server->frozen || s == kServices)
            return BleError::no_service;
        std::memcpy(server->services[s].uuid, request.service, 16);
        ++server->service_count;
    }
    unsigned c = 0;
    while (c < server->characteristic_count &&
           (server->characteristics[c].service != s ||
               std::memcmp(server->characteristics[c].uuid, request.characteristic, 16) != 0))
        ++c;
    if (c == server->characteristic_count) {
        if (server->frozen || c == kCharacteristics)
            return BleError::no_characteristic;
        auto& characteristic = server->characteristics[c];
        std::memcpy(characteristic.uuid, request.characteristic, 16);
        characteristic.service = s;
        characteristic.flags = flags;
        ++server->characteristic_count;
    }
    auto& characteristic = server->characteristics[c];
    if (request.has_arg1 && characteristic.flags != flags)
        return BleError::cannot_write;
    event.handle = characteristic.handle;
    event.uuid = short_uuid(characteristic.uuid);
    const bool ok = invoke([&] {
        if (request.length != 0)
            std::memcpy(characteristic.value, request.data, request.length);
        characteristic.length = request.length;
        if (!st.linked || characteristic.ccc == 0)
            return true;
        return server_send_data(st.connection, server->services[s].id, characteristic.index,
            // The pinned btgap.a copies the payload into its own buffer before
            // returning (server_send_data -> memcpy -> btif_gatt_attr_update_req).
            characteristic.value, characteristic.length,
            (characteristic.ccc & 1) != 0 ? GATT_PDU_TYPE_NOTIFICATION : GATT_PDU_TYPE_INDICATION);
    });
    return ok ? BleError::none : BleError::cannot_notify;
}

bool valid_ad(const std::uint8_t* data, std::size_t size) {
    for (std::size_t offset = 0; offset < size;) {
        const auto length = data[offset++];
        if (length == 0)
            return offset == size;
        if (length > size - offset)
            return false;
        offset += length;
    }
    return true;
}

bool advertise() {
    if (st.server == nullptr || !st.server->enabled || !st.opened || st.linked)
        return true;
    begin(State::Pending::advertising);
    if (invoke([] {
            const auto cause = le_adv_start();
            st.advertising_transition = cause == GAP_CAUSE_SUCCESS;
            return cause;
        }) != GAP_CAUSE_SUCCESS) {
        st.waiting = false;
        st.notification = false;
        return false;
    }
    return await();
}

BleError server_advertisement(const BleRequest& request) {
    auto* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    if (request.length > 31)
        return BleError::buffer_too_small;
    if (!valid_ad(request.data, request.length) ||
        (request.has_arg1 && (request.arg1 < 0 || request.arg1 > 2)))
        return BleError::cannot_write;
    if (invoke([] { return st.device.gap_adv_state != GAP_ADV_STATE_IDLE; })) {
        invoke([] { return le_adv_stop(); });
        if (!wait_idle(true))
            return BleError::cannot_write;
    }
    if (request.has_arg1)
        server->mode = request.arg1;
    if (server->mode == 1) {
        reverse(request.address, server->peer);
        server->peer_type = request.address_type;
        if (!amebad_ble::valid_address(server->peer, server->peer_type, false))
            return BleError::cannot_write;
    }
    const bool first = request.operation == 201 && !server->frozen;
    if (request.operation == 201 && !register_services())
        return BleError::no_service;
    const bool configured = invoke([&] {
        const std::uint8_t mode = server->mode == 0   ? GAP_ADTYPE_ADV_NONCONN_IND
                                  : server->mode == 1 ? GAP_ADTYPE_ADV_LDC_DIRECT_IND
                                                      : GAP_ADTYPE_ADV_IND;
        if (le_adv_set_param(GAP_PARAM_ADV_EVENT_TYPE, 1, const_cast<std::uint8_t*>(&mode)) !=
            GAP_CAUSE_SUCCESS)
            return false;
        if (server->mode == 1 &&
            (le_adv_set_param(GAP_PARAM_ADV_DIRECT_ADDR, 6, server->peer) != GAP_CAUSE_SUCCESS ||
                le_adv_set_param(GAP_PARAM_ADV_DIRECT_ADDR_TYPE, 1, &server->peer_type) !=
                    GAP_CAUSE_SUCCESS))
            return false;
        return le_adv_set_param(
                   request.operation == 201 ? GAP_PARAM_ADV_DATA : GAP_PARAM_SCAN_RSP_DATA,
                   request.length, const_cast<std::uint8_t*>(request.data)) == GAP_CAUSE_SUCCESS;
    });
    if (!configured)
        return BleError::cannot_write;
    if (request.operation == 201)
        server->enabled = true;
    if (!advertise())
        return BleError::cannot_write;
    if (first)
        for (unsigned c = 0; c < server->characteristic_count; ++c) {
            const auto handle = server->characteristics[c].handle;
            const std::uint8_t bytes[]{
                static_cast<std::uint8_t>(handle >> 8), static_cast<std::uint8_t>(handle)};
            append(bytes, 2);
        }
    return BleError::none;
}

void close_connection() {
    disconnect();
    invoke([] {
        if (st.device.gap_adv_state == GAP_ADV_STATE_ADVERTISING)
            le_adv_stop();
        if (st.server != nullptr) {
            st.server->enabled = false;
            for (auto& characteristic : st.server->characteristics) {
                characteristic.length = 0;
                characteristic.ccc = 0;
                std::memset(characteristic.value, 0, sizeof(characteristic.value));
            }
        }
        st.waiting = false;
        st.notification = false;
        st.authenticated_pending = false;
        st.output = nullptr;
        return true;
    });
    xQueueReset(st.events);
    st.completed = false;
    st.running = false;
    st.hold = false;
    st.closing = false;
}

void worker(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (st.closing) {
            close_connection();
            continue;
        }
        if (!st.running)
            continue;
        const auto started = xTaskGetTickCount();
        while (!st.ready && !st.failed && !st.closing &&
               xTaskGetTickCount() - started < pdMS_TO_TICKS(15000))
            vTaskDelay(pdMS_TO_TICKS(10));
        if (!st.ready)
            st.failed = true;
        if (st.closing) {
            close_connection();
            continue;
        }
        const auto request = st.request;
        invoke([&] {
            st.pin = request.pin;
            std::uint8_t enabled = true;
            le_bond_set_param(GAP_PARAM_BOND_FIXED_PASSKEY, sizeof(st.pin), &st.pin);
            le_bond_set_param(GAP_PARAM_BOND_FIXED_PASSKEY_ENABLE, sizeof(enabled), &enabled);
            return true;
        });
        st.preserve_link = false;
        BleEvent event{0, request.operation, 0, 0, 0};
        st.output = request.reply;
        st.capacity = request.reply_capacity;
        st.used = 0;
        st.overflow = false;
        st.hold = true;
        BleError error = BleError::none;
        if (!st.ready)
            error = BleError::no_connect;
        else if (request.operation == 201 || request.operation == 202)
            error = server_advertisement(request);
        else if (request.operation == 211)
            error = server_characteristic(request, event);
        else if (request.operation == 5)
            disconnect();
        else {
            error = client_job(request, event);
            if (error != BleError::none)
                cache_forget(true); // a changed peer database must not fail the next job too
        }
        if (st.overflow)
            error = BleError::buffer_too_small;
        if (!st.preserve_link &&
            ((request.one_shot && request.operation < 200) || error != BleError::none))
            if (!disconnect())
                error = BleError::disconnected;
        // Barrier: no native callback may retain the caller's output after this point.
        invoke([] {
            st.waiting = false;
            st.output = nullptr;
            return true;
        });
        if (st.closing) {
            close_connection();
            continue;
        }
        event.error = static_cast<std::uint16_t>(error);
        event.length = st.used;
        st.completion = event;
        st.last_operation = event.operation;
        st.last_error = event.error;
        st.completed = true;
        st.running = false;
        st.hold = false;
    }
}
} // namespace

bool ble_start() {
    if (st.started)
        return !st.failed;
    st.started = true;
    // Our queues and the worker outlive a stop; the stack's message queues do not.
    if (st.calls == nullptr) {
        st.calls = xQueueCreate(1, sizeof(Call));
        st.events = xQueueCreate(4, sizeof(Event));
        st.call_mutex = xSemaphoreCreateMutex();
        st.called = xSemaphoreCreateBinary();
        st.signal = xSemaphoreCreateBinary();
    }
    st.io = xQueueCreate(32, sizeof(T_IO_MSG));
    st.native_events = xQueueCreate(64, 1);
    if (st.calls == nullptr || st.events == nullptr || st.io == nullptr ||
        st.native_events == nullptr || st.call_mutex == nullptr || st.called == nullptr ||
        st.signal == nullptr) {
        st.failed = true;
        return false;
    }
    // bte_init() opens the HCI transport, which refuses to start without the Wi-Fi radio.
    const auto since = xTaskGetTickCount();
    while (!newmota_amebad_ble_radio_ready() && xTaskGetTickCount() - since < pdMS_TO_TICKS(15000))
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!newmota_amebad_ble_radio_ready()) {
        st.failed = true;
        return false;
    }
    gap_config_max_le_link_num(1);
    gap_config_max_le_paired_device(1);
    gap_config_max_mtu_size(247);
    gap_config_le_key_storage_flag(amebad_ble::kBondMask);
    if (!bte_init() || !le_gap_init(1)) {
        st.failed = true;
        return false;
    }
    const std::uint8_t duplicate = GAP_SCAN_FILTER_DUPLICATE_DISABLE;
    const std::uint8_t channels = GAP_ADVCHAN_ALL;
    const std::uint16_t interval = 160;
    char name[] = "newmota";
    le_set_gap_param(GAP_PARAM_DEVICE_NAME, sizeof(name) - 1, name);
    // The SDK PRNG is no optimal entropy source.
    const std::uint16_t auth =
        GAP_AUTHEN_BIT_BONDING_FLAG | GAP_AUTHEN_BIT_MITM_FLAG | GAP_AUTHEN_BIT_SC_FLAG;
    const std::uint8_t pairing = GAP_PAIRING_MODE_PAIRABLE, io = GAP_IO_CAP_KEYBOARD_DISPLAY;
    le_scan_set_param(GAP_PARAM_SCAN_FILTER_DUPLICATES, 1, const_cast<std::uint8_t*>(&duplicate));
    le_adv_set_param(GAP_PARAM_ADV_CHANNEL_MAP, 1, const_cast<std::uint8_t*>(&channels));
    le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MIN, 2, const_cast<std::uint16_t*>(&interval));
    le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MAX, 2, const_cast<std::uint16_t*>(&interval));
    gap_set_param(GAP_PARAM_BOND_AUTHEN_REQUIREMENTS_FLAGS, 2, const_cast<std::uint16_t*>(&auth));
    gap_set_param(GAP_PARAM_BOND_PAIRING_MODE, 1, const_cast<std::uint8_t*>(&pairing));
    gap_set_param(GAP_PARAM_BOND_IO_CAPABILITIES, 1, const_cast<std::uint8_t*>(&io));
    client_init(1);
    if (!client_register_spec_client_cb(&st.client, &kClientCallbacks)) {
        st.failed = true;
        return false;
    }
    server_init(kServices);
    server_register_app_cb(server_event);
    le_register_app_cb(gap_callback);
    bt_coex_init();
    if (xTaskCreate(pump, "ble_gap", 2048, nullptr, 2, &st.pump) != pdPASS ||
        (st.worker == nullptr &&
            xTaskCreate(worker, "ble_job", 2048, nullptr, 1, &st.worker) != pdPASS)) {
        st.failed = true;
        return false;
    }
    return true;
}

/** @brief Realtek's own off sequence: drain the link, end the message task, bte_deinit(). */
bool ble_stop() {
    if (!st.started)
        return true;
    ble_connection_close();
    const auto since = xTaskGetTickCount();
    while (st.closing || st.running) {
        if (xTaskGetTickCount() - since >= pdMS_TO_TICKS(8000))
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // The message task is created last, so it exists only on a stack bte_init() brought up.
    const bool initialized = st.pump != nullptr;
    if (initialized) {
        invoke([] {
            st.quit = true;
            return true;
        });
        while (st.pump != nullptr)
            vTaskDelay(pdMS_TO_TICKS(10));
        bte_deinit();
    }
    st.quit = false;
    st.ready = false;
    if (st.io != nullptr)
        vQueueDelete(st.io);
    if (st.native_events != nullptr)
        vQueueDelete(st.native_events);
    st.io = nullptr;
    st.native_events = nullptr;
    // The stack forgot its GATT database and client registration.
    {
        Guard guard;
        std::free(st.server);
        st.server = nullptr;
        st.device = {};
        st.connection_info = {};
    }
    st.client = 0xFF;
    st.linked = false;
    st.connecting = false;
    st.scan_transition = false;
    st.advertising_transition = false;
    st.advertisement_present = false;
    st.failed = false;
    st.started = false;
    return true;
}

bool ble_ready() {
    return st.ready && !st.failed;
}

void ble_scan_set(BleScanConsumer consumer, bool on) {
    const auto bit = static_cast<unsigned>(consumer);
    if (on)
        st.consumers.fetch_or(bit);
    else
        st.consumers.fetch_and(~bit);
}

bool ble_scan_configure(const BleScanParameters& parameters) {
    if (!ble_scan_valid(parameters))
        return false;
    Guard guard;
    st.scan_parameters = parameters;
    st.scan_dirty = true;
    return true;
}

BleScanParameters ble_scan_parameters() {
    Guard guard;
    return st.scan_parameters;
}

bool ble_scan_watch(const std::uint8_t* address, std::uint8_t type) {
    if (address == nullptr || type > 1)
        return false;
    Watch watch{};
    reverse(address, watch.address);
    watch.type = type;
    Guard guard;
    for (unsigned i = 0; i < st.watch_count; ++i)
        if (std::memcmp(&st.watches[i], &watch, sizeof(watch)) == 0)
            return true;
    if (st.watch_count == kWatches)
        return false;
    st.watches[st.watch_count++] = watch;
    return true;
}

void ble_scan_unwatch() {
    Guard guard;
    st.watch_count = 0;
}

bool ble_scan_watched(std::size_t index, std::uint8_t* address, std::uint8_t& address_type) {
    Watch entry{};
    {
        Guard guard;
        if (index >= st.watch_count)
            return false;
        entry = st.watches[index];
    }
    reverse(entry.address, address);
    address_type = entry.type;
    return true;
}

bool ble_scan_pending() {
    Guard guard;
    return st.advertisement_present &&
           (!st.advertisement_waiting ||
               xTaskGetTickCount() - st.advertisement_time >= pdMS_TO_TICKS(50));
}

bool ble_scan_take(BleAdvertisement& advertisement) {
    Guard guard;
    if (!st.advertisement_present ||
        (st.advertisement_waiting &&
            xTaskGetTickCount() - st.advertisement_time < pdMS_TO_TICKS(50)))
        return false;
    advertisement = st.advertisement;
    st.advertisement_present = false;
    st.advertisement_waiting = false;
    return true;
}

std::uint32_t ble_scan_dropped() {
    return st.scan_drops;
}

bool ble_connection_open() {
    if (!st.started || st.failed || st.closing)
        return false;
    st.opened = true;
    return true;
}

void ble_connection_close() {
    st.opened = false;
    st.consumers.fetch_and(~static_cast<unsigned>(BleScanConsumer::berry));
    if (!st.started || st.failed)
        return;
    st.closing = true;
    xTaskNotifyGive(st.worker);
}

bool ble_connection_busy() {
    return st.running || st.completed || st.closing;
}

bool ble_connection_request(const BleRequest& request) {
    if (!st.opened || st.failed || ble_connection_busy() || request.pin > 999999 ||
        (request.length != 0 && request.data == nullptr) || request.reply == nullptr ||
        request.reply_capacity == 0)
        return false;
    if (request.operation != 1 && request.operation != 2 && request.operation != 3 &&
        request.operation != 4 && request.operation != 5 && request.operation != 6 &&
        request.operation != 7 && request.operation != 201 && request.operation != 202 &&
        request.operation != 211)
        return false;
    st.request = request;
    st.running = true;
    xTaskNotifyGive(st.worker);
    return true;
}

bool ble_connection_pending() {
    return st.completed || st.authenticated_pending ||
           (!st.running && st.events != nullptr && uxQueueMessagesWaiting(st.events) != 0);
}

bool ble_connection_poll(BleEvent& event, std::uint8_t* payload, std::size_t capacity) {
    if (!st.opened || st.closing || st.running)
        return false;
    if (st.completed) {
        event = st.completion;
        st.completed = false;
        return true;
    }
    if (st.authenticated_pending) {
        BondRecord record;
        invoke([&] {
            record = st.authentication;
            st.authenticated_pending = false;
            return true;
        });
        event = {0, 230, 0, 0, sizeof(record)};
        if (capacity < sizeof(record) || payload == nullptr) {
            event.error = static_cast<std::uint16_t>(BleError::buffer_too_small);
            event.length = 0;
        } else {
            std::memcpy(payload, &record, sizeof(record));
        }
        return true;
    }
    Event item{};
    if (st.events == nullptr || xQueueReceive(st.events, &item, 0) != pdTRUE)
        return false;
    event = item.event;
    if (event.length > capacity || (event.length != 0 && payload == nullptr)) {
        event.length = 0;
        event.error = static_cast<std::uint16_t>(BleError::buffer_too_small);
    } else if (event.length != 0) {
        std::memcpy(payload, item.data, event.length);
    }
    return true;
}

std::uint32_t ble_connection_dropped() {
    return st.event_drops;
}

bool ble_configure(const BleRequest& request) {
    if (!ble_ready() || ble_connection_busy() || (request.length != 0 && request.data == nullptr))
        return false;
    if (request.operation == 231) {
        if (request.address_type > 1 || st.linked || st.connecting)
            return false;
        std::uint8_t address[6];
        reverse(request.address, address);
        if (request.address_type == 0)
            std::memcpy(address, st.public_address, 6);
        if (!amebad_ble::valid_address(address, request.address_type))
            return false;
        if (invoke([&] {
                return le_get_bond_dev_num() != 0 && (st.own_type != request.address_type ||
                                                         std::memcmp(st.local, address, 6) != 0);
            }))
            return false;
        st.hold = true;
        invoke([] {
            if (st.server != nullptr)
                st.server->enabled = false;
            if (st.device.gap_adv_state == GAP_ADV_STATE_ADVERTISING)
                le_adv_stop();
            return true;
        });
        bool applied = wait_idle() && wait_idle(true);
        if (applied && request.address_type == 1) {
            begin(State::Pending::address);
            applied = invoke([&] {
                return !st.linked && !st.connecting &&
                       le_set_rand_addr(address) == GAP_CAUSE_SUCCESS;
            });
            if (applied) {
                applied = await();
                if (!applied)
                    st.failed = true;
            } else
                st.waiting = false;
        }
        if (applied)
            applied = invoke([&] {
                std::uint8_t type = request.address_type;
                if (st.linked || st.connecting ||
                    le_adv_set_param(GAP_PARAM_ADV_LOCAL_ADDR_TYPE, 1, &type) !=
                        GAP_CAUSE_SUCCESS ||
                    le_scan_set_param(GAP_PARAM_SCAN_LOCAL_ADDR_TYPE, 1, &type) !=
                        GAP_CAUSE_SUCCESS) {
                    st.failed = true;
                    return false;
                }
                Guard guard;
                st.own_type = type;
                std::memcpy(st.local, address, 6);
                return true;
            });
        st.hold = false;
        return applied;
    }
    if (request.operation == 233) {
        if (request.length == 0 || request.length >= GAP_DEVICE_NAME_LEN)
            return false;
        return invoke([&] {
            return le_set_gap_param(
                GAP_PARAM_DEVICE_NAME, request.length, const_cast<std::uint8_t*>(request.data));
        }) == GAP_CAUSE_SUCCESS;
    }
    if (request.operation != 232 || request.length != 5 || request.data[0] > 2)
        return false;
    const auto minimum = static_cast<std::uint16_t>(request.data[1] | (request.data[2] << 8));
    const auto maximum = static_cast<std::uint16_t>(request.data[3] | (request.data[4] << 8));
    if (minimum < 32 || maximum < minimum || maximum > 16384 || ensure_server() == nullptr)
        return false;
    return invoke([&] {
        if (st.device.gap_adv_state != GAP_ADV_STATE_IDLE)
            return false;
        st.server->mode = request.data[0];
        return le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MIN, 2,
                   const_cast<std::uint16_t*>(&minimum)) == GAP_CAUSE_SUCCESS &&
               le_adv_set_param(GAP_PARAM_ADV_INTERVAL_MAX, 2,
                   const_cast<std::uint16_t*>(&maximum)) == GAP_CAUSE_SUCCESS;
    });
}

bool ble_store_bond(const std::uint8_t* data, std::size_t size) {
    if (!ble_ready() || ble_connection_busy() || st.linked || st.connecting || data == nullptr ||
        size != sizeof(BondRecord))
        return false;
    BondRecord record;
    std::memcpy(&record, data, sizeof(record));
    return invoke([&] {
        T_GAP_CONN_INFO connection{};
        if (st.device.gap_adv_state != GAP_ADV_STATE_IDLE ||
            st.device.gap_conn_state != GAP_CONN_DEV_STATE_IDLE ||
            (le_get_conn_info(0, &connection) &&
                connection.conn_state != GAP_CONN_STATE_DISCONNECTED))
            return false;
        if (!amebad_ble::valid_record(record, kRevision, st.own_type, st.local) ||
            le_get_dev_bond_info_len() != amebad_ble::kNativeBondSize)
            return false;
        auto* existing =
            le_find_key_entry(record.keys, static_cast<T_GAP_REMOTE_ADDR_TYPE>(record.keys[6]));
        if (existing != nullptr) {
            std::uint8_t keys[amebad_ble::kNativeBondSize]{};
            if (!le_get_dev_bond_info(existing, keys))
                return false;
            amebad_ble::canonicalize(keys);
            return std::memcmp(keys, record.keys, sizeof(keys)) == 0;
        }
        auto* occupied = le_find_key_entry_by_idx(0);
        if (le_get_bond_dev_num() != 0 || (occupied != nullptr && occupied->is_used))
            return false;
        bool exists = false;
        auto* imported = le_set_dev_bond_info(sizeof(record.keys), record.keys, &exists);
        pairable(false);
        return imported != nullptr && !exists;
    });
}

void ble_info(BleInfo& info) {
    info = {};
    info.version = 5;
    info.ready = ble_ready() && st.opened && !ble_connection_busy();
    info.pending = ble_connection_pending() || ble_connection_busy();
    info.mtu = 247;
    if (ble_ready())
        invoke([&] {
            info.roles = (st.device.gap_scan_state == GAP_SCAN_STATE_SCANNING ? 1 : 0) |
                         (st.linked && st.connection_info.master ? 2 : 0) |
                         (st.server != nullptr ? 4 : 0) |
                         (st.device.gap_adv_state == GAP_ADV_STATE_ADVERTISING ? 8 : 0);
            info.bonds = le_get_bond_dev_num();
            return true;
        });
    info.last_operation = st.last_operation;
    info.last_error = st.last_error;
    info.dropped_events = st.event_drops;
    info.dropped_advertisements = st.scan_drops;
    {
        Guard guard;
        reverse(st.local, info.local_address);
        info.connection = st.connection_info;
    }
    if (info.connection.present)
        info.mtu = info.connection.mtu;
    if (st.pump != nullptr)
        info.scan_stack_free = uxTaskGetStackHighWaterMark(st.pump) * sizeof(StackType_t);
    if (st.worker != nullptr)
        info.connection_stack_free = uxTaskGetStackHighWaterMark(st.worker) * sizeof(StackType_t);
}
} // namespace newmota::platform
