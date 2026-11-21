/*
  ble_connection.cpp - ESP-IDF NimBLE connection task, GATT client and server

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_connection.cpp
 * @brief ESP-IDF NimBLE connection task: GATT client, GATT server and advertising.
 *
 * A worker task, created on demand, runs each job as one linear sequence and
 * blocks on a semaphore that the host task's GAP and GATT callbacks give. The
 * host task never touches application data beyond the event queue. While a
 * central is connected to the server, client jobs run over that same link.
 */

#include "ble_internal.h"
#include "newmota/platform/ble_cache.h"

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace newmota::platform {
namespace {
constexpr std::size_t kQueueBytes = 1024;
constexpr std::uint32_t kWorkerStackBytes = 4096;
constexpr std::size_t kSubscriptionLimit = 8;
constexpr std::size_t kServiceLimit = 4;
constexpr std::size_t kCharacteristicLimit = 12;
constexpr std::int32_t kConnectTimeoutMs = 10000;
constexpr TickType_t kProcedureTimeout = pdMS_TO_TICKS(5000);
constexpr TickType_t kSecurityTimeout = pdMS_TO_TICKS(30000);
constexpr TickType_t kShutdownTimeout = pdMS_TO_TICKS(8000);
constexpr std::uint32_t kNotifyWaitMs = 15000;
constexpr std::uint16_t kCccdUuid = 0x2902;
constexpr std::uint16_t kDescriptorSearch = 8;
constexpr std::uint8_t kBaseUuid[12] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
constexpr std::uint8_t kNotification = 103;

struct Subscription {
    std::uint16_t handle;
    std::uint16_t uuid;
};

struct EventItem {
    std::uint16_t uuid;
    std::uint16_t handle;
    std::uint8_t operation;
    std::uint8_t length;
    std::uint8_t data[255];
};

constexpr std::size_t kEventHeader = offsetof(EventItem, data);
static_assert(2 * sizeof(ble_store_value_sec) <= 255, "security records must fit one event");

/** @brief The characteristic a client job operates on. */
struct Target {
    std::uint16_t value_handle;
    std::uint16_t range_end; ///< Last handle of the owning service, or a bounded guess.
    std::uint16_t uuid;
    std::uint8_t properties;
};

namespace cache = ble_cache;

/** @brief What a cached characteristic needs to skip its discovery. */
struct CachedCharacteristic {
    std::uint16_t value_handle;
    std::uint8_t properties;
};

struct ServerCharacteristic {
    ble_uuid_any_t uuid;
    std::uint8_t uuid_bytes[16];
    std::uint8_t service;
    std::uint16_t flags;
    std::uint16_t value_handle;
    std::uint8_t* value;
    std::uint8_t length;
    std::uint8_t capacity;
};

struct ServerService {
    ble_uuid_any_t uuid;
    std::uint8_t uuid_bytes[16];
};

/** @brief Allocated on the first server operation; absent in client-only use. */
struct Server {
    ServerService services[kServiceLimit];
    ServerCharacteristic characteristics[kCharacteristicLimit];
    ble_gatt_svc_def service_defs[kServiceLimit + 1];
    ble_gatt_chr_def characteristic_defs[kCharacteristicLimit + kServiceLimit];
    std::uint8_t service_count, characteristic_count;
    bool registered;          ///< The GATT database has been built and started.
    bool advertising_enabled; ///< An advertisement has been set; restart it after a link ends.
    std::uint8_t adv_mode;    ///< 0 not connectable, otherwise undirected connectable.
    std::uint16_t itvl_min, itvl_max; ///< Advertising interval in 0.625 ms units; 0 is the default.
    std::uint16_t connection;         ///< The accepted central, or none.
};

struct State {
    BleRequest request;
    BleEvent completion;
    TaskHandle_t task;
    RingbufHandle_t queue;
    StaticSemaphore_t signal_storage;
    SemaphoreHandle_t signal;
    std::atomic<bool> running;
    std::atomic<bool> completed;
    std::atomic<bool> quit;
    std::atomic<bool> notified;
    std::atomic<bool> awaiting_security;
    std::atomic<int> unsolicited;
    std::atomic<std::uint32_t> queued;
    std::atomic<std::uint32_t> dropped;
    std::atomic<std::uint32_t> cache_generation; ///< Advanced whenever cached handles may be stale.
    std::atomic<std::uint32_t>
        watched; ///< cache::watch_key() of the Service Changed we watch, or 0.
    cache::Cache<CachedCharacteristic> cache;
    std::uint16_t connection; ///< The outbound link.
    ble_addr_t peer;
    std::uint32_t pin;
    Subscription subscriptions[kSubscriptionLimit];
    std::uint8_t subscription_count;
    std::uint8_t last_operation;
    std::uint16_t last_error;
    Server* server;
    // Scratch shared with the callbacks of the one procedure being awaited.
    int status;
    std::uint16_t found_start, found_end, found_handle, cccd_handle;
    std::uint8_t found_properties, count;
    bool found, stop_descriptors;
    std::uint8_t* out;
    std::size_t out_capacity, out_used;
    bool out_overflow;
};

State st{};
portMUX_TYPE value_lock = portMUX_INITIALIZER_UNLOCKED;

[[nodiscard]] ble_uuid_any_t make_uuid(const std::uint8_t* value) {
    ble_uuid_any_t uuid{};
    if (std::memcmp(value, kBaseUuid, sizeof(kBaseUuid)) == 0 && value[14] == 0 && value[15] == 0) {
        uuid.u16.u.type = BLE_UUID_TYPE_16;
        uuid.u16.value = static_cast<std::uint16_t>(value[12] | (value[13] << 8));
    } else {
        uuid.u128.u.type = BLE_UUID_TYPE_128;
        std::memcpy(uuid.u128.value, value, 16);
    }
    return uuid;
}

[[nodiscard]] std::uint16_t short_uuid(const std::uint8_t* value) {
    return static_cast<std::uint16_t>(value[12] | (value[13] << 8));
}

/** @brief The link client jobs run over: the accepted central if any, else the outbound link. */
[[nodiscard]] std::uint16_t active_link() {
    if (st.server != nullptr && st.server->connection != BLE_HS_CONN_HANDLE_NONE)
        return st.server->connection;
    return st.connection;
}

/** @brief A link came or went: nothing learned about the previous peer's database still holds. */
void link_changed() {
    st.watched = 0;
    ++st.cache_generation;
}

// ---- Event queue

void post(std::uint8_t operation, std::uint16_t uuid, std::uint16_t handle, const void* data,
    std::size_t length) {
    EventItem item;
    item.uuid = uuid;
    item.handle = handle;
    item.operation = operation;
    item.length = static_cast<std::uint8_t>(length);
    if (length != 0)
        std::memcpy(item.data, data, length);
    if (length > sizeof(item.data) || st.queue == nullptr ||
        xRingbufferSend(st.queue, &item, kEventHeader + length, 0) != pdTRUE) {
        ++st.dropped;
        return;
    }
    ++st.queued;
}

void queue_notification(const ble_gap_event& event) {
    const auto& rx = event.notify_rx;
    const std::size_t length = OS_MBUF_PKTLEN(rx.om);
    std::uint16_t uuid = 0;
    bool subscribed = false;
    for (std::uint8_t i = 0; i < st.subscription_count; ++i)
        if (st.subscriptions[i].handle == rx.attr_handle) {
            uuid = st.subscriptions[i].uuid;
            subscribed = true;
        }
    const bool watched = st.watched == cache::watch_key(rx.conn_handle, rx.attr_handle);
    // Service Changed may also arrive on a configuration the peer kept from an earlier bond or
    // that a script subscribed. Erring towards forgetting costs one rediscovery.
    if (watched || (rx.indication && (!subscribed || uuid == cache::kServiceChangedUuid)))
        ++st.cache_generation;
    if (watched && !subscribed)
        return; // only our own watch asked for it
    st.notified = true;
    std::uint8_t data[255];
    if (length > sizeof(data) || os_mbuf_copydata(rx.om, 0, static_cast<int>(length), data) != 0) {
        ++st.dropped;
        return;
    }
    post(kNotification, uuid, rx.attr_handle, data, length);
}

// ---- Procedure plumbing

/** @brief Wakes the worker with the result of the awaited procedure. */
void finish(int status) {
    st.status = status;
    xSemaphoreGive(st.signal);
}

void begin() {
    (void)xSemaphoreTake(st.signal, 0);
    st.found = false;
    st.stop_descriptors = false;
    st.cccd_handle = 0;
}

[[nodiscard]] int await(TickType_t timeout) {
    if (xSemaphoreTake(st.signal, timeout) != pdTRUE)
        return BLE_HS_ETIMEOUT;
    return st.status;
}

/** @brief Appends bytes to the reply, flagging overflow instead of truncating. */
void append(const void* data, std::size_t size) {
    if (st.out_used + size > st.out_capacity) {
        st.out_overflow = true;
        return;
    }
    std::memcpy(st.out + st.out_used, data, size);
    st.out_used += size;
}

void append(std::uint8_t byte) {
    append(&byte, 1);
}

void append_uuid(const ble_uuid_any_t& uuid) {
    append(uuid.u.type); // the type constants equal the width in bits: 16, 32 or 128
    switch (uuid.u.type) {
    case BLE_UUID_TYPE_16:
        append(&uuid.u16.value, 2);
        break;
    case BLE_UUID_TYPE_32:
        append(&uuid.u32.value, 4);
        break;
    default:
        append(uuid.u128.value, 16);
        break;
    }
}

void append_mbuf(const os_mbuf* om) {
    const std::size_t size = OS_MBUF_PKTLEN(om);
    if (st.out_used + size > st.out_capacity) {
        st.out_overflow = true;
        return;
    }
    (void)os_mbuf_copydata(om, 0, static_cast<int>(size), st.out + st.out_used);
    st.out_used += size;
}

[[nodiscard]] bool needs_security(int status) {
    return status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
           status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC);
}

/** @brief Pairs or encrypts the active link, answering a passkey request with the staged PIN. */
[[nodiscard]] bool secure() {
    begin();
    st.awaiting_security = true;
    bool ok = ble_gap_security_initiate(active_link()) == 0;
    int status = ok ? await(kSecurityTimeout) : -1;
    if (status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) { // the peer lost its bond: pair anew
        ble_gap_conn_desc desc;
        if (ble_gap_conn_find(active_link(), &desc) == 0)
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        begin();
        status = ble_gap_security_initiate(active_link()) == 0 ? await(kSecurityTimeout) : -1;
    }
    st.awaiting_security = false;
    return status == 0;
}

/** @brief Ends the active link; the host fails its pending procedures before the disconnect. */
void drop_link();

/** @brief Runs one asynchronous procedure to completion, securing the link once if required
 *  and allowed.
 */
template <typename Start> [[nodiscard]] bool procedure(Start start, bool may_secure = true) {
    for (int attempt = 0;; ++attempt) {
        begin();
        if (start() != 0)
            return false;
        const int status = await(kProcedureTimeout);
        if (status == 0)
            return true;
        if (status == BLE_HS_ETIMEOUT) {
            drop_link(); // late callbacks would write into a reused reply or complete the next job
            return false;
        }
        if (attempt > 0 || !may_secure || !needs_security(status) || !secure())
            return false;
    }
}

// ---- Security events shared by both link roles

void post_authenticated(std::uint16_t connection) {
    ble_gap_conn_desc desc;
    if (ble_gap_conn_find(connection, &desc) != 0)
        return;
    ble_store_key_sec key{};
    key.peer_addr = desc.peer_id_addr;
    ble_store_value_sec records[2]{};
    (void)ble_store_read_our_sec(&key, &records[0]);
    (void)ble_store_read_peer_sec(&key, &records[1]);
    post(230, 0, 0, records, sizeof(records));
}

int common_event(ble_gap_event* event) {
    switch (event->type) {
    case BLE_GAP_EVENT_NOTIFY_RX:
        queue_notification(*event);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        ble_sm_io io{};
        io.action = event->passkey.params.action;
        if (io.action == BLE_SM_IOACT_INPUT || io.action == BLE_SM_IOACT_DISP)
            io.passkey = st.pin;
        else if (io.action == BLE_SM_IOACT_NUMCMP)
            io.numcmp_accept = 1;
        else
            break;
        (void)ble_sm_inject_io(event->passkey.conn_handle, &io);
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

int on_client_event(ble_gap_event* event, void*) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            st.connection = event->connect.conn_handle;
            link_changed();
            ble_detail::scan_links(1);
        }
        finish(event->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        st.connection = BLE_HS_CONN_HANDLE_NONE;
        link_changed();
        st.subscription_count = 0;
        ble_detail::scan_links(-1);
        if (!st.running)
            st.unsolicited = event->disconnect.reason;
        finish(BLE_HS_ENOTCONN);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (st.awaiting_security)
            finish(event->enc_change.status);
        if (event->enc_change.status == 0)
            post_authenticated(event->enc_change.conn_handle);
        return 0;
    default:
        return common_event(event);
    }
}

// ---- GATT client procedures

int on_mtu(uint16_t, const ble_gatt_error* error, uint16_t, void*) {
    finish(error->status);
    return 0;
}

int on_write(uint16_t, const ble_gatt_error* error, ble_gatt_attr*, void*) {
    finish(error->status);
    return 0;
}

int on_read(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
    if (error->status == 0 && attr != nullptr)
        append_mbuf(attr->om);
    else
        finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_service(uint16_t, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (error->status == 0) {
        if (!st.found) {
            st.found = true;
            st.found_start = service->start_handle;
            st.found_end = service->end_handle;
        }
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_service_listing(uint16_t, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (error->status == 0) {
        append_uuid(service->uuid);
        ++st.count;
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_characteristic(
    uint16_t, const ble_gatt_error* error, const ble_gatt_chr* characteristic, void*) {
    if (error->status == 0) {
        if (!st.found) {
            st.found = true;
            st.found_handle = characteristic->val_handle;
            st.found_properties = characteristic->properties;
        }
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_characteristic_listing(
    uint16_t, const ble_gatt_error* error, const ble_gatt_chr* characteristic, void*) {
    if (error->status == 0) {
        append_uuid(characteristic->uuid);
        append(characteristic->properties);
        append(&characteristic->val_handle, 2);
        ++st.count;
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_descriptor(
    uint16_t, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* descriptor, void*) {
    if (error->status != 0) {
        finish(error->status == BLE_HS_EDONE ? 0 : error->status);
        return 0;
    }
    if (st.stop_descriptors || st.found)
        return 0;
    if (descriptor->uuid.u.type == BLE_UUID_TYPE_16) {
        const auto value = descriptor->uuid.u16.value;
        if (value == kCccdUuid) {
            st.found = true;
            st.cccd_handle = descriptor->handle;
        } else if (value >= 0x2800 && value <= 0x2803) {
            st.stop_descriptors = true; // the next declaration ends this characteristic
        }
    }
    return 0;
}

[[nodiscard]] bool same_peer(const ble_addr_t& peer) {
    return peer.type == st.peer.type && std::memcmp(peer.val, st.peer.val, 6) == 0;
}

void disconnect_outbound();

void terminate(std::uint16_t connection) {
    begin();
    if (ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM) == 0)
        (void)await(kProcedureTimeout);
}

void drop_link() {
    if (active_link() == st.connection)
        disconnect_outbound();
    else
        terminate(active_link());
}

void disconnect_outbound() {
    if (st.connection == BLE_HS_CONN_HANDLE_NONE)
        return;
    terminate(st.connection);
    if (st.connection != BLE_HS_CONN_HANDLE_NONE) { // the event never came
        st.connection = BLE_HS_CONN_HANDLE_NONE;
        link_changed();
        ble_detail::scan_links(-1);
    }
    st.subscription_count = 0;
}

/** @brief Makes a link available: the accepted central, or a fresh outbound connection. */
[[nodiscard]] bool connect(const BleRequest& request) {
    if (st.server != nullptr && st.server->connection != BLE_HS_CONN_HANDLE_NONE)
        return true;
    ble_addr_t peer{};
    peer.type = request.address_type;
    ble_detail::reverse_address(request.address, peer.val);
    if (st.connection != BLE_HS_CONN_HANDLE_NONE) {
        if (same_peer(peer))
            return true;
        disconnect_outbound();
    }
    ble_detail::scan_hold(true);
    begin();
    bool connected = ble_gap_connect(ble_detail::own_address_type, &peer, kConnectTimeoutMs,
                         nullptr, on_client_event, nullptr) == 0;
    if (connected && await(pdMS_TO_TICKS(kConnectTimeoutMs + 1000)) != 0) {
        (void)ble_gap_conn_cancel();
        connected = false;
    }
    ble_detail::scan_hold(false);
    if (!connected)
        return false;
    st.peer = peer;
    ble_gap_upd_params parameters{};
    parameters.itvl_min = 8;
    parameters.itvl_max = 16;
    parameters.supervision_timeout = 1000;
    (void)ble_gap_update_params(st.connection, &parameters);
    begin();
    if (ble_gattc_exchange_mtu(st.connection, on_mtu, nullptr) == 0 &&
        await(kProcedureTimeout) == BLE_HS_ETIMEOUT) {
        disconnect_outbound();
        return false;
    }
    return true;
}

void reply_begin(const BleRequest& request) {
    st.out = request.reply;
    st.out_capacity = request.reply_capacity;
    st.out_used = 0;
    st.out_overflow = false;
}

/** @brief Empties the cache when the link changed since it was filled, or on request. */
void cache_forget(bool always = false) {
    st.cache.forget(st.cache_generation, always);
}

/** @brief Makes the peer indicate database changes to us, unless its database is fixed.
 *  Never pairs.
 */
[[nodiscard]] cache::Watch watch_database() {
    const auto connection = active_link();
    if (st.watched != 0 && (st.watched >> 16) == connection)
        return cache::Watch::valid;
    const ble_uuid16_t uuid = BLE_UUID16_INIT(cache::kServiceChangedUuid);
    if (!procedure(
            [&] {
                return ble_gattc_disc_chrs_by_uuid(
                    connection, 1, 0xFFFF, &uuid.u, on_characteristic, nullptr);
            },
            false))
        return cache::Watch::failed;
    if (!st.found)
        return cache::Watch::valid;
    const auto handle = st.found_handle;
    const auto last =
        static_cast<std::uint16_t>(std::min<unsigned>(0xFFFF, handle + kDescriptorSearch));
    if (!procedure(
            [&] {
                return ble_gattc_disc_all_dscs(connection, handle, last, on_descriptor, nullptr);
            },
            false) ||
        !st.found)
        return cache::Watch::failed;
    const auto cccd = st.cccd_handle;
    const std::uint8_t value[2] = {2, 0}; // indications
    // Set before the write: the first indication may follow at once.
    st.watched = cache::watch_key(connection, handle);
    if (procedure(
            [&] {
                return ble_gattc_write_flat(
                    connection, cccd, value, sizeof(value), on_write, nullptr);
            },
            false))
        return cache::Watch::rediscover;
    st.watched = 0;
    return cache::Watch::failed;
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

/** @brief Finds a service on the active link, asking the peer only on a cache miss.
 *  @return Its index in the cache, or -1.
 */
[[nodiscard]] int find_service(const std::uint8_t* uuid) {
    cache_check();
    const int index = st.cache.find_service(uuid);
    if (index >= 0)
        return index;
    const auto connection = active_link();
    const auto service = make_uuid(uuid);
    if (!procedure([&] {
            return ble_gattc_disc_svc_by_uuid(connection, &service.u, on_service, nullptr);
        }) ||
        !st.found)
        return -1;
    return st.cache.add_service(uuid, {st.found_start, st.found_end});
}

[[nodiscard]] BleError resolve_once(const BleRequest& request, Target& target) {
    const auto connection = active_link();
    if (request.has_arg1) {
        // The declaration sits directly before the value: properties, value handle, UUID.
        const auto handle = static_cast<std::uint16_t>(request.arg1);
        std::uint8_t declaration[24];
        const auto saved = std::pair{st.out, st.out_capacity};
        st.out = declaration;
        st.out_capacity = sizeof(declaration);
        st.out_used = 0;
        st.out_overflow = false;
        const bool read = handle > 1 && procedure([&] {
            return ble_gattc_read_long(connection, handle - 1, 0, on_read, nullptr);
        });
        const auto used = st.out_used;
        st.out = saved.first;
        st.out_capacity = saved.second;
        st.out_used = 0;
        if (!read || used < 5 || st.out_overflow ||
            (declaration[1] | (declaration[2] << 8)) != handle)
            return BleError::no_characteristic;
        target.value_handle = handle;
        target.range_end =
            static_cast<std::uint16_t>(std::min<unsigned>(0xFFFF, handle + kDescriptorSearch));
        target.properties = declaration[0];
        target.uuid = used == 5 ? static_cast<std::uint16_t>(declaration[3] | (declaration[4] << 8))
                                : static_cast<std::uint16_t>(
                                      declaration[3 + 12] | (declaration[3 + 13] << 8));
        return BleError::none;
    }
    const int index = find_service(request.service);
    if (index < 0)
        return BleError::no_service;
    const auto range = st.cache.services[index].range;
    target.range_end = range.end;
    target.uuid = short_uuid(request.characteristic);
    if (const auto* cached = st.cache.find_characteristic(index, request.characteristic)) {
        target.value_handle = cached->value_handle;
        target.properties = cached->properties;
        return BleError::none;
    }
    const auto characteristic = make_uuid(request.characteristic);
    if (!procedure([&] {
            return ble_gattc_disc_chrs_by_uuid(
                connection, range.start, range.end, &characteristic.u, on_characteristic, nullptr);
        }) ||
        !st.found)
        return BleError::no_characteristic;
    target.value_handle = st.found_handle;
    target.properties = st.found_properties;
    st.cache.add_characteristic(
        index, request.characteristic, {target.value_handle, target.properties});
    return BleError::none;
}

/** @brief Resolves the staged characteristic with handles that held throughout the lookup.
 *  A change announced meanwhile makes the lookup, failed or not, start over on fresh discovery.
 */
[[nodiscard]] BleError resolve(const BleRequest& request, Target& target) {
    return cache::settled(
        [] { return st.cache_generation.load(); }, [&] { return resolve_once(request, target); });
}

[[nodiscard]] BleError subscribe(const BleRequest& request, const Target& target, bool enable) {
    const bool notify = (target.properties & BLE_GATT_CHR_PROP_NOTIFY) != 0;
    if (!notify && (target.properties & BLE_GATT_CHR_PROP_INDICATE) == 0)
        return BleError::cannot_notify;
    if (enable && st.subscription_count == kSubscriptionLimit)
        return BleError::cannot_notify;
    const auto connection = active_link();
    // Unsubscribing from the Service Changed we watch only ends delivery to the script.
    const bool watch_keeps =
        !enable && st.watched == cache::watch_key(connection, target.value_handle);
    if (!watch_keeps) {
        // NimBLE takes the value handle and itself starts the search one handle later.
        const auto value_handle = target.value_handle;
        const auto last =
            std::min<unsigned>(target.range_end, target.value_handle + kDescriptorSearch);
        if (!procedure([&] {
                return ble_gattc_disc_all_dscs(connection, value_handle,
                    static_cast<std::uint16_t>(last), on_descriptor, nullptr);
            }) ||
            !st.found)
            return BleError::cannot_notify;
        const std::uint8_t value[2] = {static_cast<std::uint8_t>(enable ? (notify ? 1 : 2) : 0), 0};
        const auto cccd = st.cccd_handle;
        st.notified = false; // before the write: the first notification may follow at once
        const bool written = request.confirm ? procedure([&] {
            return ble_gattc_write_flat(connection, cccd, value, sizeof(value), on_write, nullptr);
        })
                                             : ble_gattc_write_no_rsp_flat(
                                                   connection, cccd, value, sizeof(value)) == 0;
        if (!written)
            return BleError::cannot_notify;
    }
    std::uint8_t kept = 0;
    for (std::uint8_t i = 0; i < st.subscription_count; ++i)
        if (st.subscriptions[i].handle != target.value_handle)
            st.subscriptions[kept++] = st.subscriptions[i];
    st.subscription_count = kept;
    if (enable)
        st.subscriptions[st.subscription_count++] = {target.value_handle, target.uuid};
    return BleError::none;
}

[[nodiscard]] BleError write(const BleRequest& request, const Target& target) {
    const bool with_response = (target.properties & BLE_GATT_CHR_PROP_WRITE) != 0;
    const bool without_response = (target.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
    if (!with_response && !without_response)
        return BleError::cannot_write;
    const auto connection = active_link();
    // Legacy rule: a requested confirmation needs a confirmable characteristic; otherwise
    // confirm only when write-without-response is unavailable.
    const bool confirmed = request.confirm ? with_response : !without_response;
    if (!confirmed)
        return ble_gattc_write_no_rsp_flat(
                   connection, target.value_handle, request.data, request.length) == 0
                   ? BleError::none
                   : BleError::write_failed;
    const bool long_write = request.length + 3u > ble_att_mtu(connection);
    return procedure([&] {
        if (!long_write)
            return ble_gattc_write_flat(
                connection, target.value_handle, request.data, request.length, on_write, nullptr);
        os_mbuf* om = ble_hs_mbuf_from_flat(request.data, request.length);
        return om == nullptr ? BLE_HS_ENOMEM
                             : ble_gattc_write_long(
                                   connection, target.value_handle, 0, om, on_write, nullptr);
    })
               ? BleError::none
               : BleError::write_failed;
}

/** @brief Op 6: count, then bits and UUID bytes per service. */
BleError list_services() {
    const auto connection = active_link();
    st.count = 0;
    st.out_used = 1;
    if (!procedure(
            [&] { return ble_gattc_disc_all_svcs(connection, on_service_listing, nullptr); }))
        return BleError::no_service;
    st.out[0] = st.count;
    return BleError::none;
}

/** @brief Lists characteristics once, discarding any reply from an earlier attempt. */
BleError list_characteristics_once(const BleRequest& request) {
    st.count = 0;
    st.out_used = 1;
    st.out_overflow = false;
    const auto connection = active_link();
    const int index = find_service(request.service);
    if (index < 0)
        return BleError::no_service;
    const auto range = st.cache.services[index].range;
    if (!procedure([&] {
            return ble_gattc_disc_all_chrs(
                connection, range.start, range.end, on_characteristic_listing, nullptr);
        }))
        return BleError::no_characteristic;
    st.out[0] = st.count;
    return BleError::none;
}

/** @brief Op 7: count, then bits, UUID bytes, properties and handle per characteristic.
 *  A database change during discovery retries with a fresh service range and reply.
 */
BleError list_characteristics(const BleRequest& request) {
    return cache::settled([] { return st.cache_generation.load(); },
        [&] { return list_characteristics_once(request); });
}

/** @brief Ops 1 to 4 on a resolved characteristic. */
BleError access(const BleRequest& request, const Target& target) {
    const auto connection = active_link();
    switch (request.operation) {
    case 1:
        if ((target.properties & BLE_GATT_CHR_PROP_READ) == 0)
            return BleError::cannot_read;
        return procedure([&] {
            return ble_gattc_read_long(connection, target.value_handle, 0, on_read, nullptr);
        })
                   ? BleError::none
                   : BleError::cannot_read;
    case 2:
        return write(request, target);
    default: {
        const auto error = subscribe(request, target, request.operation == 3);
        if (error == BleError::none) {
            append(static_cast<std::uint8_t>(target.value_handle >> 8)); // legacy: high byte first
            append(static_cast<std::uint8_t>(target.value_handle & 0xFF));
        }
        return error;
    }
    }
}

// ---- GATT server and advertising

[[nodiscard]] Server* ensure_server() {
    if (st.server == nullptr) {
        st.server = static_cast<Server*>(std::calloc(1, sizeof(Server)));
        if (st.server != nullptr)
            st.server->connection = BLE_HS_CONN_HANDLE_NONE;
    }
    return st.server;
}

[[nodiscard]] ServerCharacteristic* characteristic_by_handle(std::uint16_t handle) {
    if (st.server == nullptr)
        return nullptr;
    for (std::uint8_t i = 0; i < st.server->characteristic_count; ++i)
        if (st.server->characteristics[i].value_handle == handle)
            return &st.server->characteristics[i];
    return nullptr;
}

[[nodiscard]] bool set_value(
    ServerCharacteristic& c, const std::uint8_t* data, std::uint8_t length) {
    std::uint8_t* fresh = nullptr;
    std::uint8_t* old = nullptr;
    if (length > c.capacity) {
        fresh = static_cast<std::uint8_t*>(std::malloc(length));
        if (fresh == nullptr)
            return false;
    }
    portENTER_CRITICAL(&value_lock);
    if (fresh != nullptr) {
        old = c.value;
        c.value = fresh;
        c.capacity = length;
    }
    if (length != 0)
        std::memcpy(c.value, data, length);
    c.length = length;
    portEXIT_CRITICAL(&value_lock);
    std::free(old);
    return true;
}

int on_access(uint16_t, uint16_t, ble_gatt_access_ctxt* context, void* argument) {
    auto& c = st.server->characteristics[reinterpret_cast<std::uintptr_t>(argument)];
    const auto uuid = short_uuid(c.uuid_bytes);
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        std::uint8_t copy[255];
        std::uint8_t length;
        portENTER_CRITICAL(&value_lock);
        length = c.length;
        if (length != 0)
            std::memcpy(copy, c.value, length);
        portEXIT_CRITICAL(&value_lock);
        if (os_mbuf_append(context->om, copy, length) != 0)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        post(221, uuid, c.value_handle, nullptr, 0);
        return 0;
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const std::size_t length = OS_MBUF_PKTLEN(context->om);
        if (length > 255)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        std::uint8_t data[255];
        if (ble_hs_mbuf_to_flat(context->om, data, 255, nullptr) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        if (!set_value(c, data, static_cast<std::uint8_t>(length)))
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        post(222, uuid, c.value_handle, data, length);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

void advertise();

int on_server_event(ble_gap_event* event, void*) {
    Server* server = st.server;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        server->connection = event->connect.conn_handle;
        link_changed();
        ble_detail::scan_links(1);
        ble_gap_conn_desc desc;
        if (ble_gap_conn_find(server->connection, &desc) == 0)
            post(227, 0, 0, desc.peer_ota_addr.val, 6);
        if (server->itvl_min != 0 && server->itvl_max != 0) {
            ble_gap_upd_params parameters{};
            parameters.itvl_min = server->itvl_min >> 1; // 0.625 ms units to 1.25 ms units
            parameters.itvl_max = server->itvl_max >> 1;
            parameters.supervision_timeout = 400;
            (void)ble_gap_update_params(server->connection, &parameters);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        post(228, 0, 0, nullptr, 0);
        server->connection = BLE_HS_CONN_HANDLE_NONE;
        link_changed();
        st.subscription_count = 0;
        ble_detail::scan_links(-1);
        finish(BLE_HS_ENOTCONN);
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        const auto* c = characteristic_by_handle(event->subscribe.attr_handle);
        post(static_cast<std::uint8_t>(223 + (event->subscribe.cur_notify ? 1 : 0) +
                                       (event->subscribe.cur_indicate ? 2 : 0)),
            c != nullptr ? short_uuid(c->uuid_bytes) : 0, event->subscribe.attr_handle, nullptr, 0);
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_TX: {
        const auto* c = characteristic_by_handle(event->notify_tx.attr_handle);
        const int code = event->notify_tx.status;
        post(229, c != nullptr ? short_uuid(c->uuid_bytes) : 0, event->notify_tx.attr_handle, &code,
            sizeof(code));
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (st.awaiting_security)
            finish(event->enc_change.status);
        if (event->enc_change.status == 0)
            post_authenticated(event->enc_change.conn_handle);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return common_event(event);
    }
}

void advertise() {
    Server* server = st.server;
    if (server == nullptr || !server->advertising_enabled || ble_gap_adv_active())
        return;
    ble_gap_adv_params parameters{};
    parameters.conn_mode = server->adv_mode == 0 ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = server->itvl_min;
    parameters.itvl_max = server->itvl_max;
    (void)ble_gap_adv_start(ble_detail::own_address_type, nullptr, BLE_HS_FOREVER, &parameters,
        on_server_event, nullptr);
}

/** @brief Builds and starts the GATT database from the characteristics defined so far. */
[[nodiscard]] bool register_services() {
    Server& server = *st.server;
    if (server.service_count == 0) {
        server.registered = true; // a pure advertiser needs no database
        return true;
    }
    ble_detail::scan_hold(true); // the host refuses a database reset while scanning
    std::size_t next = 0;
    for (std::uint8_t s = 0; s < server.service_count; ++s) {
        auto& definition = server.service_defs[s];
        definition = {};
        definition.type = BLE_GATT_SVC_TYPE_PRIMARY;
        definition.uuid = &server.services[s].uuid.u;
        definition.characteristics = &server.characteristic_defs[next];
        for (std::uint8_t c = 0; c < server.characteristic_count; ++c) {
            auto& source = server.characteristics[c];
            if (source.service != s)
                continue;
            auto& target = server.characteristic_defs[next++];
            target = {};
            target.uuid = &source.uuid.u;
            target.access_cb = on_access;
            target.arg = reinterpret_cast<void*>(static_cast<std::uintptr_t>(c));
            target.flags = source.flags;
            target.val_handle = &source.value_handle;
        }
        server.characteristic_defs[next++] = {};
    }
    server.service_defs[server.service_count] = {};
    bool ok = ble_gatts_reset() == 0;
    if (ok) {
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ok = ble_gatts_count_cfg(server.service_defs) == 0 &&
             ble_gatts_add_svcs(server.service_defs) == 0 && ble_gatts_start() == 0;
    }
    ble_detail::scan_hold(false);
    server.registered = ok;
    return ok;
}

/** @brief Ops 201 and 202: set the advertisement or scan response; the first 201 builds the server.
 */
BleError server_advertisement(const BleRequest& request) {
    Server* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    const bool response = request.operation == 202;
    if (request.length > 31)
        return BleError::buffer_too_small;
    const bool first = !response && !server->registered;
    if (first && !register_services())
        return BleError::no_service;
    if (request.has_arg1)
        server->adv_mode = request.arg1 == 0 ? 0 : 2;
    // The reply shares the request's buffer: use the data before writing anything back.
    const int result = response ? ble_gap_adv_rsp_set_data(request.data, request.length)
                                : ble_gap_adv_set_data(request.data, request.length);
    if (result != 0)
        return BleError::cannot_write;
    if (!response) {
        server->advertising_enabled = true;
        advertise();
    }
    if (first) // legacy: the handles of all characteristics, high byte first
        for (std::uint8_t c = 0; c < server->characteristic_count; ++c) {
            append(static_cast<std::uint8_t>(server->characteristics[c].value_handle >> 8));
            append(static_cast<std::uint8_t>(server->characteristics[c].value_handle & 0xFF));
        }
    return BleError::none;
}

/** @brief Op 211: define a service and characteristic before the server starts, or set its value.
 */
BleError server_characteristic(const BleRequest& request, BleEvent& event) {
    Server* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    std::uint8_t service = server->service_count;
    for (std::uint8_t s = 0; s < server->service_count; ++s)
        if (std::memcmp(server->services[s].uuid_bytes, request.service, 16) == 0)
            service = s;
    if (service == server->service_count) {
        if (server->registered || service == kServiceLimit)
            return BleError::no_service;
        std::memcpy(server->services[service].uuid_bytes, request.service, 16);
        server->services[service].uuid = make_uuid(request.service);
        ++server->service_count;
    }
    ServerCharacteristic* found = nullptr;
    for (std::uint8_t c = 0; c < server->characteristic_count; ++c)
        if (server->characteristics[c].service == service &&
            std::memcmp(server->characteristics[c].uuid_bytes, request.characteristic, 16) == 0)
            found = &server->characteristics[c];
    if (found == nullptr) {
        if (server->registered || server->characteristic_count == kCharacteristicLimit)
            return BleError::no_characteristic;
        found = &server->characteristics[server->characteristic_count++];
        std::memcpy(found->uuid_bytes, request.characteristic, 16);
        found->uuid = make_uuid(request.characteristic);
        found->service = service;
        // Default to "all", as the legacy driver does; arg1 replaces the flag set.
        found->flags =
            request.has_arg1
                ? static_cast<std::uint16_t>(request.arg1)
                : static_cast<std::uint16_t>(
                      BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE |
                      (request.confirm ? BLE_GATT_CHR_F_WRITE : BLE_GATT_CHR_F_WRITE_NO_RSP));
    }
    if (request.length > 255 ||
        !set_value(*found, request.data, static_cast<std::uint8_t>(request.length)))
        return BleError::buffer_too_small;
    if (server->registered && found->value_handle != 0)
        ble_gatts_chr_updated(found->value_handle); // notifies or indicates per subscription
    event.uuid = short_uuid(found->uuid_bytes);
    event.handle = found->value_handle;
    return BleError::none;
}

// ---- The worker

void run(const BleRequest& request) {
    BleEvent event{static_cast<std::uint16_t>(BleError::none), request.operation, 0, 0, 0};
    reply_begin(request);
    st.pin = request.pin;
    auto error = BleError::none;
    if (request.operation == 201 || request.operation == 202) {
        error = server_advertisement(request);
    } else if (request.operation == 211) {
        error = server_characteristic(request, event);
    } else if (request.operation == 5) {
        disconnect_outbound();
        if (st.server != nullptr && st.server->connection != BLE_HS_CONN_HANDLE_NONE)
            terminate(st.server->connection);
    } else if (!connect(request)) {
        error = BleError::no_connect;
    } else {
        Target target{};
        if (request.operation == 6) {
            error = list_services();
        } else if (request.operation == 7) {
            error = list_characteristics(request);
        } else {
            error = resolve(request, target);
            if (error == BleError::none) {
                event.uuid = target.uuid;
                event.handle = target.value_handle;
                error = access(request, target);
            }
        }
        if (error != BleError::none)
            cache_forget(true); // a changed peer database must not fail the next job too
        if (error == BleError::none && request.one_shot && request.operation == 3) {
            for (std::uint32_t waited = 0; !st.notified && waited < kNotifyWaitMs; waited += 100) {
                if (active_link() == BLE_HS_CONN_HANDLE_NONE)
                    break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!st.notified)
                error = BleError::notify_timeout;
        }
        if (error != BleError::none && active_link() == BLE_HS_CONN_HANDLE_NONE)
            error = BleError::disconnected;
        // With a server present the accepted link and the outbound link stay up.
        if (request.one_shot && st.server == nullptr)
            disconnect_outbound();
    }
    if (error == BleError::none && st.out_overflow)
        error = BleError::buffer_too_small;
    event.error = static_cast<std::uint16_t>(error);
    event.length = error == BleError::none ? static_cast<std::uint16_t>(st.out_used) : 0;
    st.completion = event;
    st.last_operation = event.operation;
    st.last_error = event.error;
}

void worker(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (st.quit)
            break;
        run(st.request);
        st.completed = true;
        st.running = false;
    }
    st.running = true; // our own disconnects are not unsolicited events
    (void)ble_gap_adv_stop();
    disconnect_outbound();
    if (st.server != nullptr) {
        st.server->advertising_enabled = false;
        if (st.server->connection != BLE_HS_CONN_HANDLE_NONE)
            terminate(st.server->connection);
    }
    RingbufHandle_t queue = st.queue;
    st.queue = nullptr;
    if (queue != nullptr)
        vRingbufferDelete(queue);
    st.unsolicited = -1;
    st.completed = false;
    st.queued = 0;
    st.running = false;
    st.quit = false;
    st.task = nullptr;
    vTaskDelete(nullptr);
}
} // namespace

namespace ble_detail {
void connection_init() {
    if (st.signal == nullptr)
        st.signal = xSemaphoreCreateBinaryStatic(&st.signal_storage);
    st.connection = BLE_HS_CONN_HANDLE_NONE;
    st.unsolicited = -1;
}

bool connection_shutdown() {
    ble_connection_close();
    // The worker finishes its job and closes its links through the host first.
    const TickType_t since = xTaskGetTickCount();
    while (st.task != nullptr) {
        if (xTaskGetTickCount() - since > kShutdownTimeout)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // The host forgets the GATT database; a server is built again on demand.
    if (st.server != nullptr)
        for (auto& c : st.server->characteristics)
            std::free(c.value);
    std::free(st.server);
    st.server = nullptr;
    st.subscription_count = 0;
    return true;
}

void connection_fill(BleInfo& info) {
    if (st.connection != BLE_HS_CONN_HANDLE_NONE)
        info.roles |= 2;
    if (st.server != nullptr) {
        info.roles |= 4;
        if (ble_gap_adv_active())
            info.roles |= 8;
    }
    info.ready = st.task != nullptr && !st.quit && !st.running && !st.completed;
    info.pending = st.running || st.completed || st.queued > 0 || st.unsolicited >= 0;
    info.last_operation = st.last_operation;
    info.last_error = st.last_error;
    info.connection_stack_free =
        st.task == nullptr ? 0 : static_cast<std::uint16_t>(uxTaskGetStackHighWaterMark(st.task));
    const auto link = active_link();
    ble_gap_conn_desc desc;
    if (link == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(link, &desc) != 0)
        return;
    auto& connection = info.connection;
    connection.present = true;
    reverse_address(desc.peer_ota_addr.val, connection.peer_address);
    reverse_address(desc.peer_id_addr.val, connection.peer_id_address);
    connection.bonded = desc.sec_state.bonded;
    connection.encrypted = desc.sec_state.encrypted;
    connection.authenticated = desc.sec_state.authenticated;
    connection.master = desc.role == BLE_GAP_ROLE_MASTER;
    connection.mtu = ble_att_mtu(link);
    connection.has_rssi = ble_gap_conn_rssi(link, &connection.rssi) == 0;
    ble_store_key_sec key{};
    key.peer_addr = desc.peer_id_addr;
    ble_store_value_sec peer{};
    if (ble_store_read_peer_sec(&key, &peer) == 0 && peer.irk_present) {
        connection.has_irk = true;
        std::memcpy(connection.irk, peer.irk, 16);
    }
}
} // namespace ble_detail

bool ble_connection_open() {
    if (!ble_ready())
        return false;
    if (st.task != nullptr)
        return !st.quit;
    st.queue = xRingbufferCreate(kQueueBytes, RINGBUF_TYPE_NOSPLIT);
    if (st.queue == nullptr)
        return false;
    if (xTaskCreate(worker, "ble_conn", kWorkerStackBytes, nullptr, 2, &st.task) != pdPASS) {
        vRingbufferDelete(st.queue);
        st.queue = nullptr;
        st.task = nullptr;
        return false;
    }
    return true;
}

void ble_connection_close() {
    if (st.task == nullptr)
        return;
    st.quit = true;
    xTaskNotifyGive(st.task);
}

bool ble_connection_busy() {
    return st.running || st.completed;
}

bool ble_connection_request(const BleRequest& request) {
    if (st.task == nullptr || st.quit || ble_connection_busy())
        return false;
    st.request = request;
    st.running = true;
    xTaskNotifyGive(st.task);
    return true;
}

bool ble_connection_pending() {
    return st.completed || (!st.running && (st.unsolicited >= 0 || st.queued > 0));
}

bool ble_connection_poll(BleEvent& event, std::uint8_t* payload, std::size_t capacity) {
    if (st.completed) {
        event = st.completion;
        st.completed = false;
        return true;
    }
    if (st.running)
        return false;
    const int reason = st.unsolicited.exchange(-1);
    if (reason >= 0) {
        event = {static_cast<std::uint16_t>(reason), 5, 0, 0, 0};
        return true;
    }
    if (st.queue == nullptr || st.queued == 0)
        return false;
    std::size_t size = 0;
    auto* item = static_cast<EventItem*>(xRingbufferReceive(st.queue, &size, 0));
    if (item == nullptr)
        return false;
    --st.queued;
    event = {static_cast<std::uint16_t>(BleError::none), item->operation, item->uuid, item->handle,
        item->length};
    if (item->length > capacity) {
        event.error = static_cast<std::uint16_t>(BleError::buffer_too_small);
        event.length = 0;
    } else {
        std::memcpy(payload, item->data, item->length);
    }
    vRingbufferReturnItem(st.queue, item);
    return true;
}

std::uint32_t ble_connection_dropped() {
    return st.dropped;
}

bool ble_configure(const BleRequest& request) {
    if (!ble_ready())
        return false;
    switch (request.operation) {
    case 231: // own address
        (void)ble_gap_adv_stop();
        if (request.address_type > 0) {
            std::uint8_t address[6];
            ble_detail::reverse_address(request.address, address);
            if (ble_hs_id_set_rnd(address) != 0)
                return false;
        }
        ble_detail::own_address_type = request.address_type;
        return true;
    case 232: { // advertising parameters: mode, minimum and maximum interval
        if (request.length != 5)
            return false;
        Server* server = ensure_server();
        if (server == nullptr)
            return false;
        server->adv_mode = request.data[0] == 0 ? 0 : 2;
        server->itvl_min = static_cast<std::uint16_t>(request.data[1] | (request.data[2] << 8));
        server->itvl_max = static_cast<std::uint16_t>(request.data[3] | (request.data[4] << 8));
        return true;
    }
    case 233: { // device name
        char name[MYNEWT_VAL(BLE_SVC_GAP_DEVICE_NAME_MAX_LENGTH) + 1]{};
        if (request.length == 0 || request.length >= sizeof(name))
            return false;
        std::memcpy(name, request.data, request.length);
        return ble_svc_gap_device_name_set(name) == 0;
    }
    default:
        return false;
    }
}
} // namespace newmota::platform
