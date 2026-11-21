/*
  matter_module.cpp - Berry matter module

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/berry/matter_module.h"

#include "core/storage/filesystem.h"
#include "services/matter/matter_cert.h"
#include "services/matter/matter_stack.h"
#include "services/matter/matter_wire.h"
#include "services/network/network_service.h"
#include "services/power/power_service.h"

#include "newmota_features.h"
#if USE_SENSORS
#include "drivers/sensors/sensor_service.h"
#endif
#if USE_MATTER_BLE
#include "services/matter/matter_ble.h"
#endif
#if USE_MATTER_ENERGY
#include "services/energy/energy_service.h"
#endif

#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"

extern "C" {
#include "berry.h"
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#define MATTER_CLASSES(X)                                                                          \
    X(Base38)                                                                                      \
    X(Certs)                                                                                       \
    X(Tlv)                                                                                         \
    X(Handshake)                                                                                   \
    X(Report)                                                                                      \
    X(Subscription)                                                                                \
    X(IM)                                                                                          \
    X(Motion)                                                                                      \
    X(Cluster)                                                                                     \
    X(Descriptor)                                                                                  \
    X(AccessControl)                                                                               \
    X(BasicInformation)                                                                            \
    X(GeneralCommissioning)                                                                        \
    X(NetworkCommissioning)                                                                        \
    X(GeneralDiagnostics)                                                                          \
    X(AdminCommissioning)                                                                          \
    X(OperationalCredentials)                                                                      \
    X(GroupKeys)                                                                                   \
    X(Identify)                                                                                    \
    X(Groups)                                                                                      \
    X(RelayOutput)                                                                                 \
    X(VirtualOutput)                                                                               \
    X(LocalLamp)                                                                                   \
    X(OnOff)                                                                                       \
    X(LevelControl)                                                                                \
    X(ColorControl)                                                                                \
    X(Measurement)                                                                                 \
    X(Fabric)                                                                                      \
    X(Session)                                                                                     \
    X(Endpoint)                                                                                    \
    X(Device)

#if USE_MATTER_BRIDGE
#define MATTER_BRIDGE_CLASSES(X)                                                                   \
    X(Remote)                                                                                      \
    X(RemoteOutput)                                                                                \
    X(RemoteLamp)                                                                                  \
    X(BridgedInfo)                                                                                 \
    X(Bridge)
#else
#define MATTER_BRIDGE_CLASSES(X)
#endif

#if USE_MATTER_ENERGY
#define MATTER_ENERGY_CLASSES(X)                                                                   \
    X(PowerTopology)                                                                               \
    X(ElectricalPower)                                                                             \
    X(ElectricalEnergy)
#else
#define MATTER_ENERGY_CLASSES(X)
#endif

#if USE_MATTER_TCP
#define MATTER_CLIENT_CLASSES(X)                                                                   \
    X(CaseInitiator)                                                                               \
    X(Client)
#else
#define MATTER_CLIENT_CLASSES(X)
#endif

#if USE_MATTER_CAMERA
#define MATTER_CAMERA_CLASSES(X)                                                                   \
    X(CameraStreams)                                                                               \
    X(CameraPTZ)                                                                                   \
    X(WebRTCProvider)
#else
#define MATTER_CAMERA_CLASSES(X)
#endif

extern "C" {
#define DECLARE_CLASS(name) extern const bclass be_class_Matter_##name;
MATTER_CLASSES(DECLARE_CLASS)
MATTER_BRIDGE_CLASSES(DECLARE_CLASS)
MATTER_ENERGY_CLASSES(DECLARE_CLASS)
MATTER_CLIENT_CLASSES(DECLARE_CLASS)
MATTER_CAMERA_CLASSES(DECLARE_CLASS)
#undef DECLARE_CLASS
}

namespace newmota {
namespace {
namespace fs = filesystem;

constexpr unsigned kMessagesPerTick = 4;
constexpr std::uint32_t kTickMilliseconds = 250;
constexpr std::size_t kMaxStoredFile = 2048;
constexpr std::string_view kStoreDirectory{"/.newmota/matter"};
constexpr unsigned kMaxTlvDepth = 8;
constexpr unsigned kMaxTlvItems = 128;
constexpr std::size_t kMaxScanResults = 10;
constexpr std::uint32_t kStartWithoutWifiMilliseconds = 20000;

bool enabled{};
NetworkService* network_service{};
const PowerService* power_service{};
bool started{};
std::uint32_t next_tick{};
std::uint64_t systime_ms{};
std::uint32_t systime_last{};

/** @brief Milliseconds since start in 64 bits; each call within 49 days carries the wrap. */
std::uint64_t systime() {
    const std::uint32_t now = platform::monotonic_milliseconds();
    systime_ms += now - systime_last;
    systime_last = now;
    return systime_ms;
}

struct NamedClass {
    const char* name;
    const bclass* value;
};

#define CLASS_ENTRY(name) {#name, &be_class_Matter_##name},
constexpr NamedClass kClasses[] = {MATTER_CLASSES(CLASS_ENTRY) MATTER_BRIDGE_CLASSES(CLASS_ENTRY)
        MATTER_ENERGY_CLASSES(CLASS_ENTRY) MATTER_CLIENT_CLASSES(CLASS_ENTRY)
            MATTER_CAMERA_CLASSES(CLASS_ENTRY)};
#undef CLASS_ENTRY

/** @brief Resolves the solidified classes; they use no heap until a script names them. */
int Member(bvm* vm) {
    const char* const name = be_tostring(vm, 1);
    for (const NamedClass& entry : kClasses) {
        if (std::strcmp(name, entry.name) == 0) {
            be_pushntvclass(vm, entry.value);
            be_return(vm);
        }
    }
    // An optional class: nil tells the stack to go without, where undefined would raise.
    if (std::strcmp(name, "Bridge") == 0)
        be_return_nil(vm);
    be_getmodule(vm, "undefined");
    be_return(vm);
}

/** @brief Pushes one element and everything inside it; hostile input ends at the limits. */
[[nodiscard]] bool push_tlv(bvm* const vm, const std::uint8_t* const data, const std::size_t size,
    std::size_t& offset, const matter::TlvElement& element, const unsigned depth, unsigned& items) {
    using matter::TlvType;
    offset = element.next;
    be_stack_require(vm, 6);
    switch (element.type) {
    case TlvType::signed_integer:
    case TlvType::unsigned_integer:
        // Berry has no 64-bit integer here: 8-byte encodings stay little-endian bytes.
        if (element.width == 8U) {
            std::uint8_t value[8];
            for (unsigned index = 0; index < 8U; ++index)
                value[index] = static_cast<std::uint8_t>(element.value >> (index * 8U));
            be_pushbytes(vm, value, sizeof(value));
        } else if (element.type == TlvType::signed_integer) {
            const unsigned shift = 64U - element.width * 8U;
            be_pushint(
                vm, static_cast<bint>(static_cast<std::int64_t>(element.value << shift) >> shift));
        } else {
            be_pushint(vm, static_cast<bint>(static_cast<std::uint32_t>(element.value)));
        }
        return true;
    case TlvType::boolean:
        be_pushbool(vm, element.value != 0U);
        return true;
    case TlvType::real:
        be_pushreal(vm, static_cast<breal>(element.real));
        return true;
    case TlvType::text:
        be_pushnstring(vm, reinterpret_cast<const char*>(element.data), element.length);
        return true;
    case TlvType::octets:
        be_pushbytes(vm, element.data, element.length);
        return true;
    case TlvType::null:
        be_pushnil(vm);
        return true;
    case TlvType::end:
        return false;
    case TlvType::structure:
    case TlvType::array:
    case TlvType::list:
        break;
    }
    if (depth >= kMaxTlvDepth)
        return false;
    const bool keyed = element.type != TlvType::array;
    be_newobject(vm, keyed ? "map" : "list");
    int position = 0;
    while (true) {
        matter::TlvElement child;
        if (!matter::tlv_next(data, size, offset, child) || ++items > kMaxTlvItems)
            return false;
        if (child.type == TlvType::end) {
            offset = child.next;
            break;
        }
        if (keyed)
            be_pushint(vm, child.has_tag ? static_cast<bint>(child.tag) : --position);
        if (!push_tlv(vm, data, size, offset, child, depth + 1U, items))
            return false;
        if (keyed) {
            (void)be_data_insert(vm, -3);
            be_pop(vm, 2);
        } else {
            be_data_push(vm, -2);
            be_pop(vm, 1);
        }
    }
    be_pop(vm, 1);
    return true;
}

/** @brief matter.tlv(bytes): lists and structures become maps by tag, arrays lists; nil if bad. */
int Tlv(bvm* vm) {
    std::size_t size = 0;
    const auto* const data = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    matter::TlvElement element;
    std::size_t offset = 0;
    unsigned items = 0;
    const int top = be_top(vm);
    if (data == nullptr || !matter::tlv_next(data, size, 0, element) ||
        !push_tlv(vm, data, size, offset, element, 0, items)) {
        be_pop(vm, be_top(vm) - top);
        be_return_nil(vm);
    }
    be_return(vm);
}

/** @brief matter.send(ctx, exchange, initiator, protocol, opcode, payload, reliable). */
int Send(bvm* vm) {
    std::size_t size = 0;
    const auto* const payload = static_cast<const std::uint8_t*>(be_tobytes(vm, 6, &size));
    be_pushbool(vm, matter::stack_send(be_toint(vm, 1), static_cast<std::uint16_t>(be_toint(vm, 2)),
                        be_tobool(vm, 3), static_cast<std::uint16_t>(be_toint(vm, 4)),
                        static_cast<std::uint8_t>(be_toint(vm, 5)), payload, size, be_tobool(vm, 7),
                        platform::monotonic_milliseconds()));
    be_return(vm);
}

int MaxMessage(bvm* vm) {
    be_pushint(vm, static_cast<bint>(matter::stack_max_message(be_toint(vm, 1))));
    be_return(vm);
}

/** @brief matter.transport(ctx): 0 UDP, 1 BTP, 2 TCP, negative for a context that is gone. */
int Transport(bvm* vm) {
    be_pushint(vm, matter::stack_transport(be_toint(vm, 1)));
    be_return(vm);
}

/** @brief matter.connect(ctx) opens TCP to that context's peer; gives the handshake context. */
int Connect(bvm* vm) {
    be_pushint(vm, matter::stack_connect(be_toint(vm, 1)));
    be_return(vm);
}

/** @brief matter.tcp_supported() tells DNS-SD and clients whether large messages can travel. */
int TcpSupported(bvm* vm) {
    be_pushbool(vm, USE_MATTER_TCP != 0);
    be_return(vm);
}

int NewSessionId(bvm* vm) {
    be_pushint(vm, matter::stack_new_session_id());
    be_return(vm);
}

int Now(bvm* vm) {
    be_pushint(vm, static_cast<bint>(platform::monotonic_milliseconds()));
    be_return(vm);
}

/** @brief matter.systime() gives the milliseconds since start as 8 little-endian bytes. */
int Systime(bvm* vm) {
    std::uint64_t value = systime();
    std::uint8_t bytes[8];
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
    be_pushbytes(vm, bytes, sizeof(bytes));
    be_return(vm);
}

/** @brief Copies a bytes argument into a fixed key or node field, never past either end. */
template <std::size_t N> void copy_field(bvm* const vm, const int index, std::uint8_t (&field)[N]) {
    std::size_t size = 0;
    const void* const data = be_tobytes(vm, index, &size);
    if (data != nullptr)
        std::memcpy(field, data, std::min(size, N));
}

/** @brief matter.session(from, local, peer, decrypt, encrypt, peer_node, local_node, fabric). */
int NewSession(bvm* vm) {
    matter::SessionParameters parameters;
    parameters.local_id = static_cast<std::uint16_t>(be_toint(vm, 2));
    parameters.peer_id = static_cast<std::uint16_t>(be_toint(vm, 3));
    copy_field(vm, 4, parameters.decrypt_key);
    copy_field(vm, 5, parameters.encrypt_key);
    copy_field(vm, 6, parameters.peer_node);
    copy_field(vm, 7, parameters.local_node);
    parameters.fabric = static_cast<std::uint8_t>(be_toint(vm, 8));
    be_pushint(vm, matter::stack_session_new(be_toint(vm, 1), parameters));
    be_return(vm);
}

/** @brief matter.session_export(ctx) gives the stored form of a session, or nil. */
int SessionExport(bvm* vm) {
    std::uint8_t record[matter::kSessionRecordSize];
    if (matter::stack_session_export(be_toint(vm, 1), record) == 0U)
        be_return_nil(vm);
    be_pushbytes(vm, record, sizeof(record));
    be_return(vm);
}

/** @brief matter.session_import(bytes) restores a session and gives its context, negative if not.
 */
int SessionImport(bvm* vm) {
    std::size_t size = 0;
    const auto* const record = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    be_pushint(vm, matter::stack_session_import(record, size, platform::monotonic_milliseconds()));
    be_return(vm);
}

/** @brief matter.output_revision() changes whenever a relay or light changes, whoever changed it.
 */
int OutputRevision(bvm* const vm) {
    be_pushint(
        vm, power_service != nullptr ? static_cast<bint>(power_service->state_revision()) : 0);
    be_return(vm);
}

#if USE_MATTER_ENERGY
/**
 * @brief matter.energy() gives a one-channel energy meter as [present, dc, mV, mA, mW, mVA, mvar,
 * mHz, factor 1/10000, imported mWh, exported mWh, exports], or nil without one. Bit n of present
 * marks item n + 2 as measured; the totals are 8-byte little-endian integers. exports is 1 when
 * the meter reports export, as ENERGY does with ExportActive.
 */
int Energy(bvm* const vm) {
    energy::MatterReading reading{};
    if (!energy::service().matter_reading(reading))
        be_return_nil(vm);
    const auto push = [vm](const std::int64_t value) {
        be_pushint(vm, static_cast<bint>(std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX)));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    };
    const auto push_total = [vm](std::uint64_t value) {
        std::uint8_t bytes[8];
        for (auto& byte : bytes) {
            byte = static_cast<std::uint8_t>(value);
            value >>= 8;
        }
        be_pushbytes(vm, bytes, sizeof(bytes));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    };
    be_newobject(vm, "list");
    push(reading.present);
    push(reading.direct_current);
    for (const auto value : {reading.voltage, reading.current, reading.power, reading.apparent,
             reading.reactive, reading.frequency, reading.factor})
        push(value);
    push_total(reading.imported_mwh);
    push_total(reading.exported_mwh);
    push(reading.exports);
    be_pop(vm, 1);
    be_return(vm);
}
#endif

/** @brief matter.sensor_revision() changes whenever a sensor reports a new value. */
int SensorRevision(bvm* const vm) {
#if USE_SENSORS
    be_pushint(vm, static_cast<bint>(sensor_service().change_revision()));
#else
    be_pushint(vm, 0);
#endif
    be_return(vm);
}

int SessionsDirty(bvm* vm) {
    be_pushint(vm, static_cast<bint>(matter::stack_take_dirty_fabrics()));
    be_return(vm);
}

int NetworkReady(bvm* vm) {
    be_pushbool(vm, matter::stack_network_ready());
    be_return(vm);
}

/** @brief matter.ipv6_addresses() lists the link's addresses as 16-byte values. */
int Ipv6Addresses(bvm* vm) {
    const matter::DnsRecords& records = matter::stack_addresses();
    be_newobject(vm, "list");
    for (std::size_t index = 0; index < records.address_count; ++index) {
        be_pushbytes(vm, records.addresses[index].data(), records.addresses[index].size());
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    be_pop(vm, 1);
    be_return(vm);
}

/**
 * @brief matter.tlv_items(value) splits a tagged array into its items, each with context tag 2 for
 * use in a report. Gives nil for anything else. The value is built by this firmware.
 */
int TlvItems(bvm* vm) {
    constexpr unsigned kMaxItems = 16;
    std::size_t size = 0;
    const auto* const data = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    matter::TlvElement top;
    if (data == nullptr || !matter::tlv_next(data, size, 0, top) ||
        top.type != matter::TlvType::array)
        be_return_nil(vm);
    auto* const buffer = static_cast<std::uint8_t*>(std::malloc(matter::kMaxFrameSize));
    if (buffer == nullptr)
        be_return_nil(vm);
    be_newobject(vm, "list");
    std::size_t offset = top.next;
    bool valid = false;
    for (unsigned count = 0; count <= kMaxItems; ++count) {
        matter::TlvElement item;
        if (!matter::tlv_next(data, size, offset, item))
            break;
        if (item.type == matter::TlvType::end) {
            valid = true;
            break;
        }
        const std::size_t start = offset;
        const std::size_t tagged = matter::tlv_skip(data, size, offset, item)
                                       ? matter::tlv_with_context_tag(data + start, offset - start,
                                             2, buffer, matter::kMaxFrameSize)
                                       : 0U;
        if (tagged == 0U || count == kMaxItems)
            break;
        be_pushbytes(vm, buffer, tagged);
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    std::free(buffer);
    if (!valid)
        be_return_nil(vm);
    be_pop(vm, 1);
    be_return(vm);
}

int RemoveSession(bvm* vm) {
    matter::stack_session_remove(be_toint(vm, 1));
    be_return_nil(vm);
}

int RemoveFabric(bvm* vm) {
    matter::stack_fabric_remove(static_cast<std::uint8_t>(be_toint(vm, 1)));
    be_return_nil(vm);
}

/** @brief Copies a Berry string, truncating to the destination. */
template <std::size_t N> void copy_text(char (&destination)[N], const char* const source) {
    std::snprintf(destination, N, "%s", source);
}

int MdnsHost(bvm* vm) {
    copy_text(matter::stack_dns_records().host, be_tostring(vm, 1));
    be_return_nil(vm);
}

int MdnsClear(bvm* vm) {
    matter::stack_dns_records().service_count = 0;
    be_return_nil(vm);
}

/** @brief matter.mdns_add(instance, commissionable, "sub,sub", "key=value\\n..."). */
int MdnsAdd(bvm* vm) {
    matter::DnsRecords& records = matter::stack_dns_records();
    if (records.service_count >= matter::kDnsServices)
        be_return_nil(vm);
    matter::DnsService& service = records.services[records.service_count++];
    service = {};
    copy_text(service.instance, be_tostring(vm, 1));
    service.commissionable = be_tobool(vm, 2);
    copy_text(service.txt, be_tostring(vm, 4));
    const std::string_view subtypes{be_tostring(vm, 3)};
    std::size_t start = 0;
    for (auto& subtype : service.subtypes) {
        if (start >= subtypes.size())
            break;
        std::size_t end = subtypes.find(',', start);
        if (end == std::string_view::npos)
            end = subtypes.size();
        std::snprintf(subtype, sizeof(subtype), "%.*s", static_cast<int>(end - start),
            subtypes.data() + start);
        start = end + 1U;
    }
    be_return_nil(vm);
}

int MdnsCommit(bvm* vm) {
    matter::stack_dns_changed();
    be_return_nil(vm);
}

/** @brief Builds a path inside the store from a name chosen by this firmware. */
[[nodiscard]] bool store_path(
    char (&path)[64], const char* const name, const char* const suffix = "") {
    const int length = std::snprintf(path, sizeof(path), "%.*s/%s%s",
        static_cast<int>(kStoreDirectory.size()), kStoreDirectory.data(), name, suffix);
    return length > 0 && static_cast<std::size_t>(length) < sizeof(path);
}

/** @brief matter.store_load(name) returns bytes, or nil when absent or above the bound. */
int StoreLoad(bvm* vm) {
    char path[64];
    if (!store_path(path, be_tostring(vm, 1)))
        be_return_nil(vm);
    auto* const buffer = static_cast<std::uint8_t*>(std::malloc(kMaxStoredFile));
    if (buffer == nullptr)
        be_return_nil(vm);
    std::size_t size = 0;
    const bool loaded =
        fs::read_file(path, buffer, kMaxStoredFile, size) == fs::ReadResult::succeeded;
    if (loaded)
        be_pushbytes(vm, buffer, size);
    std::free(buffer);
    if (!loaded)
        be_return_nil(vm);
    be_return(vm);
}

/** @brief matter.store_save(name, bytes) replaces a record through a temporary file. */
int StoreSave(bvm* vm) {
    char path[64];
    char temporary[64];
    std::size_t size = 0;
    const void* const data = be_tobytes(vm, 2, &size);
    const char* const name = be_tostring(vm, 1);
    const bool saved = data != nullptr && size <= kMaxStoredFile && store_path(path, name) &&
                       store_path(temporary, name, ".tmp") &&
                       fs::ensure_directory("/.newmota") != fs::MutationResult::failed &&
                       fs::ensure_directory(kStoreDirectory) != fs::MutationResult::failed &&
                       fs::write_file(temporary, data, size) == fs::MutationResult::succeeded &&
                       fs::rename(temporary, path) == fs::MutationResult::succeeded;
    be_pushbool(vm, saved);
    be_return(vm);
}

/** @brief matter.ble_start(discriminator, vendor, product) advertises as commissionable. */
int BleStart(bvm* vm) {
#if USE_MATTER_BLE
    be_pushbool(vm, matter::ble_link_start(static_cast<std::uint16_t>(be_toint(vm, 1)),
                        static_cast<std::uint16_t>(be_toint(vm, 2)),
                        static_cast<std::uint16_t>(be_toint(vm, 3))));
#else
    be_pushbool(vm, false);
#endif
    be_return(vm);
}

int BleStop(bvm* vm) {
#if USE_MATTER_BLE
    matter::ble_link_stop();
#endif
    be_return_nil(vm);
}

/** @brief matter.ble_supported() tells the QR code which discovery the device offers. */
int BleSupported(bvm* vm) {
    be_pushbool(vm, USE_MATTER_BLE != 0);
    be_return(vm);
}

int WifiConnected(bvm* vm) {
    be_pushbool(vm, platform::wifi_snapshot().station == platform::WifiState::connected);
    be_return(vm);
}

int WifiScanStart(bvm* vm) {
    be_pushbool(vm, platform::wifi_scan_start() == platform::NetworkResult::success);
    be_return(vm);
}

/** @brief Maps a platform encryption to the Matter security bitmap. */
[[nodiscard]] int wifi_security(const platform::WifiEncryption encryption) {
    using platform::WifiEncryption;
    switch (encryption) {
    case WifiEncryption::open:
        return 0x01;
    case WifiEncryption::wep:
        return 0x02;
    case WifiEncryption::wpa_psk:
        return 0x04;
    case WifiEncryption::wpa2_psk:
    case WifiEncryption::wpa_wpa2_psk:
        return 0x08;
    case WifiEncryption::wpa3_psk:
    case WifiEncryption::wpa2_wpa3_psk:
        return 0x10;
    default:
        return 0;
    }
}

/** @brief matter.wifi_scan_results() gives a list of [security, ssid, bssid, channel, rssi] or nil.
 */
int WifiScanResults(bvm* vm) {
    auto* const records = static_cast<platform::WifiScanRecord*>(
        std::malloc(sizeof(platform::WifiScanRecord) * kMaxScanResults));
    if (records == nullptr)
        be_return_nil(vm);
    std::size_t count = 0;
    const auto result = platform::wifi_scan_poll(records, kMaxScanResults, count);
    if (result == platform::NetworkResult::busy) {
        std::free(records);
        be_return_nil(vm);
    }
    be_newobject(vm, "list");
    for (std::size_t index = 0; index < count; ++index) {
        const platform::WifiScanRecord& record = records[index];
        be_newobject(vm, "list");
        be_pushint(vm, wifi_security(record.encryption));
        be_data_push(vm, -2);
        be_pop(vm, 1);
        be_pushbytes(vm, record.ssid.data(), record.ssid_length);
        be_data_push(vm, -2);
        be_pop(vm, 1);
        be_pushbytes(vm, record.bssid.data(), record.bssid.size());
        be_data_push(vm, -2);
        be_pop(vm, 1);
        be_pushint(vm, record.channel);
        be_data_push(vm, -2);
        be_pop(vm, 1);
        be_pushint(vm, record.rssi);
        be_data_push(vm, -2);
        be_pop(vm, 1);
        be_pop(vm, 1);
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    be_pop(vm, 1);
    std::free(records);
    be_return(vm);
}

/** @brief Views a bytes argument as text, for an SSID or a passphrase. */
[[nodiscard]] std::string_view text_view(bvm* const vm, const int index) {
    std::size_t size = 0;
    const void* const data = be_tobytes(vm, index, &size);
    return data != nullptr ? std::string_view{static_cast<const char*>(data), size}
                           : std::string_view{};
}

/** @brief matter.wifi_test(ssid, password) tries credentials without storing them. */
int WifiTest(bvm* vm) {
    constexpr std::uint16_t kTestOnly = 3;
    be_pushbool(
        vm, network_service != nullptr &&
                network_service->start_wifi_test(text_view(vm, 1), text_view(vm, 2), kTestOnly,
                    platform::monotonic_milliseconds()) == platform::NetworkResult::success);
    be_return(vm);
}

/** @brief matter.wifi_test_state() gives 0 idle, 1 testing, 2 succeeded or 3 failed. */
int WifiTestState(bvm* vm) {
    be_pushint(
        vm, network_service != nullptr ? static_cast<bint>(network_service->wifi_test_state()) : 3);
    be_return(vm);
}

/** @brief matter.wifi_auth_failed() tells whether the station was refused for its credentials. */
int WifiAuthFailed(bvm* vm) {
    be_pushbool(vm, platform::wifi_auth_failed());
    be_return(vm);
}

/** @brief matter.build_passcode() gives the passcode fixed by the build, or nil. */
int BuildPasscode(bvm* vm) {
#ifdef NEWMOTA_MATTER_PASSCODE
    be_pushint(vm, NEWMOTA_MATTER_PASSCODE);
    be_return(vm);
#else
    be_return_nil(vm);
#endif
}

/** @brief matter.build_discriminator() gives the discriminator fixed by the build, or nil. */
int BuildDiscriminator(bvm* vm) {
#ifdef NEWMOTA_MATTER_DISCRIMINATOR
    be_pushint(vm, NEWMOTA_MATTER_DISCRIMINATOR);
    be_return(vm);
#else
    be_return_nil(vm);
#endif
}

/** @brief matter.wifi_commit(ssid, password) stores credentials that a test proved. */
int WifiCommit(bvm* vm) {
    be_pushbool(vm, network_service != nullptr && network_service->commit_wifi_credentials(
                                                      text_view(vm, 1), text_view(vm, 2)));
    be_return(vm);
}

int PeerActiveInterval(bvm* vm) {
    matter::stack_peer_active_interval(
        be_toint(vm, 1), static_cast<std::uint32_t>(be_toint(vm, 2)));
    be_return_nil(vm);
}

/** @brief matter.cert_tbs(tlv) gives the DER a certificate's signature covers, or nil if bad. */
int CertSignedData(bvm* vm) {
    std::size_t size = 0;
    const auto* const certificate = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    auto* const buffer = static_cast<std::uint8_t*>(std::malloc(matter::kMaxCertificateDer));
    if (certificate == nullptr || buffer == nullptr) {
        std::free(buffer);
        be_return_nil(vm);
    }
    const std::uint8_t* der = nullptr;
    const std::size_t length = matter::certificate_signed_data(
        certificate, size, buffer, matter::kMaxCertificateDer, &der);
    if (length != 0U)
        be_pushbytes(vm, der, length);
    std::free(buffer);
    if (length == 0U)
        be_return_nil(vm);
    be_return(vm);
}

/** @brief matter.cert_cats(tlv) lists the CASE Authenticated Tags of a certificate's subject. */
int CertCats(bvm* vm) {
    std::size_t size = 0;
    const auto* const certificate = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    std::uint32_t cats[3] = {};
    const std::size_t count =
        certificate != nullptr ? matter::certificate_cats(certificate, size, cats, 3) : 0U;
    be_newobject(vm, "list");
    for (std::size_t index = 0; index < count; ++index) {
        be_pushint(vm, static_cast<bint>(cats[index]));
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    be_pop(vm, 1);
    be_return(vm);
}

int StoreRemove(bvm* vm) {
    char path[64];
    be_pushbool(vm, store_path(path, be_tostring(vm, 1)) &&
                        fs::remove_if_exists(path) != fs::MutationResult::failed);
    be_return(vm);
}
} // namespace

void matter_module_bind_network(NetworkService* const network) {
    network_service = network;
}

void matter_module_bind_power(const PowerService* const power) {
    power_service = power;
}

void matter_module_enable(const bool value) {
    enabled = value;
}

bool matter_module_pending(const bool timer_tick) {
    return enabled && timer_tick;
}

int dispatch_matter_events(bvm* const vm) {
    const std::uint32_t now = platform::monotonic_milliseconds();
    systime();
    if (!started) {
        // Sensors are usually up by the time Wi-Fi is. Without Wi-Fi, BLE must not wait for it.
        const auto station = platform::wifi_snapshot().station;
        const bool without_wifi =
            USE_MATTER_BLE != 0 && station != platform::WifiState::disabled &&
            static_cast<std::int32_t>(now - kStartWithoutWifiMilliseconds) >= 0;
        if (station != platform::WifiState::connected && !without_wifi)
            be_return_nil(vm);
        started = true; // A failed start logs its error once instead of repeating every tick.
        if (!matter::stack_start())
            be_raise(vm, "runtime_error", "Matter stack: no memory");
        be_getmodule(vm, "matter");
        be_setglobal(vm, "matter");
        be_pop(vm, 1);
        be_pushntvclass(vm, &be_class_Matter_Device);
        be_call(vm, 0);
        be_setglobal(vm, "_matter");
        be_pop(vm, 1);
    }
    if (!be_getglobal(vm, "_matter") || be_isnil(vm, -1))
        be_return_nil(vm);
    const int device = be_absindex(vm, -1);
    matter::stack_poll(now);
    for (unsigned count = 0; count < kMessagesPerTick; ++count) {
        matter::Inbound message;
        if (!matter::stack_next(now, message))
            break;
        be_getmember(vm, device, "message");
        be_pushvalue(vm, device);
        be_pushint(vm, message.context);
        be_pushint(vm, message.exchange);
        be_pushbool(vm, message.peer_initiator);
        be_pushint(vm, message.protocol);
        be_pushint(vm, message.opcode);
        be_pushbytes(vm, message.payload, message.size);
        be_call(vm, 7);
        be_pop(vm, 8);
    }
    if (static_cast<std::int32_t>(now - next_tick) >= 0) {
        next_tick = now + kTickMilliseconds;
        be_getmember(vm, device, "tick");
        be_pushvalue(vm, device);
        be_pushint(vm, static_cast<bint>(now));
        be_call(vm, 2);
        be_pop(vm, 3);
    }
    be_return_nil(vm);
}
} // namespace newmota

extern "C" {
be_native_module_attr_table(matter){
    be_native_module_function("member", newmota::Member),
    be_native_module_function("tlv", newmota::Tlv),
    be_native_module_function("send", newmota::Send),
    be_native_module_function("max_message", newmota::MaxMessage),
    be_native_module_function("transport", newmota::Transport),
    be_native_module_function("connect", newmota::Connect),
    be_native_module_function("tcp_supported", newmota::TcpSupported),
    be_native_module_function("new_session_id", newmota::NewSessionId),
    be_native_module_function("now", newmota::Now),
    be_native_module_function("systime", newmota::Systime),
    be_native_module_function("session", newmota::NewSession),
    be_native_module_function("session_remove", newmota::RemoveSession),
    be_native_module_function("session_export", newmota::SessionExport),
    be_native_module_function("session_import", newmota::SessionImport),
    be_native_module_function("output_revision", newmota::OutputRevision),
    be_native_module_function("sensor_revision", newmota::SensorRevision),
#if USE_MATTER_ENERGY
    be_native_module_function("energy", newmota::Energy),
#endif
    be_native_module_function("sessions_dirty", newmota::SessionsDirty),
    be_native_module_function("network_ready", newmota::NetworkReady),
    be_native_module_function("ipv6_addresses", newmota::Ipv6Addresses),
    be_native_module_function("tlv_items", newmota::TlvItems),
    be_native_module_function("fabric_remove", newmota::RemoveFabric),
    be_native_module_function("mdns_host", newmota::MdnsHost),
    be_native_module_function("mdns_clear", newmota::MdnsClear),
    be_native_module_function("mdns_add", newmota::MdnsAdd),
    be_native_module_function("mdns_commit", newmota::MdnsCommit),
    be_native_module_function("store_load", newmota::StoreLoad),
    be_native_module_function("store_save", newmota::StoreSave),
    be_native_module_function("store_remove", newmota::StoreRemove),
    be_native_module_function("peer_active_interval", newmota::PeerActiveInterval),
    be_native_module_function("cert_tbs", newmota::CertSignedData),
    be_native_module_function("cert_cats", newmota::CertCats),
    be_native_module_function("ble_start", newmota::BleStart),
    be_native_module_function("ble_stop", newmota::BleStop),
    be_native_module_function("ble_supported", newmota::BleSupported),
    be_native_module_function("wifi_connected", newmota::WifiConnected),
    be_native_module_function("wifi_scan_start", newmota::WifiScanStart),
    be_native_module_function("wifi_scan_results", newmota::WifiScanResults),
    be_native_module_function("wifi_test", newmota::WifiTest),
    be_native_module_function("wifi_test_state", newmota::WifiTestState),
    be_native_module_function("wifi_auth_failed", newmota::WifiAuthFailed),
    be_native_module_function("build_passcode", newmota::BuildPasscode),
    be_native_module_function("build_discriminator", newmota::BuildDiscriminator),
    be_native_module_function("wifi_commit", newmota::WifiCommit),
};
extern const bntvmodule_t be_native_module_matter = {
    "matter", matter_attrs, sizeof(matter_attrs) / sizeof(matter_attrs[0]), nullptr};
}
