/*
  ble_module.cpp - Berry BLE module

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ble_module.cpp
 * @brief Berry BLE module following Tasmota's staged-context contract.
 *
 * The module owns one transfer buffer, exposed as `BLE.buffer`, a fixed-size
 * `bytes` object whose size() is its capacity (chosen with `BLE.init(size)`), and
 * `BLE.len` is the number of valid payload bytes. A script
 * registers callbacks, stages a peer, service and characteristic, puts any input
 * in `BLE.buffer`, sets `BLE.len` and starts one job with run(). The result is
 * delivered once as callback(error, operation, uuid, handle) from the cooperative
 * loop, with the payload at the start of the buffer and `BLE.len` set. Server
 * events and notifications arrive the same way. Received advertisements go to a
 * second callback(service_offset, manufacturer_offset): the buffer holds the
 * address (6), address type (1) and RSSI (1), then the payload from offset 8.
 *
 * The memory outlives the VM on purpose: the connection task reads and writes it
 * while a job runs and may still be running when the VM is destroyed.
 */

#include "services/berry/modules/ble/ble_module.h"

#include "newmota/platform/ble.h"
#include "services/ble/ble_advertisements.h"

extern "C" {
#include "be_gc.h"
#include "be_mapping.h"
#include "be_list.h"
#include "berry.h"
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
namespace platform = newmota::platform;
namespace ble = newmota::ble;

/** @brief Native callback made by the cb module; extra arguments are ignored by closures. */
using Callback = int (*)(int, int, int, int, int);

constexpr std::size_t kDefaultSize = 256;
constexpr std::size_t kMinimumSize = 32;
constexpr std::size_t kMaximumSize = 4096;
constexpr std::size_t kAdvertisementHeader = 8;
constexpr unsigned kEventsPerPoll = 4;
constexpr std::uint8_t kBaseUuid[12] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};

/** @brief The transfer buffer. Kept for the life of the firmware once created. */
std::uint8_t* memory;
std::size_t memory_size = kDefaultSize;

struct State {
    bvm* vm;
    blist* root; ///< Fixed GC root holding the mapped bytes object behind BLE.buffer.
    Callback connection_callback;
    Callback advertisement_callback;
    platform::BleRequest staged; ///< Peer, service and characteristic set so far.
    bool has_address, has_service, has_characteristic;
    std::size_t length; ///< BLE.len
};

State state;

[[nodiscard]] int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = static_cast<char>(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/** @brief Parses 4 or 8 hex digits, or a 36-character UUID, into 16 little-endian bytes. */
[[nodiscard]] bool parse_uuid(const char* text, std::uint8_t* out) {
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    const std::size_t length = std::strlen(text);
    if (length == 4 || length == 8) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const int digit = nibble(text[i]);
            if (digit < 0)
                return false;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        std::memcpy(out, kBaseUuid, sizeof(kBaseUuid));
        for (int i = 0; i < 4; ++i)
            out[12 + i] = static_cast<std::uint8_t>(value >> (8 * i));
        return true;
    }
    if (length != 36)
        return false;
    int index = 15; // the text is most significant first, the output least significant first
    for (std::size_t i = 0; i < length;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i++] != '-')
                return false;
            continue;
        }
        const int high = nibble(text[i]), low = nibble(text[i + 1]);
        if (high < 0 || low < 0)
            return false;
        out[index--] = static_cast<std::uint8_t>((high << 4) | low);
        i += 2;
    }
    return true;
}

void push_list(bvm* vm, blist* list) {
    be_pushnil(vm);
    var_setlist(be_indexof(vm, -1), list);
}

void push_value(bvm* vm, const bvalue& value) {
    be_pushnil(vm);
    var_setval(be_indexof(vm, -1), &value);
}

void ensure_memory(bvm* vm) {
    if (memory == nullptr) {
        memory = static_cast<std::uint8_t*>(std::calloc(1, memory_size));
        if (memory == nullptr)
            be_raise(vm, "memory_error", "BLE: out of memory for the buffer");
    }
}

/** @brief Pushes the single bytes object mapped onto the buffer, creating and rooting it once. */
void push_buffer(bvm* vm) {
    ensure_memory(vm);
    if (state.root != nullptr) {
        push_value(vm, *be_list_at(state.root, 0));
        return;
    }
    (void)be_getbuiltin(vm, "bytes");
    be_pushcomptr(vm, memory);
    be_pushint(vm, static_cast<bint>(memory_size));
    be_call(vm, 2); // a mapped, fixed-size bytes object that does not own the memory
    be_pop(vm, 2);
    auto* list = be_list_new(vm);
    push_list(vm, list);
    (void)be_list_push(vm, list, be_indexof(vm, -2));
    be_gc_fix(vm, reinterpret_cast<bgcobject*>(list));
    be_pop(vm, 1); // the list; the bytes object stays on top
    state.root = list;
}

void release_connection() {
    platform::ble_connection_close();
    state.connection_callback = nullptr;
}

void release_advertisement() {
    ble::set_script_consumer(false);
    state.advertisement_callback = nullptr;
}

/** @brief Reads a callback argument. @return False when the call clears the registration. */
bool read_callback(bvm* vm, Callback& callback) {
    const int argc = be_top(vm);
    if (argc < 1 || be_isnil(vm, 1) || (be_isint(vm, 1) && be_toint(vm, 1) == 0) ||
        (be_iscomptr(vm, 1) && be_tocomptr(vm, 1) == nullptr))
        return false;
    if (argc > 1)
        be_raise(vm, "ble_error", "BLE: pass only the callback, the buffer is BLE.buffer");
    if (!be_iscomptr(vm, 1))
        be_raise(vm, "type_error", "BLE expects a callback made by cb.gen_cb");
    callback = reinterpret_cast<Callback>(be_tocomptr(vm, 1));
    return true;
}

/** @brief Sets the buffer size; it is fixed once the buffer exists. */
void set_size(bvm* vm, bint value) {
    if (value < static_cast<bint>(kMinimumSize) || value > static_cast<bint>(kMaximumSize))
        be_raise(vm, "value_error", "BLE buffer size is out of range");
    // The buffer is fixed once it exists: scripts and the connection task share it.
    if (memory != nullptr && static_cast<std::size_t>(value) != memory_size)
        be_raise(vm, "ble_error", "BLE.init: the buffer already exists with another size");
    memory_size = static_cast<std::size_t>(value);
}

/** @brief Runs on `import BLE` with the module as argument, and again for an explicit
 *  `BLE.init([size])`. Importing allocates nothing, so a later init(size) can still choose.
 */
int Init(bvm* vm) {
    if (!platform::ble_ready())
        be_raise(vm, "ble_error", "BLE: off");
    if (be_top(vm) >= 1 && be_ismodule(vm, 1)) {
        be_pushvalue(vm, 1); // import uses the result of init() as the module
        be_return(vm);
    }
    if (be_top(vm) >= 1 && !be_isnil(vm, 1)) {
        if (!be_isint(vm, 1))
            be_raise(vm, "type_error", "BLE.init expects a buffer size");
        set_size(vm, be_toint(vm, 1));
    }
    be_return_nil(vm);
}

int ConnCb(bvm* vm) {
    Callback callback;
    if (!read_callback(vm, callback)) {
        release_connection();
        be_return_nil(vm);
    }
    if (platform::ble_connection_busy())
        be_raise(vm, "ble_error", "BLE: a job is running");
    ensure_memory(vm);
    if (!platform::ble_connection_open())
        be_raise(vm, "ble_error", "BLE: could not start the connection task");
    state.connection_callback = callback;
    be_return_nil(vm);
}

int AdvCb(bvm* vm) {
    Callback callback;
    if (!read_callback(vm, callback)) {
        release_advertisement();
        be_return_nil(vm);
    }
    ensure_memory(vm);
    state.advertisement_callback = callback;
    ble::set_script_consumer(true);
    be_return_nil(vm);
}

int AdvWatch(bvm* vm) {
    if (be_top(vm) < 1 || !be_isbytes(vm, 1))
        be_raise(vm, "type_error", "BLE.adv_watch expects bytes");
    std::size_t size = 0;
    const auto* mac = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    if (size != 6)
        be_raise(vm, "ble_error", "BLE: wrong size of MAC");
    const bint type = be_top(vm) >= 2 && be_isint(vm, 2) ? be_toint(vm, 2) : 0;
    if (!platform::ble_scan_watch(mac, type >= 0 && type < 4 ? static_cast<std::uint8_t>(type) : 0))
        be_raise(vm, "ble_error", "BLE: could not add MAC to watch list");
    be_return_nil(vm);
}

int Store(bvm* vm) {
    if (be_top(vm) < 1 || !be_isbytes(vm, 1))
        be_raise(vm, "type_error", "BLE.store expects bytes");
    std::size_t size = 0;
    const auto* record = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    if (!platform::ble_store_bond(record, size))
        be_raise(vm, "ble_error", "BLE: bond blob rejected");
    be_return_nil(vm);
}

int SetMac(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 1 || !be_isbytes(vm, 1))
        be_raise(vm, "type_error", "BLE.set_MAC expects bytes");
    std::size_t size = 0;
    const auto* mac = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    if (size != 6)
        be_raise(vm, "ble_error", "BLE: wrong size of MAC");
    const bint type = argc >= 2 && be_isint(vm, 2) ? be_toint(vm, 2) : 0;
    std::memcpy(state.staged.address, mac, 6);
    state.staged.address_type = type >= 0 && type < 4 ? static_cast<std::uint8_t>(type) : 0;
    state.staged.pin =
        argc >= 3 && be_isint(vm, 3) ? static_cast<std::uint32_t>(be_toint(vm, 3)) : 0;
    state.has_address = true;
    be_return_nil(vm);
}

/** @brief Stages a UUID string into @p out. */
bool stage_uuid(bvm* vm, std::uint8_t* out) {
    if (be_top(vm) < 1 || !be_isstring(vm, 1))
        be_raise(vm, "type_error", "BLE expects a UUID string");
    return parse_uuid(be_tostring(vm, 1), out);
}

int SetSvc(bvm* vm) {
    if (!stage_uuid(vm, state.staged.service))
        be_raise(vm, "ble_error", "BLE: could not set service");
    state.has_service = true;
    be_return_nil(vm);
}

int SetChr(bvm* vm) {
    if (!stage_uuid(vm, state.staged.characteristic))
        be_raise(vm, "ble_error", "BLE: could not set characteristic");
    state.has_characteristic = true;
    be_return_nil(vm);
}

int Run(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 1 || !be_isint(vm, 1))
        be_raise(vm, "value_error", "BLE.run: wrong arguments");
    const bint operation = be_toint(vm, 1);
    const bool one_shot = operation >= 11 && operation <= 17;
    const bint op = one_shot ? operation - 10 : operation;
    const bool client = op >= 1 && op <= 7;
    const bool server = op == 201 || op == 202 || op == 211;
    const bool configuration = op >= 231 && op <= 233;
    if (!client && !server && !configuration)
        be_raise(vm, "value_error", "BLE.run: operation is not available");
    if (state.connection_callback == nullptr)
        be_raise(vm, "ble_error", "BLE: register conn_cb first");
    if ((client || op == 231) && !state.has_address)
        be_raise(vm, "ble_error", "BLE: MAC not set");
    const bool has_arg1 = argc >= 3 && !be_isnil(vm, 3);
    const bint arg1 = has_arg1 ? be_toint(vm, 3) : 0;
    if (client && has_arg1 && (arg1 < 2 || arg1 > 0xFFFF))
        be_raise(vm, "value_error", "BLE.run: invalid handle");
    if (((op >= 1 && op <= 4 && !has_arg1) || op == 211) &&
        (!state.has_service || !state.has_characteristic))
        be_raise(vm, "ble_error", "BLE: service or characteristic not set");
    if (op == 7 && !state.has_service)
        be_raise(vm, "ble_error", "BLE: service not set");
    // BLE.len describes the input; consume it so a stale value cannot leak into the next job.
    const std::size_t length = state.length;
    state.length = 0;
    const bool carries_data = op == 2 || server || op == 232 || op == 233;
    if (length > memory_size || (carries_data && op == 2 && length == 0))
        be_raise(vm, "ble_error", "BLE: invalid BLE.len");
    platform::BleRequest request = state.staged;
    request.operation = static_cast<std::uint8_t>(op);
    request.one_shot = one_shot;
    request.confirm = argc >= 2 && be_tobool(vm, 2);
    request.has_arg1 = has_arg1;
    request.arg1 = static_cast<std::int32_t>(arg1);
    request.reply = memory;
    request.reply_capacity = static_cast<std::uint16_t>(memory_size);
    request.data = memory;
    request.length = carries_data ? static_cast<std::uint16_t>(length) : 0;
    if (configuration) {
        if (!platform::ble_configure(request))
            be_raise(vm, "ble_error", "BLE: could not run operation");
        be_return_nil(vm);
    }
    if (!platform::ble_connection_request(request))
        be_raise(vm, "ble_error", "BLE: could not run operation");
    be_return_nil(vm);
}

void put_key(bvm* vm, const char* key) {
    be_pushstring(vm, key);
}

void put_done(bvm* vm) {
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void put_int(bvm* vm, const char* key, bint value) {
    put_key(vm, key);
    be_pushint(vm, value);
    put_done(vm);
}

void put_bool(bvm* vm, const char* key, bool value) {
    put_key(vm, key);
    be_pushbool(vm, value);
    put_done(vm);
}

void put_string(bvm* vm, const char* key, const char* value) {
    put_key(vm, key);
    be_pushstring(vm, value);
    put_done(vm);
}

void put_address(bvm* vm, const char* key, const std::uint8_t* address) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", address[0], address[1],
        address[2], address[3], address[4], address[5]);
    put_string(vm, key, text);
}

int Info(bvm* vm) {
    platform::BleInfo info;
    platform::ble_info(info);
    be_newobject(vm, "map");
    put_int(vm, "role_bits", info.roles);
    char role[40] = "";
    static const char* const names[] = {"scanner", "client", "server", "advertiser"};
    for (unsigned i = 0; i < 4; ++i)
        if (info.roles & (1u << i)) {
            if (role[0])
                std::strncat(role, "+", sizeof(role) - std::strlen(role) - 1);
            std::strncat(role, names[i], sizeof(role) - std::strlen(role) - 1);
        }
    put_string(vm, "role", info.roles ? role : "none");
    put_address(vm, "local_addr", info.local_address);
    put_int(vm, "power", info.power);
    put_int(vm, "MTU", info.mtu);
    put_int(vm, "version", info.version);
    put_int(vm, "bonds", info.bonds);
    put_key(vm, "debug");
    be_newobject(vm, "map");
    put_bool(vm, "ready", info.ready);
    put_bool(vm, "pending", info.pending);
    put_int(vm, "operation", info.last_operation);
    put_int(vm, "error", info.last_error);
    put_int(vm, "queue_drops", info.dropped_events);
    put_int(vm, "adv_drops", info.dropped_advertisements + ble::script_advertisements_dropped());
    put_int(vm, "scan_stack_free", info.scan_stack_free);
    put_int(vm, "connection_stack_free", info.connection_stack_free);
    be_pop(vm, 1);
    put_done(vm);
    if (info.connection.present) {
        const auto& link = info.connection;
        put_key(vm, "connection");
        be_newobject(vm, "map");
        put_address(vm, "peer_addr", link.peer_address);
        put_address(vm, "peerID_addr", link.peer_id_address);
        if (link.has_rssi)
            put_int(vm, "RSSI", link.rssi);
        put_int(vm, "MTU", link.mtu);
        put_bool(vm, "bonded", link.bonded);
        put_bool(vm, "master", link.master);
        put_bool(vm, "encrypted", link.encrypted);
        put_bool(vm, "authenticated", link.authenticated);
        if (link.has_irk) {
            char irk[33];
            for (int i = 0; i < 16; ++i)
                std::snprintf(irk + 2 * i, 3, "%02X", link.irk[i]);
            put_string(vm, "IRK", irk);
        }
        be_pop(vm, 1);
        put_done(vm);
    }
    be_pop(vm, 1);
    be_return(vm);
}

int Loop(bvm* vm) {
    return newmota::dispatch_ble_events(vm);
}

/** @brief Module properties: buffer read-only, len writable. */
int Member(bvm* vm) {
    const char* name = be_top(vm) >= 1 && be_isstring(vm, 1) ? be_tostring(vm, 1) : "";
    if (std::strcmp(name, "buffer") == 0) {
        push_buffer(vm);
        be_return(vm);
    }
    if (std::strcmp(name, "len") == 0) {
        be_pushint(vm, static_cast<bint>(state.length));
        be_return(vm);
    }
    be_raise(vm, "attribute_error", "module 'BLE' has no such attribute");
    be_return_nil(vm);
}

int SetMember(bvm* vm) {
    const char* name = be_top(vm) >= 2 && be_isstring(vm, 1) ? be_tostring(vm, 1) : "";
    if (be_top(vm) < 2 || !be_isint(vm, 2)) {
        if (std::strcmp(name, "buffer") == 0)
            be_raise(vm, "attribute_error", "BLE.buffer is read-only");
        be_raise(vm, "type_error", "BLE properties take an integer");
    }
    const bint value = be_toint(vm, 2);
    if (std::strcmp(name, "len") == 0) {
        if (value < 0 || static_cast<std::size_t>(value) > memory_size)
            be_raise(vm, "value_error", "BLE.len is outside the buffer");
        state.length = static_cast<std::size_t>(value);
    } else {
        be_raise(vm, "attribute_error", "BLE has no such writable property");
    }
    be_pushbool(vm, btrue);
    be_return(vm);
}
} // namespace

extern "C" {
be_native_module_attr_table(BLE){
    be_native_module_function("init", Init),
    be_native_module_function("member", Member),
    be_native_module_function("setmember", SetMember),
    be_native_module_function("loop", Loop),
    be_native_module_function("info", Info),
    be_native_module_function("conn_cb", ConnCb),
    be_native_module_function("serv_cb", ConnCb),
    be_native_module_function("adv_cb", AdvCb),
    be_native_module_function("adv_watch", AdvWatch),
    be_native_module_function("set_MAC", SetMac),
    be_native_module_function("set_svc", SetSvc),
    be_native_module_function("set_chr", SetChr),
    be_native_module_function("run", Run),
    be_native_module_function("store", Store),
};
extern const bntvmodule_t be_native_module_BLE = {
    "BLE", BLE_attrs, sizeof(BLE_attrs) / sizeof(BLE_attrs[0]), nullptr};
}

namespace newmota {
void install_ble_module(bvm* vm) {
    state.vm = vm;
}

void remove_ble_module() {
    release_advertisement();
    release_connection();
    if (state.vm != nullptr)
        be_cb_deinit(state.vm); // the static slots would keep pointing at the deleted VM
    if (state.root != nullptr && state.vm != nullptr)
        be_gc_unfix(state.vm, reinterpret_cast<bgcobject*>(state.root));
    state = State{}; // the buffer memory stays: the connection task may still be using it
}

bool ble_module_pending() {
    return (state.connection_callback != nullptr && platform::ble_connection_pending()) ||
           (state.advertisement_callback != nullptr && !platform::ble_connection_busy() &&
               ble::script_advertisement_pending());
}

namespace {
/** @brief Offsets of the first service data (0x16) and manufacturer data (0xFF) fields. */
void locate_fields(std::size_t end, int& service, int& manufacturer) {
    std::size_t index = kAdvertisementHeader;
    service = manufacturer = 0;
    while (index < end) {
        const std::size_t length = memory[index];
        if (length == 0 || length > end - index - 1)
            break;
        if (length >= 3 && memory[index + 1] == 0x16)
            service = static_cast<int>(index + 2);
        else if (length >= 3 && memory[index + 1] == 0xFF)
            manufacturer = static_cast<int>(index + 2);
        index += length + 1;
    }
}
} // namespace

int dispatch_ble_events(bvm* vm) {
    for (unsigned i = 0; i < kEventsPerPoll && state.connection_callback != nullptr; ++i) {
        platform::BleEvent event{};
        if (!platform::ble_connection_poll(event, memory, memory_size))
            break;
        state.length = event.length;
        state.connection_callback(event.error, event.operation, event.uuid, event.handle, 0);
    }
    // One shared buffer: advertisements wait while a job may still be reading its input.
    for (unsigned i = 0; i < kEventsPerPoll && state.advertisement_callback != nullptr &&
                         !platform::ble_connection_busy();
        ++i) {
        platform::BleAdvertisement advertisement;
        if (!ble::take_script_advertisement(advertisement))
            break;
        std::memcpy(memory, advertisement.address, 6);
        memory[6] = advertisement.address_type;
        memory[7] = static_cast<std::uint8_t>(advertisement.rssi);
        // The buffer bounds the payload; BLE.len reports what was copied.
        const std::size_t room =
            std::min<std::size_t>(memory_size - kAdvertisementHeader, advertisement.length);
        std::memcpy(memory + kAdvertisementHeader, advertisement.payload, room);
        state.length = room;
        int service, manufacturer;
        locate_fields(kAdvertisementHeader + room, service, manufacturer);
        state.advertisement_callback(service, manufacturer, 0, 0, 0);
    }
    be_return_nil(vm);
}
} // namespace newmota
