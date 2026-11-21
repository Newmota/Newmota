/*
  ble.cpp - Bouffalo native blestack BLE adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble.cpp
 * @brief Bounded legacy BLE scan, GATT client/server and RAM bonds for BL602/BL616.
 *
 * The adapter admits one link total, in either role, independently of the
 * selected controller's native connection pools. SDK callbacks
 * capture owned data; only the cooperative caller delivers events to Berry.
 * A job completion has its own slot, independent of advertisements and events.
 * Security records belong to this pinned blestack ABI; EasyFlash stays disabled.
 */
#include "newmota/platform/ble.h"
#include "newmota/platform/ble_cache.h"
#include "radio.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "semphr.h"
#include "queue.h"
#include "bluetooth.h"
#include "conn.h"
#include "gatt.h"
#include "hci_core.h"
#include "hci_driver.h"
#if defined(BL602)
#include "ble_lib_api.h"
#elif defined(BL616)
#include "btble_lib_api.h"
#else
#error "Bouffalo BLE requires BL602 or BL616"
#endif
#include "keys.h"
}

// The SDK applies its archive preset after Kconfig; CONFIG_CON retains the
// effective pool count even though blestack normalizes CONFIG_BT_CONN to 1.
#if defined(BL616)
#if CONFIG_CON != 10 || CONFIG_EM_SIZE != 32 || CONFIG_BLE_TX_BUFF_DATA != 10 ||                   \
    CONFIG_BLE_ACT_MAX != 12 || !defined(CONFIG_BT_CENTRAL) || !defined(CONFIG_BT_PERIPHERAL) ||   \
    !defined(CONFIG_BT_OBSERVER) || !defined(CONFIG_BT_BROADCASTER) || defined(CONFIG_BT_BREDR) || \
    defined(CONFIG_ADV_EXTENSION)
#error "BL616 BLE requires the SDK BLE-only ten-slot preset with 32 KiB exchange RAM"
#endif
#endif

/** @brief The SDK BLE assertion path must stop rather than resume invalid state. */
extern "C" void vAssertCalled(void) {
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

namespace newmota::platform {
namespace {
constexpr std::size_t kServiceLimit = 4;
constexpr std::size_t kCharacteristicLimit = 12;
constexpr std::size_t kSubscriptionLimit = 8;
constexpr std::size_t kWatchLimit = 8;
constexpr TickType_t kProcedureTimeout = pdMS_TO_TICKS(5000);
constexpr std::uint8_t kBaseUuid[12] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};

/** @brief Native UUID storage; the common header is the first member of each variant. */
union Uuid {
    bt_uuid uuid;
    bt_uuid_16 u16;
    bt_uuid_32 u32;
    bt_uuid_128 u128;
};

Uuid make_uuid(const std::uint8_t* bytes) {
    Uuid uuid{};
    if (std::memcmp(bytes, kBaseUuid, 12) == 0) {
        if (bytes[14] == 0 && bytes[15] == 0) {
            uuid.u16.uuid.type = BT_UUID_TYPE_16;
            uuid.u16.val = static_cast<std::uint16_t>(bytes[12] | (bytes[13] << 8));
        } else {
            uuid.u32.uuid.type = BT_UUID_TYPE_32;
            std::memcpy(&uuid.u32.val, bytes + 12, 4);
        }
    } else {
        uuid.u128.uuid.type = BT_UUID_TYPE_128;
        std::memcpy(uuid.u128.val, bytes, 16);
    }
    return uuid;
}

std::uint16_t short_uuid(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[12] | (bytes[13] << 8));
}

void reverse_address(const std::uint8_t* from, std::uint8_t* to) {
    for (unsigned i = 0; i < 6; ++i)
        to[i] = from[5 - i];
}

namespace cache = ble_cache;

/** @brief What a cached characteristic needs to skip its discovery. */
struct CachedCharacteristic {
    std::uint16_t handle;
    std::uint8_t properties;
};

/** @brief Guards small copied snapshots, never SDK calls or waits. */
struct Guard {
    Guard() { taskENTER_CRITICAL(); }

    ~Guard() { taskEXIT_CRITICAL(); }
};

struct EventItem {
    BleEvent event;
    std::uint8_t data[255];
};

struct Subscription {
    std::uint16_t handle, uuid;
    bool used;
};

struct Characteristic {
    Uuid uuid;
    std::uint8_t bytes[16];
    std::uint8_t service;
    bt_gatt_chrc declaration;
    _bt_gatt_ccc ccc;
    bt_gatt_attr* value_attr;
    std::uint8_t value[255];
    std::uint8_t length;
    bt_gatt_indicate_params indication;
    std::uint8_t indicated_value[255];
    bool indicating;
    std::uint16_t notifying;
};

struct Service {
    Uuid uuid;
    std::uint8_t bytes[16];
    bt_gatt_service native;
};

/** @brief Allocated only for server use; database and callback storage live until reboot. */
struct Server {
    Service services[kServiceLimit];
    Characteristic characteristics[kCharacteristicLimit];
    bt_gatt_attr attributes[kServiceLimit + 3 * kCharacteristicLimit];
    std::uint8_t service_count, characteristic_count;
    bool registered, enabled, advertising;
    std::uint8_t mode;
    std::uint16_t interval_min, interval_max;
    std::uint8_t ad[31], response[31], ad_length, response_length;
};

struct State {
    std::atomic<bool> started{}, failed{}, ready{}, opened{}, running{}, completed{},
        close_requested{};
    std::atomic<bool> draining{}, connecting{};
    std::atomic<bool> scan_running{}, scan_transition{}, hold{}, linked{}, notified{}, securing{};
    std::atomic<bool> scan_dirty{}; ///< Parameters changed; restart a running scan.
    std::atomic<unsigned> consumers{};
    std::atomic<std::uint32_t> scan_dropped{}, event_dropped{};
    std::atomic<std::uint32_t> cache_generation{}; ///< Advanced when cached handles may be stale.
    std::atomic<std::uint16_t> service_changed{};  ///< Its handle while indicated to us, or 0.
    TaskHandle_t worker{}, scanner{};
    SemaphoreHandle_t signal{}, control{};
    QueueHandle_t events{};
    BleRequest request{};
    BleEvent completion{};
    // Protected by Guard. The worker takes a separate reference before SDK operations.
    bt_conn* connection{};
    bool master{};
    BleConnectionInfo connection_info{};
    BleAdvertisement advertisement{};
    bool advertisement_present{}, advertisement_waiting{};
    TickType_t advertisement_time{};
    bt_addr_le_t watched[kWatchLimit]{};
    std::uint8_t watch_count{};
    BleScanParameters scan_parameters{kBleScanDefaults};
    Server* server{};
    Subscription subscriptions[kSubscriptionLimit]{};
    std::uint32_t pin{};
    std::uint8_t last_operation{};
    std::uint16_t last_error{};
    int status{};
    // One worker and one in-flight procedure own all of the following storage.
    bt_gatt_discover_params discovery{};
    bt_gatt_read_params read{};
    bt_gatt_write_params write{};
    bt_gatt_exchange_params exchange{};
    Uuid filter{};
    bool found{}, overflow{}, listing{}, watching{};
    cache::Cache<CachedCharacteristic> cache{};
    std::uint16_t found_start{}, found_end{}, found_handle{}, found_ccc{};
    std::uint8_t properties{}, count{};
    std::uint8_t* output{};
    std::size_t used{}, capacity{};
};

State st;
constexpr bt_addr_le_t kAnyAddress{};

/** @brief Serializes synchronous configuration against worker-side radio control. */
struct ControlGuard {
    bool locked;

    explicit ControlGuard(TickType_t timeout = 0)
        : locked(st.control != nullptr && xSemaphoreTake(st.control, timeout) == pdTRUE) {}

    ~ControlGuard() {
        if (locked)
            xSemaphoreGive(st.control);
    }
};

bt_conn_cb connection_callbacks{};
bt_conn_auth_cb authentication_callbacks{};

void post(std::uint8_t operation, std::uint16_t uuid = 0, std::uint16_t handle = 0,
    const void* data = nullptr, std::size_t length = 0) {
    if (!st.opened)
        return;
    if (length > 255 || st.events == nullptr) {
        ++st.event_dropped;
        return;
    }
    EventItem item{{0, operation, uuid, handle, static_cast<std::uint16_t>(length)}, {}};
    if (length != 0)
        std::memcpy(item.data, data, length);
    if (xQueueSend(st.events, &item, 0) != pdTRUE)
        ++st.event_dropped;
}

void finish(int status) {
    st.status = status;
    xSemaphoreGive(st.signal);
}

void begin() {
    (void)xSemaphoreTake(st.signal, 0);
    st.found = false;
    st.found_ccc = 0;
}

int await(TickType_t timeout = kProcedureTimeout) {
    return xSemaphoreTake(st.signal, timeout) == pdTRUE ? st.status : -1;
}

void append(const void* data, std::size_t length) {
    if (length > st.capacity - std::min(st.used, st.capacity)) {
        st.overflow = true;
        return;
    }
    if (length != 0)
        std::memcpy(st.output + st.used, data, length);
    st.used += length;
}

void append(std::uint8_t byte) {
    append(&byte, 1);
}

void append_uuid(const bt_uuid* uuid) {
    switch (uuid->type) {
    case BT_UUID_TYPE_16:
        append(16);
        append(&reinterpret_cast<const bt_uuid_16*>(uuid)->val, 2);
        break;
    case BT_UUID_TYPE_32:
        append(32);
        append(&reinterpret_cast<const bt_uuid_32*>(uuid)->val, 4);
        break;
    default:
        append(128);
        append(reinterpret_cast<const bt_uuid_128*>(uuid)->val, 16);
        break;
    }
}

/** @brief Takes a reference without exposing the callback-owned reference to the worker. */
bt_conn* connection_ref() {
    Guard guard;
    return st.connection == nullptr ? nullptr : bt_conn_ref(st.connection);
}

/** @brief A link came or went: nothing learned about the previous peer's database still holds. */
void link_changed() {
    st.service_changed = 0;
    ++st.cache_generation;
}

/** @brief Same-SDK bond envelope rejects records from other hosts or ABI configurations. */
struct BondRecord {
    std::uint8_t magic[4], revision[40], options, id, local_address[6];
    std::uint8_t address_type, address[6], enc_size, flags, key_mask[2];
    std::uint8_t ltk_rand[8], ltk_ediv[2], ltk[16], irk[16];
    std::uint8_t slave_rand[8], slave_ediv[2], slave_ltk[16];
    std::uint8_t local_csrk[16], local_counter[4], remote_csrk[16], remote_counter[4];
};

static_assert(sizeof(BondRecord) <= 255, "bond record must fit an event");

constexpr std::uint8_t bond_options() {
    std::uint8_t options = 0;
#if defined(CONFIG_BT_SIGNING)
    options |= 1;
#endif
#if !defined(CONFIG_BT_SMP_SC_PAIR_ONLY)
    options |= 2;
#endif
#if defined(BL616)
    // Bind BL616's ten-slot BLE-only controller policy separately from BL602.
    options |= 4;
#endif
    return options;
}

void pairing_complete(bt_conn* conn, bool bonded) {
    {
        Guard guard;
        if (conn != st.connection)
            return;
    }
    if (!bonded)
        return;
    auto* keys = bt_keys_find_addr(BT_ID_DEFAULT, bt_conn_get_dst(conn));
    if (keys == nullptr)
        return;
    BondRecord record{};
    std::memcpy(record.magic, "BLK1", 4);
    static_assert(sizeof(NEWMOTA_SDK_REVISION) == 41, "bond records require the full SDK revision");
    std::memcpy(record.revision, NEWMOTA_SDK_REVISION, sizeof(record.revision));
    record.options = bond_options();
    record.id = keys->id;
    bt_addr_le_t local{};
    if (bt_get_local_public_address(&local) != 0)
        return;
    std::memcpy(record.local_address, local.a.val, 6);
    record.address_type = keys->addr.type;
    std::memcpy(record.address, keys->addr.a.val, 6);
    record.enc_size = keys->enc_size;
    record.flags = keys->flags & (BT_KEYS_AUTHENTICATED | BT_KEYS_SC);
    record.key_mask[0] = static_cast<std::uint8_t>(keys->keys);
    record.key_mask[1] = static_cast<std::uint8_t>(keys->keys >> 8);
    std::memcpy(record.ltk_rand, keys->ltk.rand, 8);
    std::memcpy(record.ltk_ediv, keys->ltk.ediv, 2);
    std::memcpy(record.ltk, keys->ltk.val, 16);
    std::memcpy(record.irk, keys->irk.val, 16);
#if !defined(CONFIG_BT_SMP_SC_PAIR_ONLY)
    std::memcpy(record.slave_rand, keys->slave_ltk.rand, 8);
    std::memcpy(record.slave_ediv, keys->slave_ltk.ediv, 2);
    std::memcpy(record.slave_ltk, keys->slave_ltk.val, 16);
#endif
#if defined(CONFIG_BT_SIGNING)
    std::memcpy(record.local_csrk, keys->local_csrk.val, 16);
    std::memcpy(record.local_counter, &keys->local_csrk.cnt, 4);
    std::memcpy(record.remote_csrk, keys->remote_csrk.val, 16);
    std::memcpy(record.remote_counter, &keys->remote_csrk.cnt, 4);
#endif
    {
        Guard guard;
        st.connection_info.bonded = true;
        st.connection_info.has_irk = (keys->keys & BT_KEYS_IRK) != 0;
        if (st.connection_info.has_irk)
            std::memcpy(st.connection_info.irk, keys->irk.val, 16);
    }
    post(230, 0, 0, &record, sizeof(record));
}

void passkey_entry(bt_conn* conn) {
    {
        Guard guard;
        if (conn != st.connection)
            return;
    }
    (void)bt_conn_auth_passkey_entry(conn, st.pin);
}

void pairing_cancel(bt_conn*) {}

void pairing_failed(bt_conn* conn, bt_security_err error) {
    {
        Guard guard;
        if (conn != st.connection)
            return;
    }
    if (st.securing)
        finish(static_cast<int>(error) + 1);
}

void security_changed(bt_conn* conn, bt_security_t level, bt_security_err error) {
    {
        Guard guard;
        if (conn != st.connection)
            return;
        st.connection_info.encrypted = level >= BT_SECURITY_L2;
        st.connection_info.authenticated = level >= BT_SECURITY_L3;
    }
    if (st.securing)
        finish(error == 0 ? 0 : static_cast<int>(error) + 1);
}

void connected(bt_conn* conn, std::uint8_t error) {
    bool occupied;
    {
        Guard guard;
        occupied = st.connection != nullptr && st.connection != conn;
    }
    // BL616's controller supports more links than this adapter owns.
    // An unexpected second link must not replace or complete the active job.
    if (occupied) {
        if (error == 0)
            (void)bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }
    st.connecting = false;
    st.draining = false;
    if (error != 0) {
        if (st.server != nullptr)
            st.server->advertising = false;
        if (st.running)
            finish(error);
        return;
    }
    bt_conn_info native{};
    (void)bt_conn_get_info(conn, &native);
    const bool master = native.role == BT_CONN_ROLE_MASTER;
    {
        Guard guard;
        if (st.connection == nullptr)
            st.connection = bt_conn_ref(conn);
        st.master = master;
        st.connection_info = {};
        st.connection_info.present = true;
        st.connection_info.master = master;
        st.connection_info.mtu = bt_gatt_get_mtu(conn);
        if (auto* keys = bt_keys_find_addr(native.id, bt_conn_get_dst(conn))) {
            st.connection_info.bonded = keys->keys != 0;
            st.connection_info.has_irk = (keys->keys & BT_KEYS_IRK) != 0;
            if (st.connection_info.has_irk)
                std::memcpy(st.connection_info.irk, keys->irk.val, 16);
        }
        reverse_address(bt_conn_get_dst(conn)->a.val, st.connection_info.peer_address);
        reverse_address(bt_conn_get_dst(conn)->a.val, st.connection_info.peer_id_address);
        st.linked = true;
        if (st.server != nullptr)
            st.server->advertising = false;
    }
    link_changed();
    if (master && st.running)
        finish(0);
    else
        post(227, 0, 0, bt_conn_get_dst(conn)->a.val, 6);
}

void mtu_changed(bt_conn* conn, int mtu) {
    Guard guard;
    if (conn == st.connection)
        st.connection_info.mtu = static_cast<std::uint16_t>(mtu);
}

void identity_resolved(bt_conn* conn, const bt_addr_le_t*, const bt_addr_le_t* identity) {
    Guard guard;
    if (conn == st.connection)
        reverse_address(identity->a.val, st.connection_info.peer_id_address);
}

void disconnected(bt_conn* conn, std::uint8_t reason) {
    bool master;
    {
        Guard guard;
        if (conn != st.connection)
            return;
        master = st.master;
        if (st.connection == conn) {
            bt_conn_unref(st.connection);
            st.connection = nullptr;
        }
        st.connection_info = {};
        st.linked = false;
    }
    link_changed();
    if (!master)
        post(228);
    else if (!st.running && st.opened && st.events != nullptr) {
        EventItem item{{reason, 5, 0, 0, 0}, {}};
        if (xQueueSend(st.events, &item, 0) != pdTRUE)
            ++st.event_dropped;
    }
    st.draining = false;
    if (st.running)
        finish(-2);
}

/** @brief Captures one report and allows its matching scan response to append. */
void report(
    const bt_addr_le_t* address, std::int8_t rssi, std::uint8_t type, net_buf_simple* buffer) {
    Guard guard;
    if (st.consumers.load() == 0)
        return;
    bool watched = st.watch_count == 0;
    for (unsigned i = 0; i < st.watch_count; ++i)
        if (bt_addr_le_cmp(address, &st.watched[i]) == 0)
            watched = true;
    if (!watched)
        return;
    std::uint8_t display[6];
    reverse_address(address->a.val, display);
    if (type == BT_LE_ADV_SCAN_RSP && st.advertisement_present && st.advertisement_waiting &&
        st.advertisement.address_type == address->type &&
        std::memcmp(display, st.advertisement.address, 6) == 0) {
        const auto length = std::min<std::size_t>(buffer->len, 62 - st.advertisement.length);
        std::memcpy(st.advertisement.payload + st.advertisement.length, buffer->data, length);
        st.advertisement.length += static_cast<std::uint8_t>(length);
        st.advertisement_waiting = false;
        return;
    }
    if (st.advertisement_present) {
        ++st.scan_dropped;
        return;
    }
    // A response without its advertisement is still useful raw advertising data.
    std::memcpy(st.advertisement.address, display, 6);
    st.advertisement.address_type = address->type;
    st.advertisement.rssi = rssi;
    st.advertisement.length = static_cast<std::uint8_t>(std::min<std::size_t>(buffer->len, 31));
    std::memcpy(st.advertisement.payload, buffer->data, st.advertisement.length);
    st.advertisement_time = xTaskGetTickCount();
    st.advertisement_waiting =
        st.scan_parameters.active && (type == BT_LE_ADV_IND || type == BT_LE_ADV_SCAN_IND);
    st.advertisement_present = true;
}

void scan_task(void*) {
    for (;;) {
        st.scan_transition = true;
        const bool wanted = st.ready && st.consumers.load() != 0 && !st.hold && !st.linked &&
                            !st.connecting && !st.draining;
        if (wanted && !st.scan_running) {
            BleScanParameters scan;
            {
                Guard guard;
                scan = st.scan_parameters;
                st.scan_dirty = false;
            }
            bt_le_scan_param parameters{};
            parameters.type = scan.active ? BT_HCI_LE_SCAN_ACTIVE : BT_HCI_LE_SCAN_PASSIVE;
            parameters.filter_dup = BT_HCI_LE_SCAN_FILTER_DUP_DISABLE;
            parameters.interval = scan.interval;
            parameters.window = scan.window;
            if (bt_le_scan_start(&parameters, report) == 0)
                st.scan_running = true;
        } else if ((!wanted || st.scan_dirty) && st.scan_running) {
            if (bt_le_scan_stop() == 0)
                st.scan_running = false;
        }
        st.scan_transition = false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool hold_scan() {
    st.hold = true;
    for (unsigned i = 0; i < 100 && (st.scan_running || st.scan_transition); ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    return !st.scan_running && !st.scan_transition;
}

/** @brief Cancels the exact request before reusable callback storage can be released. */
template <typename Params, typename Start>
bool procedure(bt_conn* conn, Params& parameters, Start start) {
    const Params initial_parameters = parameters;
    const auto initial_used = st.used;
    const auto initial_count = st.count;
    const bool initial_overflow = st.overflow;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (attempt != 0) {
            parameters = initial_parameters;
            st.used = initial_used;
            st.count = initial_count;
            st.overflow = initial_overflow;
        }
        begin();
        if (start() != 0)
            return false;
        const int status = await();
        if (status == 0)
            return true;
        if (status < 0) {
            // Cancellation detaches this exact ATT request. Do not admit another
            // job while the link can still deliver its old disconnect callback.
            bt_gatt_cancel(conn, &parameters);
            st.draining = st.linked.load() || st.connecting.load();
            begin();
            if (bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN) == 0)
                (void)await();
            return false;
        }
        if (attempt != 0 || st.watching ||
            (status != BT_ATT_ERR_AUTHENTICATION && status != BT_ATT_ERR_AUTHORIZATION &&
                status != BT_ATT_ERR_INSUFFICIENT_ENCRYPTION))
            return false;
        if (bt_conn_get_security(conn) >= BT_SECURITY_L3)
            return false; // no new security transition would produce a callback
        begin();
        st.securing = true;
        const bool secured =
            bt_conn_set_security(conn, BT_SECURITY_L3) == 0 && await(pdMS_TO_TICKS(30000)) == 0;
        st.securing = false;
        if (!secured)
            return false;
    }
    return false;
}

std::uint8_t discovered(bt_conn*, const bt_gatt_attr* attribute, bt_gatt_discover_params* params) {
    if (attribute == nullptr) {
#if defined(BFLB_BLE_PATCH_ADD_ERRNO_IN_DISCOVER_CALLBACK)
        finish(params->err == BT_ATT_ERR_ATTRIBUTE_NOT_FOUND ? 0 : params->err);
#else
        finish(0);
#endif
        return BT_GATT_ITER_STOP;
    }
    if (params->type == BT_GATT_DISCOVER_PRIMARY) {
        auto* service = static_cast<bt_gatt_service_val*>(attribute->user_data);
        if (st.listing) {
            if (st.count == 255)
                st.overflow = true;
            else {
                append_uuid(service->uuid);
                ++st.count;
            }
        } else if (!st.found) {
            st.found = true;
            st.found_start = attribute->handle;
            st.found_end = service->end_handle;
        }
    } else if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
        auto* characteristic = static_cast<bt_gatt_chrc*>(attribute->user_data);
        if (st.listing) {
            if (st.count == 255)
                st.overflow = true;
            else {
                append_uuid(characteristic->uuid);
                append(characteristic->properties);
                append(&characteristic->value_handle, 2);
                ++st.count;
            }
        } else if (!st.found) {
            st.found = true;
            st.found_start = attribute->handle;
            st.found_handle = characteristic->value_handle;
            st.properties = characteristic->properties;
        }
    } else if (params->type == BT_GATT_DISCOVER_DESCRIPTOR) {
        if (bt_uuid_cmp(attribute->uuid, BT_UUID_GATT_CCC) == 0 && !st.found) {
            st.found = true;
            st.found_ccc = attribute->handle;
        }
    }
    return BT_GATT_ITER_CONTINUE;
}

bool discover(bt_conn* conn, std::uint8_t type, std::uint16_t start, std::uint16_t end,
    const std::uint8_t* uuid = nullptr, bool listing = false) {
    st.discovery = {};
    st.discovery.type = type;
    st.discovery.start_handle = start;
    st.discovery.end_handle = end;
    st.discovery.func = discovered;
    if (uuid != nullptr) {
        st.filter = make_uuid(uuid);
        st.discovery.uuid = &st.filter.uuid;
    }
    st.listing = listing;
    return procedure(conn, st.discovery, [&] { return bt_gatt_discover(conn, &st.discovery); });
}

std::uint8_t read_result(
    bt_conn*, std::uint8_t error, bt_gatt_read_params*, const void* data, std::uint16_t length) {
    if (error != 0 || data == nullptr) {
        finish(error);
        return BT_GATT_ITER_STOP;
    }
    append(data, length);
    return BT_GATT_ITER_CONTINUE;
}

void write_result(bt_conn*, std::uint8_t error, bt_gatt_write_params*) {
    finish(error);
}

void exchanged(bt_conn*, std::uint8_t error, bt_gatt_exchange_params*) {
    finish(error);
}

bool read_value(bt_conn* conn, std::uint16_t handle) {
    st.read = {};
    st.read.func = read_result;
    st.read.handle_count = 1;
    st.read.single.handle = handle;
    return procedure(conn, st.read, [&] { return bt_gatt_read(conn, &st.read); });
}

/** @brief The SDK's all-notifications hook avoids its unacknowledged subscription API. */
void notification(bt_conn*, std::uint16_t handle, const void* data, std::uint16_t length) {
    std::uint16_t uuid = 0;
    bool selected = false;
    {
        Guard guard;
        for (const auto& subscription : st.subscriptions)
            if (subscription.used && subscription.handle == handle) {
                uuid = subscription.uuid;
                selected = true;
            }
    }
    // The hook does not tell indications from notifications. Service Changed may also arrive
    // on a configuration the peer kept from an earlier bond; forgetting costs one rediscovery.
    if (handle == st.service_changed || !selected || uuid == cache::kServiceChangedUuid)
        ++st.cache_generation;
    if (selected) {
        st.notified = true;
        post(103, uuid, handle, data, length);
    }
}

struct Target {
    std::uint16_t handle, end, uuid;
    std::uint8_t properties;
};

bool discover_ccc(bt_conn* conn, const Target& target);

/** @brief Empties the cache when the link changed since it was filled, or on request. */
void cache_forget(bool always = false) {
    st.cache.forget(st.cache_generation, always);
}

/** @brief Makes the peer indicate database changes to us, unless its database is fixed.
 *  Never pairs.
 */
cache::Watch watch_database(bt_conn* conn) {
    if (st.service_changed != 0)
        return cache::Watch::valid;
    std::uint8_t uuid[16]{};
    std::memcpy(uuid, kBaseUuid, sizeof(kBaseUuid));
    uuid[12] = static_cast<std::uint8_t>(cache::kServiceChangedUuid);
    uuid[13] = static_cast<std::uint8_t>(cache::kServiceChangedUuid >> 8);
    st.watching = true;
    auto watch = cache::Watch::failed;
    if (discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, 1, 65535, uuid)) {
        if (!st.found) {
            watch = cache::Watch::valid;
        } else {
            // Its CCC follows the value directly; a short range keeps the search to one exchange.
            const auto end = static_cast<std::uint16_t>(std::min(0xFFFF, st.found_handle + 8));
            const Target target{st.found_handle, end, cache::kServiceChangedUuid, st.properties};
            if (discover_ccc(conn, target)) {
                static const std::uint16_t indications = BT_GATT_CCC_INDICATE;
                // Set before the write: the first indication may follow at once.
                st.service_changed = target.handle;
                st.write = {};
                st.write.func = write_result;
                st.write.handle = st.found_ccc;
                st.write.data = &indications;
                st.write.length = 2;
                if (procedure(conn, st.write, [&] { return bt_gatt_write(conn, &st.write); }))
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
void cache_check(bt_conn* conn) {
    cache_forget();
    if (!st.cache.unchecked())
        return;
    const auto watch = watch_database(conn);
    cache_forget(); // a change during the check leaves an empty, unchecked cache
    st.cache.settle(watch);
}

/** @brief Finds a service, asking the peer only on a cache miss. @return Its index, or -1. */
int find_service(bt_conn* conn, const std::uint8_t* uuid) {
    cache_check(conn);
    const int index = st.cache.find_service(uuid);
    if (index >= 0)
        return index;
    if (!discover(conn, BT_GATT_DISCOVER_PRIMARY, 1, 65535, uuid) || !st.found)
        return -1;
    return st.cache.add_service(uuid, {st.found_start, st.found_end});
}

BleError resolve_once(bt_conn* conn, const BleRequest& request, Target& target) {
    if (request.has_arg1) {
        if (request.arg1 < 2 || request.arg1 > 65535)
            return BleError::no_characteristic;
        std::uint8_t declaration[19];
        auto* output = st.output;
        const auto capacity = st.capacity;
        st.output = declaration;
        st.capacity = sizeof(declaration);
        st.used = 0;
        const bool ok = read_value(conn, static_cast<std::uint16_t>(request.arg1 - 1));
        const auto used = st.used;
        st.output = output;
        st.capacity = capacity;
        st.used = 0;
        if (!ok || st.overflow || (used != 5 && used != 19) ||
            (declaration[1] | declaration[2] << 8) != request.arg1)
            return BleError::no_characteristic;
        target.handle = static_cast<std::uint16_t>(request.arg1);
        target.end = 0xFFFF; // No service range is known for an explicit value handle.
        target.properties = declaration[0];
        const unsigned offset = used == 5 ? 3 : 15;
        target.uuid =
            static_cast<std::uint16_t>(declaration[offset] | declaration[offset + 1] << 8);
        return BleError::none;
    }
    const int index = find_service(conn, request.service);
    if (index < 0)
        return BleError::no_service;
    const auto range = st.cache.services[index].range;
    target.end = range.end;
    target.uuid = short_uuid(request.characteristic);
    if (const auto* cached = st.cache.find_characteristic(index, request.characteristic)) {
        target.handle = cached->handle;
        target.properties = cached->properties;
        return BleError::none;
    }
    if (!discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, range.start, range.end,
            request.characteristic) ||
        !st.found)
        return BleError::no_characteristic;
    target.handle = st.found_handle;
    target.properties = st.properties;
    st.cache.add_characteristic(index, request.characteristic, {target.handle, target.properties});
    return BleError::none;
}

/** @brief Finds only this characteristic's CCC, before the next declaration.
 *
 * blestack filters declarations out of descriptor discovery, so the boundary
 * must come from a separate unfiltered characteristic discovery. A UUID target
 * is also bounded by its service end; an explicit handle can reach 0xffff.
 */
bool discover_ccc(bt_conn* conn, const Target& target) {
    if (target.handle >= target.end)
        return false; // Includes 0xffff: do not wrap the first descriptor handle.
    const auto first = static_cast<std::uint16_t>(target.handle + 1);
    auto last = target.end;
    if (!discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, first, last))
        return false;
    if (st.found) {
        if (st.found_start <= first || st.found_start > last)
            return false; // No descriptor range, or an invalid declaration handle.
        last = static_cast<std::uint16_t>(st.found_start - 1);
    }
    return discover(conn, BT_GATT_DISCOVER_DESCRIPTOR, first, last) && st.found;
}

/** @brief Resolves the staged characteristic with handles that held throughout the lookup. */
BleError resolve(bt_conn* conn, const BleRequest& request, Target& target) {
    return cache::settled([] { return st.cache_generation.load(); },
        [&] { return resolve_once(conn, request, target); });
}

/** @brief Op 7 once, discarding any reply from an earlier attempt. */
BleError list_characteristics_once(bt_conn* conn, const BleRequest& request) {
    st.used = 0;
    st.count = 0;
    st.overflow = false;
    const int index = find_service(conn, request.service);
    if (index < 0)
        return BleError::no_service;
    const auto range = st.cache.services[index].range;
    append(0);
    const bool ok =
        discover(conn, BT_GATT_DISCOVER_CHARACTERISTIC, range.start, range.end, nullptr, true);
    if (st.capacity != 0)
        st.output[0] = st.count;
    return ok ? BleError::none : BleError::no_characteristic;
}

/** @brief Op 7: a change announced during discovery retries with a fresh range and reply. */
BleError list_characteristics(bt_conn* conn, const BleRequest& request) {
    return cache::settled([] { return st.cache_generation.load(); },
        [&] { return list_characteristics_once(conn, request); });
}

BleError access(bt_conn* conn, const BleRequest& request, const Target& target) {
    if (request.operation == 1) {
        if ((target.properties & BT_GATT_CHRC_READ) == 0)
            return BleError::cannot_read;
        return read_value(conn, target.handle) ? BleError::none : BleError::cannot_read;
    }
    if (request.operation == 2) {
        const bool confirmed = (target.properties & BT_GATT_CHRC_WRITE) != 0;
        const bool unconfirmed = (target.properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) != 0;
        if (!confirmed && !unconfirmed)
            return BleError::cannot_write;
        if ((request.confirm && confirmed) || !unconfirmed) {
            st.write = {};
            st.write.func = write_result;
            st.write.handle = target.handle;
            st.write.data = request.data;
            st.write.length = request.length;
            return procedure(conn, st.write, [&] { return bt_gatt_write(conn, &st.write); })
                       ? BleError::none
                       : BleError::write_failed;
        }
        return bt_gatt_write_without_response(
                   conn, target.handle, request.data, request.length, false) == 0
                   ? BleError::none
                   : BleError::write_failed;
    }
    Subscription* slot = nullptr;
    for (auto& subscription : st.subscriptions)
        if (subscription.used && subscription.handle == target.handle)
            slot = &subscription;
    const bool enable = request.operation == 3;
    if (!enable && slot == nullptr)
        return BleError::cannot_notify;
    if (slot == nullptr)
        for (auto& subscription : st.subscriptions)
            if (!subscription.used) {
                slot = &subscription;
                break;
            }
    if (slot == nullptr || (target.properties & (BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_INDICATE)) == 0)
        return BleError::cannot_notify;
    if (!enable && target.handle == st.service_changed) {
        // Unsubscribing from the Service Changed we watch only ends delivery to the script.
        {
            Guard guard;
            slot->used = false;
        }
        append(static_cast<std::uint8_t>(target.handle >> 8));
        append(static_cast<std::uint8_t>(target.handle));
        return BleError::none;
    }
    if (!discover_ccc(conn, target))
        return BleError::cannot_notify;
    const std::uint16_t value =
        enable ? ((target.properties & BT_GATT_CHRC_NOTIFY) != 0 ? BT_GATT_CCC_NOTIFY
                                                                 : BT_GATT_CCC_INDICATE)
               : 0;
    {
        Guard guard;
        slot->handle = target.handle;
        slot->uuid = target.uuid;
        slot->used = enable;
    }
    st.notified = false;
    st.write = {};
    st.write.func = write_result;
    st.write.handle = st.found_ccc;
    st.write.data = &value;
    st.write.length = 2;
    const bool written =
        request.confirm
            ? procedure(conn, st.write, [&] { return bt_gatt_write(conn, &st.write); })
            : bt_gatt_write_without_response(conn, st.write.handle, &value, 2, false) == 0;
    if (!written) {
        Guard guard;
        slot->used = false;
        return BleError::cannot_notify;
    }
    append(static_cast<std::uint8_t>(target.handle >> 8));
    append(static_cast<std::uint8_t>(target.handle));
    return BleError::none;
}

// ---- Local GATT server

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

ssize_t server_read(bt_conn* conn, const bt_gatt_attr* attribute, void* buffer,
    std::uint16_t length, std::uint16_t offset) {
    auto& characteristic = *static_cast<Characteristic*>(attribute->user_data);
    std::uint8_t value[255], size;
    {
        Guard guard;
        size = characteristic.length;
        std::memcpy(value, characteristic.value, size);
    }
    const auto result = bt_gatt_attr_read(conn, attribute, buffer, length, offset, value, size);
    if (result >= 0)
        post(221, short_uuid(characteristic.bytes), attribute->handle);
    return result;
}

ssize_t server_write(bt_conn*, const bt_gatt_attr* attribute, const void* value,
    std::uint16_t length, std::uint16_t offset, std::uint8_t flags) {
    auto& characteristic = *static_cast<Characteristic*>(attribute->user_data);
    if ((flags & BT_GATT_WRITE_FLAG_PREPARE) != 0)
        return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    if (offset != 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (length > sizeof(characteristic.value))
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    {
        Guard guard;
        std::memcpy(characteristic.value, value, length);
        characteristic.length = static_cast<std::uint8_t>(length);
    }
    post(222, short_uuid(characteristic.bytes), attribute->handle, value, length);
    return length;
}

void ccc_changed(const bt_gatt_attr* attribute, std::uint16_t value) {
    if (st.server == nullptr)
        return;
    for (auto& characteristic : st.server->characteristics)
        if (&characteristic.ccc == attribute->user_data) {
            post(static_cast<std::uint8_t>(223 + (value & 3)), short_uuid(characteristic.bytes),
                characteristic.value_attr->handle);
            break;
        }
}

void indicated(bt_conn*, const bt_gatt_attr* attribute, std::uint8_t error) {
    auto& characteristic = *static_cast<Characteristic*>(attribute->user_data);
    const int status = error;
    post(229, short_uuid(characteristic.bytes), attribute->handle, &status, sizeof(status));
    Guard guard;
    characteristic.indicating = false;
}

void notified(bt_conn*, void* context) {
    auto& characteristic = *static_cast<Characteristic*>(context);
    const int status = 0;
    post(229, short_uuid(characteristic.bytes), characteristic.value_attr->handle, &status,
        sizeof(status));
    Guard guard;
    --characteristic.notifying;
}

/** @brief Maps the legacy NimBLE flag numbers without exposing NimBLE to this backend. */
std::uint8_t permissions(std::uint16_t flags) {
    std::uint8_t result = 0;
    if ((flags & 2) != 0) {
        result |= (flags & 0x400) != 0   ? BT_GATT_PERM_READ_AUTHEN
                  : (flags & 0x200) != 0 ? BT_GATT_PERM_READ_ENCRYPT
                                         : BT_GATT_PERM_READ;
    }
    if ((flags & 12) != 0) {
        result |= (flags & 0x2000) != 0   ? BT_GATT_PERM_WRITE_AUTHEN
                  : (flags & 0x1000) != 0 ? BT_GATT_PERM_WRITE_ENCRYPT
                                          : BT_GATT_PERM_WRITE;
    }
    return result;
}

std::uint16_t characteristic_flags[kCharacteristicLimit]{};

bool register_services() {
    auto& server = *st.server;
    std::size_t next = 0;
    for (unsigned s = 0; s < server.service_count; ++s) {
        auto& service = server.services[s];
        service.native.attrs = server.attributes + next;
        auto& primary = server.attributes[next++];
        primary.uuid = BT_UUID_GATT_PRIMARY;
        primary.read = bt_gatt_attr_read_service;
        primary.user_data = &service.uuid.uuid;
        primary.perm = BT_GATT_PERM_READ;
        for (unsigned c = 0; c < server.characteristic_count; ++c) {
            auto& characteristic = server.characteristics[c];
            if (characteristic.service != s)
                continue;
            auto& declaration = server.attributes[next++];
            characteristic.declaration.uuid = &characteristic.uuid.uuid;
            characteristic.declaration.properties =
                static_cast<std::uint8_t>(characteristic_flags[c] & 0x3F);
            declaration.uuid = BT_UUID_GATT_CHRC;
            declaration.read = bt_gatt_attr_read_chrc;
            declaration.user_data = &characteristic.declaration;
            declaration.perm = BT_GATT_PERM_READ;
            auto& value = server.attributes[next++];
            value.uuid = &characteristic.uuid.uuid;
            value.read = server_read;
            value.write = server_write;
            value.user_data = &characteristic;
            value.perm = permissions(characteristic_flags[c]);
            characteristic.value_attr = &value;
            if ((characteristic_flags[c] & 0x30) != 0) {
                auto& ccc = server.attributes[next++];
                characteristic.ccc.cfg_changed = ccc_changed;
                ccc.uuid = BT_UUID_GATT_CCC;
                ccc.read = bt_gatt_attr_read_ccc;
                ccc.write = bt_gatt_attr_write_ccc;
                ccc.user_data = &characteristic.ccc;
                ccc.perm = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
            }
        }
        service.native.attr_count = server.attributes + next - service.native.attrs;
        if (bt_gatt_service_register(&service.native) != 0) {
            for (unsigned i = 0; i < s; ++i)
                (void)bt_gatt_service_unregister(&server.services[i].native);
            return false;
        }
    }
    for (unsigned c = 0; c < server.characteristic_count; ++c)
        server.characteristics[c].declaration.value_handle =
            server.characteristics[c].value_attr->handle;
    server.registered = true;
    return true;
}

/** @brief Parses bounded AD structures; malformed lengths fail before an SDK call. */
bool parse_ad(const std::uint8_t* bytes, std::size_t length, bt_data* fields, std::size_t& count) {
    count = 0;
    for (std::size_t offset = 0; offset < length;) {
        const auto size = bytes[offset++];
        if (size == 0)
            return offset == length;
        if (size > length - offset || count == 15)
            return false;
        fields[count++] = {bytes[offset], static_cast<std::uint8_t>(size - 1), bytes + offset + 1};
        offset += size;
    }
    return true;
}

bool advertise() {
    auto* server = st.server;
    if (server == nullptr || !server->enabled || server->advertising || st.linked || !st.opened)
        return true;
    bt_data advertisement[15], response[15];
    std::size_t advertisement_count, response_count;
    if (!parse_ad(server->ad, server->ad_length, advertisement, advertisement_count) ||
        !parse_ad(server->response, server->response_length, response, response_count))
        return false;
    bt_le_adv_param parameters{};
    parameters.options = BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_ONE_TIME;
    if (server->mode != 0)
        parameters.options |= BT_LE_ADV_OPT_CONNECTABLE;
    if (server->response_length == 0 && server->mode == 0)
        parameters.options |= BT_LE_ADV_OPT_USE_IDENTITY;
    parameters.interval_min = server->interval_min != 0 ? server->interval_min : 160;
    parameters.interval_max = server->interval_max != 0 ? server->interval_max : 240;
    const bool ok = bt_le_adv_start(&parameters, advertisement, advertisement_count, response,
                        response_count) == 0;
    server->advertising = ok;
    return ok;
}

BleError server_advertisement(const BleRequest& request) {
    auto* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    if (request.length > 31)
        return BleError::buffer_too_small;
    bt_data fields[15];
    std::size_t count;
    if (!parse_ad(request.data, request.length, fields, count))
        return BleError::cannot_write;
    const bool first = !server->registered && request.operation == 201;
    if (first && !register_services())
        return BleError::no_service;
    if (request.has_arg1) {
        if (request.arg1 != 0 && request.arg1 != 2)
            return BleError::cannot_write;
        server->mode = static_cast<std::uint8_t>(request.arg1);
    }
    if (server->advertising) {
        if (bt_le_adv_stop() != 0)
            return BleError::cannot_write;
        server->advertising = false;
    }
    if (request.operation == 202) {
        std::memcpy(server->response, request.data, request.length);
        server->response_length = static_cast<std::uint8_t>(request.length);
    } else {
        std::memcpy(server->ad, request.data, request.length);
        server->ad_length = static_cast<std::uint8_t>(request.length);
        server->enabled = true;
    }
    if (!advertise())
        return BleError::cannot_write;
    if (first)
        for (unsigned c = 0; c < server->characteristic_count; ++c) {
            const auto handle = server->characteristics[c].value_attr->handle;
            append(static_cast<std::uint8_t>(handle >> 8));
            append(static_cast<std::uint8_t>(handle));
        }
    return BleError::none;
}

BleError server_characteristic(const BleRequest& request, BleEvent& event) {
    auto* server = ensure_server();
    if (server == nullptr)
        return BleError::no_service;
    if (request.length > 255)
        return BleError::buffer_too_small;
    unsigned service = 0;
    while (service < server->service_count &&
           std::memcmp(server->services[service].bytes, request.service, 16) != 0)
        ++service;
    if (service == server->service_count) {
        if (server->registered || service == kServiceLimit)
            return BleError::no_service;
        server->services[service].uuid = make_uuid(request.service);
        std::memcpy(server->services[service].bytes, request.service, 16);
        ++server->service_count;
    }
    unsigned index = 0;
    while (index < server->characteristic_count &&
           (server->characteristics[index].service != service ||
               std::memcmp(server->characteristics[index].bytes, request.characteristic, 16) != 0))
        ++index;
    if (index == server->characteristic_count) {
        if (server->registered || index == kCharacteristicLimit)
            return BleError::no_characteristic;
        const auto flags = static_cast<std::uint16_t>(
            request.has_arg1 ? request.arg1 : 2 | 0x10 | 0x20 | (request.confirm ? 8 : 4));
        // Signed, reliable/extended writes and application authorization have
        // no immediate-response implementation in this backend.
        if ((flags & (0x40 | 0x80 | 0x100 | 0x800 | 0x4000 | 0x8000)) != 0)
            return BleError::cannot_write;
        auto& characteristic = server->characteristics[index];
        characteristic.uuid = make_uuid(request.characteristic);
        std::memcpy(characteristic.bytes, request.characteristic, 16);
        characteristic.service = static_cast<std::uint8_t>(service);
        characteristic_flags[index] = flags;
        ++server->characteristic_count;
    }
    auto& characteristic = server->characteristics[index];
    {
        Guard guard;
        std::memcpy(characteristic.value, request.data, request.length);
        characteristic.length = static_cast<std::uint8_t>(request.length);
    }
    event.uuid = short_uuid(characteristic.bytes);
    event.handle = characteristic.value_attr == nullptr ? 0 : characteristic.value_attr->handle;
    if (!server->registered)
        return BleError::none;
    auto* conn = connection_ref();
    if (conn == nullptr)
        return BleError::none;
    int result = 0;
    if (bt_gatt_is_subscribed(conn, characteristic.value_attr, BT_GATT_CCC_NOTIFY)) {
        bt_gatt_notify_params parameters{};
        parameters.attr = characteristic.value_attr;
        parameters.data = request.data;
        parameters.len = request.length;
        parameters.func = notified;
        parameters.user_data = &characteristic;
        {
            Guard guard;
            ++characteristic.notifying;
        }
        result = bt_gatt_notify_cb(conn, &parameters);
        if (result != 0) {
            Guard guard;
            --characteristic.notifying;
        }
    } else if (bt_gatt_is_subscribed(conn, characteristic.value_attr, BT_GATT_CCC_INDICATE)) {
        if (characteristic.indicating) {
            result = -1;
        } else {
            characteristic.indication = {};
            characteristic.indication.attr = characteristic.value_attr;
            characteristic.indication.func = indicated;
            characteristic.indication.data = characteristic.indicated_value;
            characteristic.indication.len = request.length;
            std::memcpy(characteristic.indicated_value, request.data, request.length);
            characteristic.indicating = true;
            result = bt_gatt_indicate(conn, &characteristic.indication);
            if (result != 0)
                characteristic.indicating = false;
        }
    }
    bt_conn_unref(conn);
    return result == 0 ? BleError::none : BleError::cannot_notify;
}

// ---- Worker and lifecycle

bool disconnect(bt_conn* conn) {
    st.draining = st.linked.load() || st.connecting.load();
    begin();
    if (bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN) != 0)
        return !st.linked;
    (void)await();
    return !st.linked;
}

bt_conn* connect(const BleRequest& request) {
    auto* conn = connection_ref();
    bt_addr_le_t peer{};
    peer.type = request.address_type;
    reverse_address(request.address, peer.a.val);
    if (conn != nullptr) {
        bool master;
        {
            Guard guard;
            master = st.master;
        }
        if (bt_addr_le_cmp(bt_conn_get_dst(conn), &peer) == 0)
            return conn;
        if (!master) {
            bt_conn_unref(conn);
            return nullptr;
        }
        if (!disconnect(conn)) {
            bt_conn_unref(conn);
            return nullptr;
        }
        bt_conn_unref(conn);
    }
    if (st.server != nullptr && st.server->advertising) {
        if (bt_le_adv_stop() != 0)
            return nullptr;
        st.server->advertising = false;
    }
    if (!hold_scan()) {
        st.hold = false;
        return nullptr;
    }
    // All earlier callbacks and subscription nodes are gone after disconnect.
    {
        Guard guard;
        std::memset(st.subscriptions, 0, sizeof(st.subscriptions));
    }
    bt_le_conn_param parameters{};
    parameters.interval_min = 8;
    parameters.interval_max = 16;
    parameters.timeout = 400;
    begin();
    st.connecting = true;
    conn = bt_conn_create_le(&peer, &parameters);
    if (conn == nullptr)
        st.connecting = false;
    const bool ok = conn != nullptr && await(pdMS_TO_TICKS(11000)) == 0 && st.linked;
    st.hold = false;
    if (!ok) {
        if (conn != nullptr) {
            (void)disconnect(conn);
            bt_conn_unref(conn);
        }
        return nullptr;
    }
    st.exchange = {};
    st.exchange.func = exchanged;
    if (!procedure(conn, st.exchange, [&] { return bt_gatt_exchange_mtu(conn, &st.exchange); })) {
        (void)disconnect(conn);
        bt_conn_unref(conn);
        return nullptr;
    }
    {
        Guard guard;
        st.connection_info.mtu = bt_gatt_get_mtu(conn);
    }
    return conn;
}

void run(const BleRequest& request) {
    st.output = request.reply;
    st.capacity = request.reply_capacity;
    st.used = 0;
    st.overflow = false;
    st.pin = request.pin <= 999999 ? request.pin : 0;
    BleEvent event{0, request.operation, 0, 0, 0};
    BleError error = BleError::none;
    if (request.operation == 201 || request.operation == 202)
        error = server_advertisement(request);
    else if (request.operation == 211)
        error = server_characteristic(request, event);
    else if (request.operation == 5) {
        auto* conn = connection_ref();
        if (conn != nullptr) {
            if (!disconnect(conn))
                error = BleError::disconnected;
            bt_conn_unref(conn);
        }
    } else {
        auto* conn = connect(request);
        if (conn == nullptr)
            error = BleError::no_connect;
        else {
            if (request.operation == 6) {
                st.count = 0;
                append(0);
                if (!discover(conn, BT_GATT_DISCOVER_PRIMARY, 1, 65535, nullptr, true))
                    error = BleError::no_service;
                if (st.capacity != 0)
                    st.output[0] = st.count;
            } else if (request.operation == 7) {
                error = list_characteristics(conn, request);
            } else {
                Target target{};
                error = resolve(conn, request, target);
                if (error == BleError::none) {
                    event.handle = target.handle;
                    event.uuid = target.uuid;
                    error = access(conn, request, target);
                }
            }
            if (error != BleError::none)
                cache_forget(true); // a changed peer database must not fail the next job too
            if (error == BleError::none && request.one_shot && request.operation == 3) {
                for (unsigned waited = 0;
                    waited < 15000 && !st.notified && st.linked && !st.close_requested;
                    waited += 20)
                    vTaskDelay(pdMS_TO_TICKS(20));
                if (!st.notified)
                    error = BleError::notify_timeout;
            }
            if (request.one_shot && (st.server == nullptr || !st.server->registered))
                (void)disconnect(conn);
            if (!st.linked && error != BleError::none)
                error = BleError::disconnected;
            bt_conn_unref(conn);
        }
    }
    if (st.overflow)
        error = BleError::buffer_too_small;
    event.error = static_cast<std::uint16_t>(error);
    event.length = error == BleError::none ? static_cast<std::uint16_t>(st.used) : 0;
    Guard guard;
    st.completion = event;
    st.last_operation = event.operation;
    st.last_error = event.error;
    st.completed = true;
    st.running = false;
}

void close_connection() {
    st.opened = false;
    if (st.server != nullptr) {
        st.server->enabled = false;
        if (st.server->advertising) {
            (void)bt_le_adv_stop();
            st.server->advertising = false;
        }
    }
    auto* conn = connection_ref();
    if (conn != nullptr) {
        const bool closed = disconnect(conn);
        bt_conn_unref(conn);
        if (!closed)
            return; // retry, retaining callback-visible database and value storage
    }
    if (st.server != nullptr) {
        Guard guard;
        for (const auto& characteristic : st.server->characteristics)
            if (characteristic.notifying != 0 || characteristic.indicating)
                return; // a delayed SDK workqueue callback still owns this storage
    }
    if (st.server != nullptr && st.server->registered) {
        for (unsigned s = 0; s < st.server->service_count; ++s)
            if (st.server->services[s].native.attr_count != 0) {
                if (bt_gatt_service_unregister(&st.server->services[s].native) != 0)
                    return;
                st.server->services[s].native.attr_count = 0;
            }
    }
    if (st.server != nullptr) {
        // Do not free callback-visible storage. Reopening starts a fresh database.
        Guard guard;
        std::memset(st.server, 0, sizeof(Server));
        st.server->mode = 2;
    }
    st.completed = false;
    xQueueReset(st.events);
    st.close_requested = false;
}

void worker_task(void*) {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        ControlGuard control(portMAX_DELAY);
        if (!control.locked)
            continue;
        if (st.running)
            run(st.request);
        if (st.close_requested)
            close_connection();
        else if (st.draining) {
            auto* conn = connection_ref();
            if (conn != nullptr) {
                (void)disconnect(conn);
                bt_conn_unref(conn);
            }
        } else if (st.ready && st.opened)
            (void)advertise();
    }
}

void enabled(int error) {
    if (error != 0) {
        st.failed = true;
        return;
    }
    connection_callbacks.connected = connected;
    connection_callbacks.disconnected = disconnected;
    connection_callbacks.security_changed = security_changed;
    connection_callbacks.identity_resolved = identity_resolved;
    bt_conn_cb_register(&connection_callbacks);
    authentication_callbacks.passkey_entry = passkey_entry;
    authentication_callbacks.cancel = pairing_cancel;
    authentication_callbacks.pairing_complete = pairing_complete;
    authentication_callbacks.pairing_failed = pairing_failed;
    if (bt_conn_auth_cb_register(&authentication_callbacks) != 0) {
        st.failed = true;
        return;
    }
    bt_gatt_register_notification_callback(notification);
    bt_gatt_register_mtu_callback(mtu_changed);
    (void)bt_set_name("newmota");
    st.ready = true;
}

void start_task(void*) {
    if (initialize_radio()) {
#if defined(BL602)
        ble_controller_init(configMAX_PRIORITIES - 1);
#else
        btble_controller_init(configMAX_PRIORITIES - 1);
#endif
        hci_driver_init();
        if (bt_enable(enabled) != 0)
            st.failed = true;
    } else
        st.failed = true;
    vTaskDelete(nullptr);
}
} // namespace

bool ble_start() {
    if (st.started)
        return !st.failed;
    if (st.signal == nullptr)
        st.signal = xSemaphoreCreateBinary();
    if (st.events == nullptr)
        st.events = xQueueCreate(4, sizeof(EventItem));
    if (st.control == nullptr)
        st.control = xSemaphoreCreateMutex();
    if (st.signal == nullptr || st.events == nullptr || st.control == nullptr)
        return false;
    if (st.worker == nullptr &&
        xTaskCreate(worker_task, "ble-job", 1536, nullptr, 5, &st.worker) != pdPASS)
        return false;
    if (st.scanner == nullptr &&
        xTaskCreate(scan_task, "ble-scan", 768, nullptr, 5, &st.scanner) != pdPASS)
        return false;
    st.started = true;
    if (xTaskCreate(start_task, "ble-init", 1024, nullptr, 10, nullptr) != pdPASS) {
        st.started = false;
        return false;
    }
    return true;
}

/** @brief bt_disable() releases host and controller but refuses while a link, scan or
 *  advertisement is active; Wi-Fi keeps the shared RF initialization.
 */
bool ble_stop() {
    if (!st.started)
        return true;
    if (!st.ready && !st.failed)
        return false; // still starting; bt_enable() would complete after the stop
    ble_connection_close();
    const auto since = xTaskGetTickCount();
    while (st.close_requested || st.running || st.draining) {
        if (xTaskGetTickCount() - since >= pdMS_TO_TICKS(8000))
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool held = hold_scan();
    ControlGuard control(portMAX_DELAY);
    const int error = held && control.locked ? bt_disable() : -1;
    if (error != 0 && error != -EALREADY) {
        st.hold = false;
        return false;
    }
    st.ready = false; // before releasing the hold, so the scan task stays away
    st.hold = false;
    st.failed = false;
    st.linked = false;
    st.connecting = false;
    st.started = false;
    return true;
}

bool ble_ready() {
    return st.ready;
}

void ble_scan_set(BleScanConsumer consumer, bool on) {
    const auto mask = static_cast<unsigned>(consumer);
    if (on)
        st.consumers.fetch_or(mask);
    else
        st.consumers.fetch_and(~mask);
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
    if (type > 1)
        return false;
    bt_addr_le_t peer{};
    peer.type = type;
    reverse_address(address, peer.a.val);
    Guard guard;
    for (unsigned i = 0; i < st.watch_count; ++i)
        if (bt_addr_le_cmp(&peer, &st.watched[i]) == 0)
            return true;
    if (st.watch_count == kWatchLimit)
        return false;
    st.watched[st.watch_count++] = peer;
    return true;
}

void ble_scan_unwatch() {
    Guard guard;
    st.watch_count = 0;
}

bool ble_scan_watched(std::size_t index, std::uint8_t* address, std::uint8_t& address_type) {
    bt_addr_le_t entry{};
    {
        Guard guard;
        if (index >= st.watch_count)
            return false;
        entry = st.watched[index];
    }
    reverse_address(entry.a.val, address);
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
    return true;
}

std::uint32_t ble_scan_dropped() {
    return st.scan_dropped;
}

bool ble_connection_open() {
    if (!st.started || st.failed || st.close_requested)
        return false;
    st.opened = true;
    return true;
}

void ble_connection_close() {
    ble_scan_set(BleScanConsumer::berry, false);
    st.opened = false;
    if (st.worker != nullptr) {
        st.close_requested = true;
        xTaskNotifyGive(st.worker);
    }
}

bool ble_connection_busy() {
    return st.running || st.completed || st.close_requested || st.draining;
}

bool ble_connection_request(const BleRequest& request) {
    if (!st.opened || !st.ready || ble_connection_busy())
        return false;
    if ((request.operation < 1 || request.operation > 7) && request.operation != 201 &&
        request.operation != 202 && request.operation != 211)
        return false;
    if ((request.data == nullptr && request.length != 0) ||
        (request.reply == nullptr && request.reply_capacity != 0) || request.address_type > 1)
        return false;
    st.request = request;
    st.running = true;
    xTaskNotifyGive(st.worker);
    return true;
}

bool ble_connection_pending() {
    return st.completed ||
           (!st.running && st.events != nullptr && uxQueueMessagesWaiting(st.events) != 0);
}

bool ble_connection_poll(BleEvent& event, std::uint8_t* payload, std::size_t capacity) {
    if (st.completed) {
        event = st.completion;
        st.completed = false;
        return true;
    }
    if (st.running || st.events == nullptr)
        return false;
    EventItem item;
    if (xQueueReceive(st.events, &item, 0) != pdTRUE)
        return false;
    event = item.event;
    if (event.length > capacity) {
        event.error = static_cast<std::uint16_t>(BleError::buffer_too_small);
        event.length = 0;
    } else if (event.length != 0)
        std::memcpy(payload, item.data, event.length);
    return true;
}

std::uint32_t ble_connection_dropped() {
    return st.event_dropped;
}

bool ble_configure(const BleRequest& request) {
    ControlGuard control;
    if (!control.locked || !st.ready || ble_connection_busy())
        return false;
    if (request.operation == 233) {
        if (request.length == 0 || request.length > CONFIG_BT_DEVICE_NAME_MAX ||
            std::memchr(request.data, 0, request.length) != nullptr)
            return false;
        char name[CONFIG_BT_DEVICE_NAME_MAX + 1]{};
        std::memcpy(name, request.data, request.length);
        return bt_set_name(name) == 0;
    }
    // Address replacement changes the controller identity and bond association.
    // The SDK offers no safe live identity-reset contract for this controller.
    if (request.operation != 232 || request.length != 5 || st.linked ||
        (request.data[0] != 0 && request.data[0] != 2))
        return false;
    auto* server = ensure_server();
    if (server == nullptr)
        return false;
    const auto minimum = static_cast<std::uint16_t>(request.data[1] | request.data[2] << 8);
    const auto maximum = static_cast<std::uint16_t>(request.data[3] | request.data[4] << 8);
    if ((minimum != 0 || maximum != 0) && (minimum < 0x20 || maximum > 0x4000 || minimum > maximum))
        return false;
    if (server->advertising) {
        if (bt_le_adv_stop() != 0)
            return false;
        server->advertising = false;
    }
    server->mode = request.data[0];
    server->interval_min = minimum;
    server->interval_max = maximum;
    return advertise();
}

bool ble_store_bond(const std::uint8_t* bytes, std::size_t size) {
    ControlGuard control;
    if (!control.locked || !st.ready || ble_connection_busy() || st.linked || st.connecting ||
        (st.server != nullptr && st.server->advertising) || bytes == nullptr ||
        size != sizeof(BondRecord))
        return false;
    BondRecord record{};
    std::memcpy(&record, bytes, sizeof(record));
    const std::uint16_t mask =
        static_cast<std::uint16_t>(record.key_mask[0] | record.key_mask[1] << 8);
    bt_addr_le_t local{}, peer{};
    peer.type = record.address_type;
    std::memcpy(peer.a.val, record.address, 6);
    if (std::memcmp(record.magic, "BLK1", 4) != 0 ||
        std::memcmp(record.revision, NEWMOTA_SDK_REVISION, 40) != 0 ||
        record.options != bond_options() || record.id != BT_ID_DEFAULT || peer.type > 1 ||
        record.enc_size < 7 || record.enc_size > 16 ||
        (mask & (BT_KEYS_LTK | BT_KEYS_SLAVE_LTK | BT_KEYS_LTK_P256)) == 0 ||
        (mask & ~BT_KEYS_ALL) != 0 || (record.flags & ~(BT_KEYS_AUTHENTICATED | BT_KEYS_SC)) != 0 ||
        bt_addr_le_cmp(&peer, &kAnyAddress) == 0 || bt_get_local_public_address(&local) != 0 ||
        std::memcmp(record.local_address, local.a.val, 6) != 0)
        return false;
#if !defined(CONFIG_BT_SIGNING)
    if ((mask & (BT_KEYS_LOCAL_CSRK | BT_KEYS_REMOTE_CSRK)) != 0)
        return false;
#endif
    // Refuse to evict another peer through the SDK's overwrite-oldest policy.
    if (bt_keys_find_addr(record.id, &peer) == nullptr) {
        std::uint8_t count = 0;
        bt_keys_foreach(
            BT_KEYS_ALL, [](bt_keys*, void* value) { ++*static_cast<std::uint8_t*>(value); },
            &count);
        if (count >= CONFIG_BT_MAX_PAIRED)
            return false;
    }
    auto* destination = bt_keys_get_addr(record.id, &peer);
    if (destination == nullptr)
        return false;
    std::memset(&destination->irk.rpa, 0, sizeof(destination->irk.rpa));
    destination->enc_size = record.enc_size;
    destination->flags = record.flags;
    destination->keys = mask;
    std::memcpy(destination->ltk.rand, record.ltk_rand, 8);
    std::memcpy(destination->ltk.ediv, record.ltk_ediv, 2);
    std::memcpy(destination->ltk.val, record.ltk, 16);
    std::memcpy(destination->irk.val, record.irk, 16);
#if !defined(CONFIG_BT_SMP_SC_PAIR_ONLY)
    std::memcpy(destination->slave_ltk.rand, record.slave_rand, 8);
    std::memcpy(destination->slave_ltk.ediv, record.slave_ediv, 2);
    std::memcpy(destination->slave_ltk.val, record.slave_ltk, 16);
#endif
#if defined(CONFIG_BT_SIGNING)
    std::memcpy(destination->local_csrk.val, record.local_csrk, 16);
    std::memcpy(&destination->local_csrk.cnt, record.local_counter, 4);
    std::memcpy(destination->remote_csrk.val, record.remote_csrk, 16);
    std::memcpy(&destination->remote_csrk.cnt, record.remote_counter, 4);
#endif
    return true;
}

void ble_info(BleInfo& info) {
    info = {};
    {
        Guard guard;
        info.connection = st.connection_info;
        if (st.server != nullptr) {
            if (st.server->registered)
                info.roles |= 4;
            if (st.server->advertising)
                info.roles |= 8;
        }
    }
    if (st.scan_running)
        info.roles |= 1;
    if (info.connection.present && info.connection.master)
        info.roles |= 2;
    info.version = 4;
    info.mtu = info.connection.present ? info.connection.mtu : 23;
    info.ready = st.ready;
    info.pending = ble_connection_busy();
    info.last_operation = st.last_operation;
    info.last_error = st.last_error;
    info.dropped_events = st.event_dropped;
    info.dropped_advertisements = st.scan_dropped;
    info.scan_stack_free = st.scanner == nullptr
                               ? 0
                               : static_cast<std::uint16_t>(
                                     uxTaskGetStackHighWaterMark(st.scanner) * sizeof(StackType_t));
    info.connection_stack_free =
        st.worker == nullptr ? 0
                             : static_cast<std::uint16_t>(
                                   uxTaskGetStackHighWaterMark(st.worker) * sizeof(StackType_t));
    if (st.ready) {
        bt_addr_le_t address{};
        if (bt_get_local_public_address(&address) == 0)
            reverse_address(address.a.val, info.local_address);
        bt_keys_foreach(
            BT_KEYS_ALL, [](bt_keys*, void* count) { ++*static_cast<std::uint8_t*>(count); },
            &info.bonds);
    }
}
} // namespace newmota::platform
