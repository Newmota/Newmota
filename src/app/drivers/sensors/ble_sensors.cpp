/*
  ble_sensors.cpp - Native BLE sensor decoding

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Packet formats, device table and configuration file follow Tasmota
  xsns_62_esp32_mi.ino, Copyright (C) 2021 Christian Baars and Theo Arends,
  GPL version 3 or later.
*/
/** @file ble_sensors.cpp
 * @brief Decodes MiBeacon, BTHome, Qingping CGD1 and Mi scale advertisements and generic
 * sightings into a bounded registry, read through the sensor visitor.
 *
 * Advertisements arrive in the cooperative loop through the shared take point. The registry
 * holds every admitted device and the dashboard walks all of them; only kVisible reach the
 * sensor visitor, and the sensor service leaves out what would not fit the sensor document.
 * Values expire after kExpiry.
 */
#include "drivers/sensors/ble_sensors.h"
#include "drivers/sensors/sensor_service.h"
#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "services/ble/ble_advertisements.h"
#if USE_TIME
#include "services/time/time_service.h"
#endif
#include "support/json.h"
#include "support/number.h"
#include "support/text.h"
#include "bearssl.h"
#include "bearssl_newmota_config.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace newmota {
namespace {
constexpr std::size_t kCapacity = 32; // Tasmota's MI32_CFG_MAX_SENSORS.
constexpr std::size_t kVisible = 8;   // Devices reaching the sensor visitor.
constexpr std::size_t kKeys = 16;     // Bind keys or IRKs; only keyed devices use one.
constexpr std::size_t kNames = 16;    // Configured or advertised names.
constexpr std::uint32_t kExpiry = 60U * 60U * 1000U;
// Per-field stamps count 65.536 s ticks (the top 16 bits of the millisecond clock), so they wrap
// with it; 55 ticks are one hour.
constexpr std::uint16_t kExpiryTicks = 55;
constexpr std::uint32_t kScaleSession = 10000;
constexpr std::uint32_t kEventRetention = 10000; // Undelivered events older than this are dropped.
constexpr std::size_t kConfigMaximum = 2 + kCapacity * 139; // Tasmota's MI32_CFG_MAX_SIZE.
constexpr const char* kConfigPath = "/mi32cfg";

// Tasmota's MI32 feature bits, kept so `feat` in mi32cfg stays interchangeable with Tasmota.
enum Feature : std::uint32_t {
    needs_key = 1U << 0,
    temperature = 1U << 1,
    humidity = 1U << 2,
    temperature_humidity = 1U << 3,
    illuminance = 1U << 4,
    moisture = 1U << 5,
    conductivity = 1U << 6,
    battery = 1U << 7,
    no_motion_time = 1U << 8,
    motion = 1U << 9,
    button = 1U << 10,
    knob = 1U << 11,
    door = 1U << 12,
    leak = 1U << 13,
    raw_payload = 1U << 14,
    weight = 1U << 15,
    impedance = 1U << 16,
};

constexpr std::uint32_t kClimate = temperature | humidity | temperature_humidity | battery;
// Event-only bits above the feature bits (Tasmota's eventType).
constexpr std::uint32_t kNoMotion = 1U << 17, kHold = 1U << 18, kImpedanceFailed = 1U << 19;

enum Type : std::uint8_t {
    unknown,
    flora,
    mj_ht_v1,
    lywsd02,
    lywsd03,
    cgg1,
    cgd1,
    nlight,
    mjyd2s,
    ylyk01,
    mhoc401,
    mhoc303,
    mccgq02,
    sjws01l,
    ylkg08,
    ylai003,
    bthome,
    mibcs,
};

struct TypeInfo {
    std::uint16_t pid;
    const char* name;
    std::uint32_t features;
};

// ATC (0x0A1C) and PVVX (0x944A) are dropped in favor of BTHome.
constexpr std::array<TypeInfo, 18> kTypes{{
    {0x0000, "", 0},
    {0x0098, "Flora", temperature | moisture | conductivity | illuminance | battery},
    {0x01AA, "MJ_HT_V1", kClimate},
    {0x045B, "LYWSD02", kClimate},
    {0x055B, "LYWSD03", kClimate},
    {0x0347, "CGG1", kClimate},
    {0x0576, "CGD1", kClimate},
    {0x03DD, "NLIGHT", motion | no_motion_time},
    {0x07F6, "MJYD2S", motion | no_motion_time | illuminance | battery},
    {0x0153, "YLYK01", button},
    {0x0387, "MHOC401", kClimate},
    {0x06D3, "MHOC303", kClimate},
    {0x098B, "MCCGQ02", battery | door},
    {0x0863, "SJWS01L", leak | battery | button},
    {0x03B6, "YLKG08", button | knob},
    {0x07BF, "YLAI003", button},
    {0xB770, "BTHOME", 0},                 // Fake ID.
    {0x181B, "MIBCS", weight | impedance}, // Fake ID: the service UUID.
}};

enum Status : std::uint8_t {
    has_counter = 1U << 0,
    wrong_key = 1U << 1,
    unbound = 1U << 2,
    configured = 1U << 3,
    has_sequence = 1U << 4, // an encrypted BTHome counter was accepted
};

enum ScaleState : std::uint8_t {
    weight_sent = 1U << 0,
    impedance_sent = 1U << 1,
    failed_sent = 1U << 2,
    finished = 1U << 3,
};

struct Device {
    std::uint8_t mac[6]; // Display order.
    std::uint8_t address_type;
    std::uint8_t type;
    std::uint16_t pid;
    std::uint8_t counter, status, scale_state, weight_unit;
    std::int8_t rssi, key, name;
    std::uint32_t features, valid; // Feature bits ever reported / currently held.
    std::uint32_t seen, no_motion_at;
    std::array<std::uint16_t, 17> stamps; // Tick of the last update, per feature bit.
    std::uint32_t illuminance, no_motion;
    std::int16_t temperature;                      // 0.01 °C
    std::uint16_t humidity;                        // 0.01 %
    std::uint16_t conductivity, impedance, weight; // µS/cm, ohm, 0.01 of the reported unit
    std::uint8_t moisture, battery, door, leak;
    std::uint32_t events;      // Bits changed since the last event message (Tasmota's eventType).
    std::uint32_t trigger_at;  // When an immediate message was requested; 0 when none.
    std::uint16_t event_count; // Motion and door events since boot.
    std::uint8_t button, button_type, hold, dimmer_pressed;
    std::int8_t dimmer;
    std::uint32_t sequence; // Last accepted encrypted BTHome counter.
};

struct Options {
    bool aggregate{true}; // MI32Option0, used by event publication.
    bool no_summary{};    // MI32Option1
    bool direct_bridge{}; // MI32Option2, used by event publication.
    bool closed{};        // MI32Option3: no new devices.
    bool every_device{};  // MI32Option5
};

std::array<Device, kCapacity> devices{};
std::size_t device_count{};
std::array<std::array<std::uint8_t, 16>, kKeys> keys{};
std::array<bool, kKeys> key_used{};
std::array<std::array<char, kSensorNameCapacity>, kNames> names{};
std::array<bool, kNames> name_used{};
std::array<std::uint8_t, kVisible> visible{};
std::size_t visible_count{}, hidden_logged{};
#if USE_TIME
const TimeService* time_source{};
#endif
bool irk_configured{};
Options options{};

std::uint8_t type_for(std::uint16_t pid) {
    for (std::size_t i = 1; i < kTypes.size(); ++i)
        if (kTypes[i].pid == pid)
            return static_cast<std::uint8_t>(i);
    return unknown;
}

std::uint16_t le16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | data[1] << 8);
}

std::uint32_t le24(const std::uint8_t* data) {
    return data[0] | std::uint32_t(data[1]) << 8 | std::uint32_t(data[2]) << 16;
}

int nibble(char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    character = support::ascii_lower(character);
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

/** @brief Parses exactly 2 * @p size hex digits. */
bool hex_bytes(std::string_view text, std::uint8_t* output, std::size_t size) {
    if (text.size() != size * 2)
        return false;
    for (std::size_t i = 0; i < size; ++i) {
        const int high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        output[i] = static_cast<std::uint8_t>(high << 4 | low);
    }
    return true;
}

template <std::size_t N> int claim(std::array<bool, N>& used) {
    for (std::size_t i = 0; i < N; ++i)
        if (!used[i]) {
            used[i] = true;
            return static_cast<int>(i);
        }
    return -1;
}

/** @brief Stores a name, clamped to the sensor name limit, replacing the previous one. */
bool set_name(Device& device, std::string_view text) {
    if (device.name >= 0)
        name_used[device.name] = false;
    device.name = -1;
    if (text.empty())
        return true;
    const int slot = claim(name_used);
    if (slot < 0)
        return false;
    auto& name = names[slot];
    const auto length = std::min(text.size(), name.size() - 1);
    std::size_t out = 0;
    for (std::size_t i = 0; i < length; ++i)
        if (text[i] >= 0x20 && text[i] < 0x7F)
            name[out++] = text[i];
    name[out] = '\0';
    device.name = static_cast<std::int8_t>(slot);
    return true;
}

bool set_key(Device& device, const std::uint8_t* key) {
    if (device.key < 0) {
        const int slot = claim(key_used);
        if (slot < 0)
            return false;
        device.key = static_cast<std::int8_t>(slot);
    }
    std::memcpy(keys[device.key].data(), key, 16);
    device.status &= ~wrong_key;
    if (device.pid == 0)
        irk_configured = true; // A key on a generic entry resolves its random addresses.
    return true;
}

/** @brief Writes Tasmota's MI32 display name: configured, advertised or type plus MAC tail. */
void display_name(const Device& device, char* output) {
    if (device.name >= 0) {
        std::memcpy(output, names[device.name].data(), kSensorNameCapacity);
        return;
    }
    char base[8];
    if (device.type != unknown)
        std::snprintf(base, sizeof(base), "%s", kTypes[device.type].name);
    else if (device.pid != 0)
        std::snprintf(base, sizeof(base), "MI_%04X", unsigned(device.pid));
    else
        std::snprintf(base, sizeof(base), "BLE");
    std::snprintf(output, kSensorNameCapacity, "%s-%02x%02x%02x", base, device.mac[3],
        device.mac[4], device.mac[5]);
}

Device* find(const std::uint8_t* mac) {
    for (std::size_t i = 0; i < device_count; ++i)
        if (std::memcmp(devices[i].mac, mac, 6) == 0)
            return &devices[i];
    return nullptr;
}

/** @brief Finds or admits a device; admission stops when closed or full. */
Device* admit(const std::uint8_t* mac, std::uint8_t address_type, std::uint16_t pid) {
    if (auto* device = find(mac))
        return device;
    if (options.closed || device_count == kCapacity)
        return nullptr;
    auto& device = devices[device_count++];
    device = {};
    std::memcpy(device.mac, mac, 6);
    device.address_type = address_type;
    device.pid = pid;
    device.type = type_for(pid);
    device.features = kTypes[device.type].features;
    device.key = device.name = -1;
    char name[kSensorNameCapacity];
    display_name(device, name);
    AddLog(LogLevel::debug, "M32: new %s at slot %u", name, unsigned(device_count - 1));
    return &device;
}

void touch(Device& device, const platform::BleAdvertisement& advertisement, std::uint32_t now) {
    device.rssi = advertisement.rssi;
    device.seen = now == 0 ? 1 : now;
    sensor_service().mark_device_dirty(std::size_t(&device - devices.data()));
}

/** @brief Requests an immediate event message for @p device. */
void trigger(Device& device, std::uint32_t now) {
    if (device.trigger_at == 0)
        device.trigger_at = now == 0 ? 1 : now;
}

/** @brief Records new values; @p event marks them changed, which in direct-bridge mode
 *  (MI32Option2) also requests an immediate message.
 */
void store(Device& device, std::uint32_t bits, std::uint32_t now, bool event = true) {
    device.features |= bits;
    device.valid |= bits;
    for (unsigned bit = 0; bit < device.stamps.size(); ++bit)
        if (bits & (1U << bit))
            device.stamps[bit] = static_cast<std::uint16_t>(now >> 16);
    if (!event)
        return;
    device.events |= bits;
    if (options.direct_bridge)
        trigger(device, now);
}

/** @brief Records an event-only change and requests an immediate message. */
void event(Device& device, std::uint32_t bits, std::uint32_t now) {
    device.features |= bits & 0x1FFFFU; // event-only bits are not features
    device.events |= bits;
    trigger(device, now);
}

// ---- MiBeacon 0xFE95

/** @brief Decrypts a v3 or v5 MiBeacon object into @p payload.
 *  @return 0, -1 for a wrong key, -2 without key, -3 for a malformed frame.
 */
int decrypt(const Device& device, const std::uint8_t* data, std::size_t size, std::uint8_t* payload,
    std::size_t& length) {
    if (device.key < 0)
        return -2;
    constexpr std::size_t kPayload = 19;
    const unsigned version = le16(data) >> 12;
    const bool includes_mac = (data[0] & 0x10) != 0;
    std::uint8_t nonce[13];
    std::size_t nonce_length = 12;
    std::uint8_t tag[4]{};
    if (version == 5) {
        const std::size_t offset = includes_mac ? 11 : 5;
        if (size < offset + 10 || size > offset + 7 + kPayload)
            return -3;
        length = size - offset - 7;
        std::memcpy(payload, data + offset, length);
        for (unsigned i = 0; i < 6; ++i)
            nonce[i] = includes_mac ? data[5 + i] : device.mac[5 - i];
        std::memcpy(nonce + 6, data + 2, 2); // product ID
        nonce[8] = data[4];                  // frame counter
        std::memcpy(nonce + 9, data + size - 7, 3);
        std::memcpy(tag, data + size - 4, 4);
    } else if (version == 3) {
        if (!includes_mac || size < 18 || size > 15 + kPayload)
            return -3;
        length = size - 15;
        std::memcpy(payload, data + 11, length);
        nonce_length = 13;
        std::memcpy(nonce, data, 2); // frame control
        std::memcpy(nonce + 2, data + 2, 2);
        nonce[4] = data[4];
        std::memcpy(nonce + 5, data + size - 4, 3);
        std::memcpy(nonce + 8, data + 5, 5);
    } else {
        return -3;
    }
    br_aes_small_ctrcbc_keys cipher;
    br_aes_small_ctrcbc_init(&cipher, keys[device.key].data(), 16);
    br_ccm_context ccm;
    br_ccm_init(&ccm, &cipher.vtable);
    constexpr std::uint8_t aad = 0x11;
    if (!br_ccm_reset(&ccm, nonce, nonce_length, 1, length, sizeof(tag)))
        return -3;
    br_ccm_aad_inject(&ccm, &aad, 1);
    br_ccm_flip(&ccm);
    br_ccm_run(&ccm, 0, payload, length);
    // v3 frames carry no tag; Tasmota accepts them when the object type looks valid.
    if (!br_ccm_check_tag(&ccm, tag) && !(version == 3 && payload[1] == 0x10))
        return -1;
    return std::size_t(payload[2]) + 3 > length ? -3 : 0;
}

void apply_object(Device& device, const std::uint8_t* payload, std::uint32_t now) {
    const auto object = le16(payload);
    const std::uint8_t size = payload[2];
    const std::uint8_t* value = payload + 3;
    switch (object) {
    case 0x1004:
        if (size >= 2 && static_cast<std::int16_t>(le16(value)) < 600) {
            device.temperature = static_cast<std::int16_t>(le16(value) * 10);
            store(device, temperature, now);
        }
        break;
    case 0x1006:
        if (size >= 2 && le16(value) < 1010) {
            device.humidity = static_cast<std::uint16_t>(le16(value) * 10);
            store(device, humidity, now);
        }
        break;
    case 0x100D:
        if (size < 4)
            break;
        if (static_cast<std::int16_t>(le16(value)) < 600) {
            device.temperature = static_cast<std::int16_t>(le16(value) * 10);
            store(device, temperature, now);
        }
        if (le16(value + 2) < 1000) {
            device.humidity = static_cast<std::uint16_t>(le16(value + 2) * 10);
            store(device, humidity, now);
        }
        break;
    case 0x1007:
        if (size >= 3) {
            device.illuminance = le24(value);
            store(device, illuminance, now);
            if (device.type == mjyd2s) // MJYD2S reports light when motion has ended
                event(device, kNoMotion, now);
        }
        break;
    case 0x1008:
        if (size >= 1) {
            device.moisture = value[0];
            store(device, moisture, now);
        }
        break;
    case 0x1009:
        if (size >= 2) {
            device.conductivity = le16(value);
            store(device, conductivity, now);
        }
        break;
    case 0x100A:
    case 0x4803:
        // LYWSD03MMC and MHO-C401 advertise bogus battery values (Tasmota's ignoreBogusBattery).
        if (size >= 1 && value[0] <= 100 && device.type != lywsd03 && device.type != mhoc401) {
            device.battery = value[0];
            store(device, battery, now);
        }
        break;
    case 0x000F:
        if (size >= 3) {
            device.illuminance = le24(value);
            store(device, illuminance, now);
        }
        device.no_motion = 0;
        device.no_motion_at = now;
        ++device.event_count;
        store(device, no_motion_time, now);
        event(device, motion, now);
        break;
    case 0x1014:
        if (size >= 1) {
            device.leak = value[0];
            store(device, leak, now);
            if (device.leak != 0)
                trigger(device, now);
        }
        break;
    case 0x1017:
        if (size >= 4) {
            device.no_motion = value[0] | std::uint32_t(le24(value + 1)) << 8;
            device.no_motion_at = now;
            store(device, no_motion_time, now);
            trigger(device, now);
        }
        break;
    case 0x1019:
        if (size >= 1) {
            device.door = value[0];
            ++device.event_count;
            store(device, door, now);
            trigger(device, now);
        }
        break;
    case 0x1001:
        if (size < 3)
            break;
        if (value[2] == 4) { // dimmer knob rotation, pressed when a number is given
            device.dimmer_pressed = value[0] != 0;
            device.dimmer = static_cast<std::int8_t>(value[0] != 0 ? value[0] : value[1]);
            event(device, knob, now);
        } else if (value[0] == 1 && (device.features & knob)) { // knob held without rotation
            device.hold = value[1];
            event(device, kHold, now);
        } else {
            device.button = value[0];
            device.button_type =
                static_cast<std::uint8_t>((device.features & knob) ? value[1] - 1 : value[2]);
            event(device, button, now);
        }
        break;
    case 0x4E0C:
    case 0x4E0D:
    case 0x4E0E:
        device.button = size; // the button number is carried in the size byte
        device.button_type = static_cast<std::uint8_t>(object - 0x4E0C);
        event(device, button, now);
        break;
    case 0x0002:
        break;
    default:
        if (device.type == nlight) { // NLIGHT reports motion with an unlisted object.
            device.no_motion = 0;
            device.no_motion_at = now;
            ++device.event_count;
            store(device, no_motion_time, now);
            event(device, motion, now);
        }
        break;
    }
}

void decode_mibeacon(const std::uint8_t* data, std::size_t size,
    const platform::BleAdvertisement& advertisement, std::uint32_t now) {
    if (size < 9) // NLIGHT sends the shortest frame.
        return;
    auto* device = admit(advertisement.address, advertisement.address_type, le16(data + 2));
    if (device == nullptr)
        return;
    touch(*device, advertisement, now);
    const std::uint8_t counter = data[4];
    if ((device->status & has_counter) && counter != 0 && device->counter == counter)
        return;
    device->counter = counter;
    device->status |= has_counter;
    const auto frame = le16(data);
    if ((frame & 0x40) == 0) { // no object
        if (size > 11 && data[11] == 0x28)
            device->status |= unbound;
        return;
    }
    std::uint8_t payload[19]{};
    std::size_t length = 0;
    if (frame & 0x08) {
        device->features |= needs_key;
        const int result = decrypt(*device, data, size, payload, length);
        if (result == -1)
            device->status |= wrong_key;
        if (result != 0)
            return;
        device->status &= ~wrong_key;
    } else {
        const std::size_t offset = 5 + ((frame & 0x10) ? 6 : 0) + ((frame & 0x20) ? 1 : 0);
        if (size < offset + 3)
            return;
        length = std::size_t(data[offset + 2]) + 3;
        if (length > sizeof(payload) || length > size - offset)
            return;
        std::memcpy(payload, data + offset, length);
    }
    apply_object(*device, payload, now);
}

// ---- BTHome v2 0xFCD2

/** @brief Object payload sizes from the BTHome v2 specification; 0 is unknown, 0xFF variable. */
std::uint8_t bthome_size(std::uint8_t id) {
    // clang-format off
    static constexpr std::uint8_t sizes[0x5E] = {
        1, 1, 2, 2, 3, 3, 2, 2, 2, 1, 3, 3, 2, 2, 2, 1,       // 0x00
        1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,       // 0x10
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,       // 0x20
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 2, 4, 2,       // 0x30
        2, 2, 3, 2, 2, 2, 1, 2, 2, 2, 2, 3, 4, 4, 4, 4,       // 0x40
        4, 2, 2, 0xFF, 0xFF, 4, 2, 1, 1, 1, 2, 4, 4, 2,       // 0x50
    };
    // clang-format on
    if (id < sizeof(sizes))
        return sizes[id];
    return id == 0xF0 ? 2 : id == 0xF1 ? 4 : id == 0xF2 ? 3 : 0;
}

/** @brief Where a decoded BTHome object goes. */
enum class Target : std::uint8_t {
    packet,  // packet ID, for deduplication
    battery, // Tasmota's MI32 device fields, shared with MiBeacon
    temperature,
    humidity,
    illuminance,
    moisture,
    conductivity,
    weight_kg,
    weight_lb,
    pool,   // other measurements and binary states
    button, // events
    dimmer,
};

struct BthomeObject {
    std::uint8_t id;
    bool is_signed;
    Target target;
    SensorQuantity quantity; // pool targets only
    std::int32_t milli;      // milli-units of the quantity per raw step
    const char* field;       // names a pooled value; objects sharing a name are numbered together
};

// Decoded objects; any other object of known size is skipped.
// clang-format off
constexpr BthomeObject kBthome[] = {
    {0x00, false, Target::packet, SensorQuantity::count, 1, "Packet"},
    {0x01, false, Target::battery, SensorQuantity::battery, 1000, "Battery"},
    {0x02, true, Target::temperature, SensorQuantity::temperature, 10, "Temperature"},
    {0x03, false, Target::humidity, SensorQuantity::humidity, 10, "Humidity"},
    {0x04, false, Target::pool, SensorQuantity::pressure, 10, "Pressure"},
    {0x05, false, Target::illuminance, SensorQuantity::illuminance, 10, "Illuminance"},
    {0x06, false, Target::weight_kg, SensorQuantity::weight, 10, "Weight"},
    {0x07, false, Target::weight_lb, SensorQuantity::weight, 10, "Weight"},
    {0x09, false, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x0A, false, Target::pool, SensorQuantity::energy, 1, "Energy"},
    {0x0B, false, Target::pool, SensorQuantity::power, 10, "Power"},
    {0x0C, false, Target::pool, SensorQuantity::voltage, 1, "Voltage"},
    {0x0D, false, Target::pool, SensorQuantity::particulate, 1000, "PM2.5"},
    {0x0E, false, Target::pool, SensorQuantity::particulate, 1000, "PM10"},
    {0x0F, false, Target::pool, SensorQuantity::state, 1000, "Generic"},
    {0x10, false, Target::pool, SensorQuantity::state, 1000, "PowerOn"},
    {0x11, false, Target::pool, SensorQuantity::state, 1000, "Opening"},
    {0x12, false, Target::pool, SensorQuantity::carbon_dioxide, 1000, "CarbonDioxide"},
    {0x13, false, Target::pool, SensorQuantity::volatile_organic, 1000, "TVOC"},
    {0x14, false, Target::moisture, SensorQuantity::moisture, 10, "Moisture"},
    {0x15, false, Target::pool, SensorQuantity::state, 1000, "BatteryLow"},
    {0x16, false, Target::pool, SensorQuantity::state, 1000, "Charging"},
    {0x17, false, Target::pool, SensorQuantity::state, 1000, "CO"},
    {0x18, false, Target::pool, SensorQuantity::state, 1000, "Cold"},
    {0x19, false, Target::pool, SensorQuantity::state, 1000, "Connectivity"},
    {0x1A, false, Target::pool, SensorQuantity::state, 1000, "Door"},
    {0x1B, false, Target::pool, SensorQuantity::state, 1000, "GarageDoor"},
    {0x1C, false, Target::pool, SensorQuantity::state, 1000, "Gas"},
    {0x1D, false, Target::pool, SensorQuantity::state, 1000, "Heat"},
    {0x1E, false, Target::pool, SensorQuantity::state, 1000, "Light"},
    {0x1F, false, Target::pool, SensorQuantity::state, 1000, "Lock"},
    {0x20, false, Target::pool, SensorQuantity::state, 1000, "Wet"},
    {0x21, false, Target::pool, SensorQuantity::state, 1000, "Motion"},
    {0x22, false, Target::pool, SensorQuantity::state, 1000, "Moving"},
    {0x23, false, Target::pool, SensorQuantity::state, 1000, "Occupancy"},
    {0x24, false, Target::pool, SensorQuantity::state, 1000, "Plug"},
    {0x25, false, Target::pool, SensorQuantity::state, 1000, "Presence"},
    {0x26, false, Target::pool, SensorQuantity::state, 1000, "Problem"},
    {0x27, false, Target::pool, SensorQuantity::state, 1000, "Running"},
    {0x28, false, Target::pool, SensorQuantity::state, 1000, "Safety"},
    {0x29, false, Target::pool, SensorQuantity::state, 1000, "Smoke"},
    {0x2A, false, Target::pool, SensorQuantity::state, 1000, "Sound"},
    {0x2B, false, Target::pool, SensorQuantity::state, 1000, "Tamper"},
    {0x2C, false, Target::pool, SensorQuantity::state, 1000, "Vibration"},
    {0x2D, false, Target::pool, SensorQuantity::state, 1000, "Window"},
    {0x2E, false, Target::humidity, SensorQuantity::humidity, 1000, "Humidity"},
    {0x2F, false, Target::moisture, SensorQuantity::moisture, 1000, "Moisture"},
    {0x3A, false, Target::button, SensorQuantity::count, 1, "Button"},
    {0x3C, false, Target::dimmer, SensorQuantity::count, 1, "Dimmer"},
    {0x3D, false, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x3E, false, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x40, false, Target::pool, SensorQuantity::distance, 1, "Distance"},
    {0x41, false, Target::pool, SensorQuantity::distance, 100, "Distance"},
    {0x43, false, Target::pool, SensorQuantity::current, 1, "Current"},
    {0x45, true, Target::temperature, SensorQuantity::temperature, 100, "Temperature"},
    {0x46, false, Target::pool, SensorQuantity::uv_index, 100, "UvIndex"},
    {0x4A, false, Target::pool, SensorQuantity::voltage, 100, "Voltage"},
    {0x4D, false, Target::pool, SensorQuantity::energy, 1, "Energy"},
    {0x56, false, Target::conductivity, SensorQuantity::conductivity, 1000, "Fertility"},
    {0x57, true, Target::temperature, SensorQuantity::temperature, 1000, "Temperature"},
    {0x58, true, Target::temperature, SensorQuantity::temperature, 350, "Temperature"},
    {0x59, true, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x5A, true, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x5B, true, Target::pool, SensorQuantity::count, 1000, "Count"},
    {0x5C, true, Target::pool, SensorQuantity::power, 10, "Power"},
    {0x5D, true, Target::pool, SensorQuantity::current, 1, "Current"},
};
// clang-format on

const BthomeObject* bthome_object(std::uint8_t id) {
    for (const auto& object : kBthome)
        if (object.id == id)
            return &object;
    return nullptr;
}

/** @brief Values of BTHome objects without a device field, or a device field's second object,
 *  shared by all devices. An entry is keyed by device, field name and occurrence, so objects
 *  that share a name (the 8- and 16-bit counts) number together.
 */
struct PoolEntry {
    std::uint8_t device, id, occurrence;
    bool changed;        // since the last event message
    std::int32_t value;  // milli-units, or the raw event for buttons and dimmers
    std::uint16_t stamp; // tick of the last update, as for the device fields
};

constexpr std::size_t kPool = 32;
std::array<PoolEntry, kPool> pool{};
std::size_t pool_count{};
bool pool_full_logged{};

bool is_event(const BthomeObject& object) {
    return object.target == Target::button || object.target == Target::dimmer;
}

/** @brief Finds or makes an entry. When full, an already published event, then the least
 *  recently updated value makes room, so no device is starved; a waiting event is kept.
 */
PoolEntry* pool_entry(
    std::size_t device, const BthomeObject& object, std::uint8_t occurrence, std::uint32_t now) {
    for (std::size_t i = 0; i < pool_count; ++i) {
        const auto* other = bthome_object(pool[i].id);
        if (pool[i].device == device && pool[i].occurrence == occurrence && other != nullptr &&
            std::strcmp(other->field, object.field) == 0) {
            pool[i].id = object.id;
            return &pool[i];
        }
    }
    PoolEntry* entry = pool_count < kPool ? &pool[pool_count++] : nullptr;
    if (entry == nullptr) {
        const auto tick = static_cast<std::uint16_t>(now >> 16);
        std::uint16_t oldest = 0;
        for (auto& candidate : pool) {
            const auto* other = bthome_object(candidate.id);
            const bool published_event = other != nullptr && is_event(*other) && !candidate.changed;
            const std::uint16_t age = static_cast<std::uint16_t>(tick - candidate.stamp);
            if (published_event) {
                entry = &candidate;
                break;
            }
            if (other != nullptr && !is_event(*other) && (entry == nullptr || age > oldest)) {
                entry = &candidate;
                oldest = age;
            }
        }
        if (entry == nullptr)
            return nullptr; // every slot holds an unpublished event
        if (!pool_full_logged) {
            AddLog(LogLevel::info, "M32: BTHome value pool full, reusing the oldest values");
            pool_full_logged = true;
        }
    }
    *entry = {static_cast<std::uint8_t>(device), object.id, occurrence, false, 0, 0};
    return entry;
}

void pool_remove(std::size_t index) {
    pool[index] = pool[--pool_count];
}

/** @brief Writes a pool entry's field name: events always numbered, values from the second. */
void pool_field(const PoolEntry& entry, char* output, std::size_t capacity) {
    const auto* object = bthome_object(entry.id);
    const char* name = object != nullptr ? object->field : "Value";
    if (entry.occurrence != 0 || object == nullptr || is_event(*object))
        std::snprintf(output, capacity, "%s%u", name, entry.occurrence + 1U);
    else
        std::snprintf(output, capacity, "%s", name);
}

/** @brief Reads a little-endian object; only signed objects are sign-extended. */
std::int64_t bthome_raw(const BthomeObject& object, const std::uint8_t* value, std::size_t size) {
    std::uint32_t raw = 0;
    for (std::size_t i = 0; i < size; ++i)
        raw |= std::uint32_t(value[i]) << (8 * i);
    if (!object.is_signed)
        return raw;
    if (size < 4 && (raw & (1U << (8 * size - 1))))
        raw |= ~0U << (8 * size);
    return static_cast<std::int32_t>(raw);
}

/** @brief Decrypts a BTHome v2 payload in place.
 *  @return The plaintext object length, or -1 for a wrong key or a malformed frame.
 */
int bthome_decrypt(const Device& device, const std::uint8_t* data, std::size_t size,
    std::uint8_t* plain, std::uint32_t& sequence) {
    // device info, objects, 4-byte counter, 4-byte tag
    if (size < 1 + 1 + 4 + 4 || size - 9 > 32)
        return -1;
    const std::size_t length = size - 9;
    std::uint8_t nonce[13];
    std::memcpy(nonce, device.mac, 6); // display order
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    nonce[8] = data[0];
    std::memcpy(nonce + 9, data + 1 + length, 4);
    sequence = data[1 + length] | std::uint32_t(data[2 + length]) << 8 |
               std::uint32_t(data[3 + length]) << 16 | std::uint32_t(data[4 + length]) << 24;
    std::memcpy(plain, data + 1, length);
    br_aes_small_ctrcbc_keys cipher;
    br_aes_small_ctrcbc_init(&cipher, keys[device.key].data(), 16);
    br_ccm_context ccm;
    br_ccm_init(&ccm, &cipher.vtable);
    if (!br_ccm_reset(&ccm, nonce, sizeof(nonce), 0, length, 4))
        return -1;
    br_ccm_flip(&ccm);
    br_ccm_run(&ccm, 0, plain, length);
    return br_ccm_check_tag(&ccm, data + 5 + length) ? static_cast<int>(length) : -1;
}

void decode_bthome(const std::uint8_t* data, std::size_t size,
    const platform::BleAdvertisement& advertisement, std::string_view name, std::uint32_t now) {
    if (size < 1 || (data[0] >> 5) != 2) // device information: version 2 only
        return;
    auto* device = admit(advertisement.address, advertisement.address_type, kTypes[bthome].pid);
    if (device == nullptr)
        return;
    touch(*device, advertisement, now);
    if (device->name < 0 && !name.empty())
        (void)set_name(*device, name);
    const std::uint8_t info = data[0];
    std::uint8_t plain[32];
    const std::uint8_t* objects = data + 1;
    std::size_t length = size - 1;
    if (info & 0x01) {
        device->features |= needs_key;
        if (device->key < 0)
            return;
        std::uint32_t sequence = 0;
        const int decrypted = bthome_decrypt(*device, data, size, plain, sequence);
        if (decrypted < 0) {
            device->status |= wrong_key;
            return;
        }
        device->status &= ~wrong_key;
        // Replay protection: the counter must increase.
        if ((device->status & has_sequence) && sequence <= device->sequence)
            return;
        device->sequence = sequence;
        device->status |= has_sequence;
        objects = plain;
        length = std::size_t(decrypted);
    } else if (device->key >= 0) {
        return; // a keyed device must not be downgraded to plaintext
    }
    const std::size_t slot = std::size_t(device - devices.data());
    // Occurrences count per field name, so objects sharing a name number together.
    std::array<const char*, 16> seen_names{};
    std::array<std::uint8_t, 16> seen_counts{};
    std::size_t seen = 0;
    const auto next_occurrence = [&](const char* field) -> std::uint8_t {
        for (std::size_t i = 0; i < seen; ++i)
            if (std::strcmp(seen_names[i], field) == 0)
                return seen_counts[i]++;
        if (seen < seen_names.size()) {
            seen_names[seen] = field;
            seen_counts[seen++] = 1;
        }
        return 0;
    };
    for (std::size_t index = 0; index < length;) {
        const std::uint8_t id = objects[index++];
        std::size_t object_size = bthome_size(id);
        if (object_size == 0xFF && index < length)
            object_size = std::size_t(objects[index++]);
        if (object_size == 0 || object_size > length - index)
            break; // unknown object: keep what was decoded
        const std::uint8_t* value = objects + index;
        index += object_size;
        const auto* object = bthome_object(id);
        if (object == nullptr || object_size > 4)
            continue;
        const std::uint8_t occurrence = next_occurrence(object->field);
        const std::int64_t raw = bthome_raw(*object, value, object_size);
        std::int64_t milli = raw * object->milli;
        if (object->target == Target::weight_lb) // hundredths of a pound to grams
            milli = raw * 45359237 / 10000000;
        auto target = object->target;
        if (target != Target::packet && !is_event(*object) && occurrence != 0)
            target = Target::pool; // a device field's second object, such as Temperature2
        switch (target) {
        case Target::packet:
            if ((device->status & has_counter) && device->counter == value[0])
                return;
            device->counter = value[0];
            device->status |= has_counter;
            break;
        case Target::battery:
            if (raw <= 100) {
                device->battery = static_cast<std::uint8_t>(raw);
                store(*device, battery, now);
            }
            break;
        case Target::temperature:
            device->temperature = static_cast<std::int16_t>(milli / 10);
            store(*device, temperature, now);
            break;
        case Target::humidity:
            device->humidity = static_cast<std::uint16_t>(milli / 10);
            store(*device, humidity, now);
            break;
        case Target::illuminance:
            device->illuminance = static_cast<std::uint32_t>(milli / 1000);
            store(*device, illuminance, now);
            break;
        case Target::moisture:
            device->moisture = static_cast<std::uint8_t>(std::min<std::int64_t>(milli / 1000, 100));
            store(*device, moisture, now);
            break;
        case Target::conductivity:
            device->conductivity = static_cast<std::uint16_t>(raw);
            store(*device, conductivity, now);
            break;
        case Target::weight_kg:
        case Target::weight_lb:
            device->weight = static_cast<std::uint16_t>(raw);
            device->weight_unit = object->target == Target::weight_lb ? 1 : 0;
            store(*device, weight, now);
            break;
        case Target::button:
        case Target::dimmer:
            if (raw == 0) // no event
                break;
            if (auto* entry = pool_entry(slot, *object, occurrence, now)) {
                entry->value = static_cast<std::int32_t>(raw);
                entry->changed = true;
                entry->stamp = static_cast<std::uint16_t>(now >> 16);
                trigger(*device, now);
            }
            break;
        case Target::pool:
            if (auto* entry = pool_entry(slot, *object, occurrence, now)) {
                entry->value = static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(milli, INT32_MIN, INT32_MAX));
                entry->changed = true;
                entry->stamp = static_cast<std::uint16_t>(now >> 16);
                if (options.direct_bridge)
                    trigger(*device, now);
            }
            break;
        }
    }
    // A repeated packet returned above, so a trigger-based packet publishes once.
    if (info & 0x04)
        trigger(*device, now);
}

// ---- Qingping CGD1 0xFDCD

void decode_cgd1(const std::uint8_t* data, std::size_t size,
    const platform::BleAdvertisement& advertisement, std::uint32_t now) {
    if (size < 10)
        return;
    auto* device = admit(advertisement.address, advertisement.address_type, kTypes[cgd1].pid);
    if (device == nullptr)
        return;
    touch(*device, advertisement, now);
    switch (le16(data + 8)) {
    case 0x0401:
        if (size < 14)
            return;
        if (static_cast<std::int16_t>(le16(data + 10)) < 600) {
            device->temperature = static_cast<std::int16_t>(le16(data + 10) * 10);
            store(*device, temperature, now);
        }
        if (le16(data + 12) < 1000) {
            device->humidity = static_cast<std::uint16_t>(le16(data + 12) * 10);
            store(*device, humidity, now);
        }
        break;
    case 0x0102:
        if (size >= 11 && data[10] <= 100) {
            device->battery = data[10];
            store(*device, battery, now);
        }
        break;
    default:
        break;
    }
}

// ---- Mi Body Composition Scale 0x181B

void decode_scale(const std::uint8_t* data, std::size_t size,
    const platform::BleAdvertisement& advertisement, std::uint32_t now) {
    if (size < 13)
        return;
    auto* device = admit(advertisement.address, advertisement.address_type, kTypes[mibcs].pid);
    if (device == nullptr)
        return;
    const std::uint8_t flags_a = data[0], flags_b = data[1];
    const bool stable = flags_b & 0x20, impedance_stable = flags_b & 0x02, done = flags_b & 0x80;
    const auto impedance_value = le16(data + 9), raw = le16(data + 11);
    // A new session starts after silence or with the first unfinished packet after a finished one.
    if (now - device->seen > kScaleSession || ((device->scale_state & finished) && !done))
        device->scale_state = 0;
    touch(*device, advertisement, now);
    if (raw == 0xFFF0) // overload
        return;
    // Weight in hundredths of the reported unit: jin and lb count 0.01, kg counts 0.005.
    device->weight_unit = (flags_b & 0x40) ? 2 : (flags_a & 0x01) ? 1 : 0;
    device->weight = device->weight_unit == 0 ? static_cast<std::uint16_t>(raw / 2) : raw;
    // Live weight is retained for the dashboard; messages follow the session milestones.
    store(*device, weight, now, false);
    if (stable && !(device->scale_state & weight_sent)) {
        device->scale_state |= weight_sent;
        store(*device, weight, now);
        trigger(*device, now); // session milestones are events, not only direct-bridge values
    }
    if (impedance_stable && impedance_value != 0 && impedance_value != 0xFFFD &&
        impedance_value != 0xFFFE && !(device->scale_state & impedance_sent)) {
        device->impedance = impedance_value;
        device->scale_state |= impedance_sent;
        store(*device, impedance | weight, now); // repeat the weight: self-contained message
        trigger(*device, now);
    }
    if (done) {
        if (!(device->scale_state & (impedance_sent | failed_sent))) {
            device->scale_state |= failed_sent;
            device->events |= kImpedanceFailed;
            store(*device, weight, now);
            trigger(*device, now);
        }
        device->scale_state |= finished;
    }
}

// ---- Generic sightings

/** @brief Matches a resolvable private address against the IRKs of generic entries. */
Device* resolve(const std::uint8_t* address) {
    for (std::size_t i = 0; i < device_count; ++i) {
        auto& device = devices[i];
        if (device.pid != 0 || device.key < 0)
            continue;
        std::uint8_t block[16]{};
        block[13] = address[0];
        block[14] = address[1];
        block[15] = address[2];
        std::uint8_t iv[16]{};
        br_aes_small_cbcenc_keys cipher;
        br_aes_small_cbcenc_init(&cipher, keys[device.key].data(), 16);
        br_aes_small_cbcenc_run(&cipher, iv, block, sizeof(block));
        if (std::memcmp(block + 13, address + 3, 3) == 0)
            return &device;
    }
    return nullptr;
}

/** @brief Records a generic sighting. @return The device it belongs to, or nullptr. */
Device* sighting(
    const platform::BleAdvertisement& advertisement, std::string_view name, std::uint32_t now) {
    Device* device = nullptr;
    if (advertisement.address_type == 0)
        device = admit(advertisement.address, 0, 0);
    else if (advertisement.address_type == 1 && irk_configured)
        device = resolve(advertisement.address);
    if (device == nullptr)
        return nullptr;
    touch(*device, advertisement, now);
    device->features |= raw_payload;
    device->events |= raw_payload;
    if (options.direct_bridge)
        trigger(*device, now);
    if (device->name < 0 && !name.empty())
        (void)set_name(*device, name);
    return device;
}

// ---- Event messages

/** @brief Bounded comma-separated JSON members. */
struct Members {
    std::array<char, 256> data{};
    std::size_t size{};
    bool okay{true};

    void add(const char* format, ...) {
        if (!okay)
            return;
        if (size != 0)
            data[size++] = ',';
        va_list arguments;
        va_start(arguments, format);
        const int written =
            std::vsnprintf(data.data() + size, data.size() - size, format, arguments);
        va_end(arguments);
        okay = written >= 0 && std::size_t(written) < data.size() - size;
        if (okay)
            size += std::size_t(written);
    }
};

void clear_pool_changes(std::size_t device) {
    for (std::size_t e = 0; e < pool_count; ++e)
        if (pool[e].device == device)
            pool[e].changed = false;
}

/** @brief Publishes one immediate message per triggered device, like Tasmota's MI32triggerTele:
 *  the changed fields, or every held value with MI32Option0. A message the transport cannot
 *  take now is retried on the next call and dropped after kEventRetention.
 *  @param sighted The generic device @p advertisement was resolved to, or nullptr. A generic
 *         device publishes only together with its report, so a retry waits for the next one
 *         and always carries a current payload.
 */
void publish_events(
    std::uint32_t now, const Device* sighted, const platform::BleAdvertisement* advertisement) {
    static constexpr const char* units[]{"kg", "lb", "jin"};
    for (std::size_t i = 0; i < device_count; ++i) {
        auto& device = devices[i];
        if (device.trigger_at == 0)
            continue;
        if (now - device.trigger_at > kEventRetention) {
            device.trigger_at = 0;
            device.events = 0;
            clear_pool_changes(i);
            continue;
        }
        const bool with_payload = advertisement != nullptr && &device == sighted;
        if (device.pid == 0 && !with_payload)
            continue;
        const auto events = device.events;
        const auto wanted = [&](std::uint32_t bit) {
            return (device.valid & bit) && ((events & bit) || options.aggregate);
        };
        const auto changed = [&](std::uint32_t bit) {
            return (device.valid & bit) && (events & bit);
        };
        char name[kSensorNameCapacity];
        display_name(device, name);
        std::array<SensorReading, 12 + kPool> readings{};
        std::array<std::array<char, 24>, kPool> fields{};
        std::size_t count = 0;
        const auto add = [&](const char* field, SensorQuantity quantity, std::int32_t value) {
            readings[count++] = {name, field, quantity, value};
        };
        if (wanted(temperature))
            add("Temperature", SensorQuantity::temperature, device.temperature * 10);
        if (wanted(humidity))
            add("Humidity", SensorQuantity::humidity, device.humidity * 10);
        if (wanted(temperature) && wanted(humidity))
            add("DewPoint", SensorQuantity::dew_point, 0);
        if (wanted(illuminance))
            add("Illuminance", SensorQuantity::illuminance,
                static_cast<std::int32_t>(std::min<std::uint32_t>(device.illuminance, 2000000)) *
                    1000);
        if (wanted(moisture))
            add("Moisture", SensorQuantity::moisture, device.moisture * 1000);
        if (wanted(conductivity))
            add("Fertility", SensorQuantity::conductivity, device.conductivity * 1000);
        if (changed(door))
            add("Door", SensorQuantity::state, device.door * 1000);
        if (changed(leak))
            add("Leak", SensorQuantity::state, device.leak * 1000);
        if (changed(no_motion_time)) {
            const auto seconds = device.no_motion + (now - device.no_motion_at) / 1000;
            add("NMT", SensorQuantity::age,
                static_cast<std::int32_t>(std::min<std::uint32_t>(seconds, 2000000)) * 1000);
        }
        if (wanted(battery) && device.battery != 0)
            add("Battery", SensorQuantity::battery, device.battery * 1000);
        if (wanted(impedance) && device.impedance != 0)
            add("Impedance", SensorQuantity::impedance, device.impedance * 1000);

        Members members;
        if (events & button)
            members.add("\"Button%u\":%u", unsigned(device.button), device.button_type + 1U);
        if (events & knob)
            members.add("\"Dimmer%s\":%d", device.dimmer_pressed ? "_P" : "", device.dimmer);
        if (events & kHold)
            members.add("\"Hold\":%u", unsigned(device.hold));
        if (events & motion)
            members.add("\"Motion\":1,\"Events\":%u", unsigned(device.event_count));
        else if (events & kNoMotion)
            members.add("\"Motion\":0");
        if (changed(door))
            members.add("\"Events\":%u", unsigned(device.event_count));
        if (wanted(weight)) // in the unit the scale reports, like Tasmota
            members.add("\"Weight\":%u.%02u,\"Unit\":\"%s\"", device.weight / 100U,
                device.weight % 100U, units[device.weight_unit % 3]);
        if (events & kImpedanceFailed)
            members.add("\"ImpedanceFailed\":1");
        if (with_payload && ((events & raw_payload) || options.aggregate)) {
            char hex[2 * sizeof(advertisement->payload) + 1]{};
            for (std::size_t b = 0; b < advertisement->length; ++b)
                std::snprintf(hex + 2 * b, 3, "%02X", advertisement->payload[b]);
            members.add("\"Payload\":\"%s\"", hex);
        }
        members.add("\"RSSI\":%d,\"MAC\":\"%02X%02X%02X%02X%02X%02X\"", device.rssi, device.mac[0],
            device.mac[1], device.mac[2], device.mac[3], device.mac[4], device.mac[5]);
        for (std::size_t e = 0; e < pool_count; ++e) {
            const auto& entry = pool[e];
            const auto* object = bthome_object(entry.id);
            if (entry.device != i || object == nullptr)
                continue;
            auto& field = fields[e];
            pool_field(entry, field.data(), field.size());
            if (object->target == Target::button && entry.changed)
                members.add("\"%s\":%d", field.data(), int(entry.value));
            else if (object->target == Target::dimmer && entry.changed) // event 1 turns left
                members.add("\"%s\":%d", field.data(),
                    (entry.value & 0xFF) == 1 ? -(entry.value >> 8) : entry.value >> 8);
            else if (!is_event(*object) && (entry.changed || options.aggregate))
                add(field.data(), object->quantity, entry.value);
        }
        if (!members.okay) { // cannot happen within the bounds above; never retry forever
            device.trigger_at = 0;
            continue;
        }
        if (sensor_service().publish_event(
                name, readings.data(), count, {members.data.data(), members.size})) {
            device.trigger_at = 0;
            device.events = 0;
            clear_pool_changes(i);
        }
    }
}

/** @brief Walks every AD element once; malformed elements end the walk. */
void receive(const platform::BleAdvertisement& advertisement) {
    const auto now = platform::monotonic_milliseconds();
    const std::uint8_t* data = advertisement.payload;
    const std::size_t size = advertisement.length;
    std::string_view name;
    for (std::size_t index = 0; index < size;) {
        const std::size_t length = data[index];
        if (length == 0 || length > size - index - 1)
            break;
        if (data[index + 1] == 0x08 || data[index + 1] == 0x09)
            name = {reinterpret_cast<const char*>(data + index + 2), length - 1};
        index += length + 1;
    }
    bool handled = false;
    for (std::size_t index = 0; index < size;) {
        const std::size_t length = data[index];
        if (length == 0 || length > size - index - 1)
            break;
        const std::uint8_t* element = data + index + 2;
        if (data[index + 1] == 0x16 && length >= 3) {
            const std::size_t value_size = length - 3;
            switch (le16(element)) {
            case 0xFE95:
                decode_mibeacon(element + 2, value_size, advertisement, now);
                handled = true;
                break;
            case 0xFCD2:
                decode_bthome(element + 2, value_size, advertisement, name, now);
                handled = true;
                break;
            case 0xFDCD:
                decode_cgd1(element + 2, value_size, advertisement, now);
                handled = true;
                break;
            case 0x181B:
                decode_scale(element + 2, value_size, advertisement, now);
                handled = true;
                break;
            default:
                break;
            }
        }
        index += length + 1;
    }
    const Device* sighted = nullptr;
    if (!handled && options.every_device)
        sighted = sighting(advertisement, name, now);
    publish_events(now, sighted, &advertisement);
}

// ---- Configuration file

void load_configuration() {
    std::unique_ptr<char[]> text{new (std::nothrow) char[kConfigMaximum]};
    constexpr std::size_t kTokens = 1 + kCapacity * 11;
    std::unique_ptr<support::JsonToken[]> tokens{new (std::nothrow) support::JsonToken[kTokens]};
    if (!text || !tokens)
        return;
    std::size_t size = 0;
    const auto result = filesystem::read_file(kConfigPath, text.get(), kConfigMaximum, size);
    if (result == filesystem::ReadResult::not_found)
        return;
    if (result != filesystem::ReadResult::succeeded) {
        AddLog(LogLevel::error, "M32: config read failed");
        return;
    }
    while (size > 0 && text[size - 1] == '\0') // Tasmota stores a terminator
        --size;
    support::JsonReader json{{text.get(), size}, tokens.get(), kTokens};
    std::size_t root = 0;
    if (!json.parse(root) || json.token(root).kind != support::JsonKind::array) {
        AddLog(LogLevel::error, "M32: invalid config");
        return;
    }
    unsigned loaded = 0;
    for (std::size_t i = 0;; ++i) {
        const auto entry = json.array_item(root, i);
        if (entry >= json.token_count())
            break;
        char mac_text[13], pid_text[5];
        std::size_t length = 0;
        std::uint8_t mac[6], pid[2];
        if (!support::json_copy_string(
                json, json.member(entry, "MAC"), mac_text, sizeof(mac_text), length) ||
            !hex_bytes({mac_text, length}, mac, 6) ||
            !support::json_copy_string(
                json, json.member(entry, "PID"), pid_text, sizeof(pid_text), length) ||
            !hex_bytes({pid_text, length}, pid, 2)) {
            AddLog(LogLevel::error, "M32: config entry %u invalid", unsigned(i));
            continue;
        }
        const std::uint16_t product = static_cast<std::uint16_t>(pid[0] << 8 | pid[1]);
        if (product == 0x0A1C || product == 0x944A) {
            AddLog(LogLevel::info, "M32: ATC/PVVX entry %u skipped, use BTHome", unsigned(i));
            continue;
        }
        auto* device = admit(mac, 0, product);
        if (device == nullptr) {
            AddLog(LogLevel::error, "M32: config full at entry %u", unsigned(i));
            break;
        }
        device->status |= configured;
        if (product == 0)
            options.every_device = true; // a configured generic entry is handled
        char key_text[33], name[kSensorNameCapacity + 4];
        std::uint8_t key[16];
        if (support::json_copy_string(
                json, json.member(entry, "key"), key_text, sizeof(key_text), length) &&
            length != 0 && (!hex_bytes({key_text, length}, key, 16) || !set_key(*device, key)))
            AddLog(LogLevel::error, "M32: config key %u invalid", unsigned(i));
        if (support::json_copy_string(json, json.member(entry, "name"), name, sizeof(name), length))
            (void)set_name(*device, {name, length});
        std::uint32_t features = 0;
        if (support::json_unsigned(json, json.member(entry, "feat"), UINT32_MAX, features))
            device->features = features;
        ++loaded;
    }
    if (loaded != 0)
        options.closed = true; // as in Tasmota, a loaded configuration admits no new devices
    AddLog(LogLevel::info, "M32: loaded %u sensor configurations", loaded);
}

bool save_configuration() {
    std::unique_ptr<char[]> text{new (std::nothrow) char[kConfigMaximum]};
    if (!text || device_count == 0)
        return false;
    std::size_t position = 0;
    text[position++] = '[';
    for (std::size_t i = 0; i < device_count; ++i) {
        const auto& device = devices[i];
        char key[33]{};
        if (device.key >= 0)
            for (unsigned b = 0; b < 16; ++b)
                std::snprintf(key + 2 * b, 3, "%02X", keys[device.key][b]);
        char extra[112]{};
        if (device.name >= 0) {
            // Names are printable ASCII; only quotes and backslashes need escaping.
            char name[2 * kSensorNameCapacity]{};
            std::size_t length = 0;
            for (const char* c = names[device.name].data(); *c != '\0'; ++c) {
                if (*c == '"' || *c == '\\')
                    name[length++] = '\\';
                name[length++] = *c;
            }
            std::snprintf(extra, sizeof(extra), ",\"name\":\"%s\",\"feat\":%lu", name,
                static_cast<unsigned long>(device.features));
        } else if (device.type == bthome)
            std::snprintf(
                extra, sizeof(extra), ",\"feat\":%lu", static_cast<unsigned long>(device.features));
        const int written = std::snprintf(text.get() + position, kConfigMaximum - position,
            "{\"MAC\":\"%02X%02X%02X%02X%02X%02X\",\"PID\":\"%04x\",\"key\":\"%s\"%s},",
            device.mac[0], device.mac[1], device.mac[2], device.mac[3], device.mac[4],
            device.mac[5], unsigned(device.pid), key, extra);
        if (written < 0 || std::size_t(written) >= kConfigMaximum - position)
            return false;
        position += std::size_t(written);
    }
    text[position - 1] = ']';
    if (filesystem::write_file(kConfigPath, text.get(), position) !=
        filesystem::MutationResult::succeeded)
        return false;
    AddLog(LogLevel::info, "M32: saved %u sensor configurations", unsigned(device_count));
    return true;
}

// ---- Sensor driver hooks

void reset() {
    ble::set_native_consumer(nullptr);
    device_count = visible_count = hidden_logged = 0;
    key_used = {};
    name_used = {};
    irk_configured = false;
    options = {};
    pool_count = 0;
    pool_full_logged = false;
}

void initialize(std::uint32_t, const Hardware*) {
    load_configuration();
    ble::set_native_consumer(receive);
    AddLog(LogLevel::info, "M32: driver started");
}

/** @brief Retries undelivered events, drops expired values, then chooses the visible devices:
 *  configured first, then the most recently seen; a device unseen for kExpiry is not shown.
 */
void poll(std::uint32_t now) {
    publish_events(now, nullptr, nullptr);
    const auto tick = static_cast<std::uint16_t>(now >> 16);
    for (std::size_t i = 0; i < device_count; ++i) {
        auto& device = devices[i];
        for (unsigned bit = 0; bit < device.stamps.size(); ++bit)
            if ((device.valid & (1U << bit)) &&
                static_cast<std::uint16_t>(tick - device.stamps[bit]) >= kExpiryTicks)
                device.valid &= ~(1U << bit);
    }
    for (std::size_t e = pool_count; e-- > 0;)
        if (static_cast<std::uint16_t>(tick - pool[e].stamp) >= kExpiryTicks)
            pool_remove(e);
    std::array<bool, kCapacity> taken{};
    visible_count = 0;
    for (std::size_t i = 0; i < device_count && visible_count < kVisible; ++i)
        if ((devices[i].status & configured) && devices[i].seen != 0 &&
            now - devices[i].seen < kExpiry) {
            taken[i] = true;
            visible[visible_count++] = static_cast<std::uint8_t>(i);
        }
    std::size_t candidates = visible_count;
    for (std::size_t i = 0; i < device_count; ++i)
        if (!taken[i] && devices[i].seen != 0 && now - devices[i].seen < kExpiry)
            ++candidates;
    while (visible_count < kVisible) {
        std::size_t best = kCapacity;
        for (std::size_t i = 0; i < device_count; ++i)
            if (!taken[i] && devices[i].seen != 0 && now - devices[i].seen < kExpiry &&
                (best == kCapacity || now - devices[i].seen < now - devices[best].seen))
                best = i;
        if (best == kCapacity)
            break;
        taken[best] = true;
        visible[visible_count++] = static_cast<std::uint8_t>(best);
    }
    const std::size_t hidden = candidates - visible_count;
    if (hidden != hidden_logged) {
        if (hidden != 0)
            AddLog(LogLevel::info, "M32: %u devices not shown, limit %u", unsigned(hidden),
                unsigned(kVisible));
        hidden_logged = hidden;
    }
}

/** @brief Emits one device's readings, a dashboard-only field among them. */
bool visit_slot(const std::size_t slot, SensorReadingVisitor visitor, void* context) {
    const auto now = platform::monotonic_milliseconds();
    const auto& device = devices[slot];
    char name[kSensorNameCapacity];
    display_name(device, name);
    const bool sighting_only = device.pid == 0;
    const bool hidden_summary = options.no_summary;
    auto emit = [&](const char* field, SensorQuantity quantity, std::int32_t value,
                    bool dashboard = false) {
        return visitor(context, {name, field, quantity, value, dashboard || hidden_summary});
    };
    const auto bits = device.valid;
    if ((bits & temperature) &&
        !emit("Temperature", SensorQuantity::temperature, device.temperature * 10))
        return false;
    if ((bits & humidity) && !emit("Humidity", SensorQuantity::humidity, device.humidity * 10))
        return false;
    if ((bits & temperature) && (bits & humidity) &&
        !emit("DewPoint", SensorQuantity::dew_point, 0))
        return false;
    if ((bits & illuminance) &&
        !emit("Illuminance", SensorQuantity::illuminance,
            static_cast<std::int32_t>(std::min<std::uint32_t>(device.illuminance, 2000000)) * 1000))
        return false;
    if ((bits & moisture) && !emit("Moisture", SensorQuantity::moisture, device.moisture * 1000))
        return false;
    if ((bits & conductivity) &&
        !emit("Fertility", SensorQuantity::conductivity, device.conductivity * 1000))
        return false;
    if ((bits & door) && !emit("Door", SensorQuantity::state, device.door * 1000))
        return false;
    if ((bits & leak) && !emit("Leak", SensorQuantity::state, device.leak * 1000))
        return false;
    if (bits & no_motion_time) {
        const auto seconds = device.no_motion + (now - device.no_motion_at) / 1000;
        if (!emit("NMT", SensorQuantity::age,
                static_cast<std::int32_t>(std::min<std::uint32_t>(seconds, 2000000)) * 1000))
            return false;
    }
    if ((bits & battery) && !emit("Battery", SensorQuantity::battery, device.battery * 1000))
        return false;
    if (bits & weight) {
        // Weight is shown in kg; lb and jin are converted (1 jin = 0.5 kg).
        std::int32_t grams = device.weight * 10;
        if (device.weight_unit == 1)
            grams = static_cast<std::int32_t>(std::int64_t(device.weight) * 45359237 / 10000000);
        else if (device.weight_unit == 2)
            grams = device.weight * 5;
        if (!emit("Weight", SensorQuantity::weight, grams))
            return false;
    }
    if ((bits & impedance) &&
        !emit("Impedance", SensorQuantity::impedance, device.impedance * 1000))
        return false;
    for (std::size_t e = 0; e < pool_count; ++e) {
        const auto& entry = pool[e];
        const auto* object = bthome_object(entry.id);
        if (entry.device != slot || object == nullptr || is_event(*object))
            continue;
        char field[24];
        pool_field(entry, field, sizeof(field));
        if (!emit(field, object->quantity, entry.value))
            return false;
    }
    // Local time of the last packet, like Tasmota; omitted until the clock is set.
    char members[48];
    int used = 0;
#if USE_TIME
    if (time_source != nullptr && time_source->synchronized())
        used = std::snprintf(members, sizeof(members), "\"Time\":%lu,",
            static_cast<unsigned long>(
                time_source->to_local(time_source->utc_seconds(now) - (now - device.seen) / 1000)));
#endif
    std::snprintf(members + used, sizeof(members) - std::size_t(used),
        "\"MAC\":\"%02X%02X%02X%02X%02X%02X\"", device.mac[0], device.mac[1], device.mac[2],
        device.mac[3], device.mac[4], device.mac[5]);
    if (!visitor(context, {name, "RSSI", SensorQuantity::signal_strength, device.rssi * 1000,
                              sighting_only || hidden_summary, members}))
        return false;
    const auto age = (now - device.seen) / 1000;
    if (!emit("LastSeen", SensorQuantity::age,
            static_cast<std::int32_t>(std::min<std::uint32_t>(age, 2000000)) * 1000, true))
        return false;
    return true;
}

bool visit_readings(SensorReadingVisitor visitor, void* context) {
    for (std::size_t v = 0; v < visible_count; ++v)
        if (!visit_slot(visible[v], visitor, context))
            return false;
    return true;
}

std::size_t walk_count(const bool every) {
    return every ? device_count : visible_count;
}

bool visit_device(
    const std::size_t index, const bool every, SensorReadingVisitor visitor, void* context) {
    if (index >= walk_count(every))
        return true;
    const std::size_t slot = every ? index : visible[index];
    return devices[slot].seen == 0 || visit_slot(slot, visitor, context);
}
} // namespace

// Constant lifecycle descriptor: no static constructor or linker discovery.
extern const SensorDriverHooks ble_sensors_driver{
    reset, initialize, poll, visit_readings, walk_count, visit_device};

#if USE_TIME
void ble_sensors_attach_time(const TimeService* time) {
    time_source = time;
}
#endif

namespace commands {
namespace {
CommandStatus key_command(const CommandRequest& request, CommandOutput& output) {
    const auto text = support::trim_ascii(request.payload);
    std::uint8_t buffer[22];
    if (text.size() == 44) {
        if (!hex_bytes(text, buffer, 22))
            return CommandStatus::invalid;
    } else if (text.size() == 36) {
        // Short YLKG07/08 form: the middle of the key is fixed.
        if (!hex_bytes(text, buffer, 18))
            return CommandStatus::invalid;
        std::memmove(buffer + 10, buffer + 6, 12);
        constexpr std::uint8_t fill[4]{0x8D, 0x3D, 0x3C, 0x97};
        std::memcpy(buffer + 6, fill, 4);
    } else {
        return CommandStatus::invalid;
    }
    auto* device = find(buffer + 16);
    if (device == nullptr) {
        (void)output.write("{\"MI32Key\":\"Unknown MAC\"}");
        return CommandStatus::failed;
    }
    if (!set_key(*device, buffer))
        return CommandStatus::failed;
    return output.write("{\"MI32Key\":\"Done\"}") ? CommandStatus::handled : CommandStatus::failed;
}

CommandStatus name_command(const CommandRequest& request, CommandOutput& output) {
    const std::size_t slot = request.has_index ? request.index : 1U; // Tasmota's default index
    if (slot >= device_count)
        return CommandStatus::invalid;
    auto& device = devices[slot];
    if (!request.is_query() && !set_name(device, support::trim_ascii(request.payload)))
        return CommandStatus::failed;
    char name[kSensorNameCapacity];
    display_name(device, name);
    return output.write("{\"MI32Name\":") && output.json_string(name) && output.write("}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}

CommandStatus option_command(const CommandRequest& request, CommandOutput& output) {
    const unsigned index = request.has_index ? request.index : 0U;
    bool* option = nullptr;
    switch (index) {
    case 0:
        option = &options.aggregate;
        break;
    case 1:
        option = &options.no_summary;
        break;
    case 2:
        option = &options.direct_bridge;
        break;
    case 3:
        option = &options.closed;
        break;
    case 5:
        option = &options.every_device;
        break;
    default:
        return CommandStatus::invalid; // 4 is BLEActive, 99 belongs to core BLE
    }
    if (!request.is_query()) {
        std::uint32_t value = 0;
        if (!support::parse_unsigned_decimal(support::trim_ascii(request.payload), 1, value))
            return CommandStatus::invalid;
        *option = value != 0;
    }
    return output.format("{\"MI32Option%u\":%u}", index, *option ? 1U : 0U) ? CommandStatus::handled
                                                                            : CommandStatus::failed;
}

CommandStatus config_command(const CommandRequest&, CommandOutput& output) {
    const bool saved = save_configuration();
    return output.format("{\"MI32Cfg\":\"%s\"}", saved ? "Done" : "Failed") && saved
               ? CommandStatus::handled
               : CommandStatus::failed;
}
} // namespace

CommandStatus dispatch_ble_sensors(const CommandRequest& request, CommandOutput& output) {
    if (support::ascii_iequals(request.name, "MI32Key"))
        return request.has_index ? CommandStatus::invalid : key_command(request, output);
    if (support::ascii_iequals(request.name, "MI32Name"))
        return name_command(request, output);
    if (support::ascii_iequals(request.name, "MI32Cfg"))
        return request.has_index ? CommandStatus::invalid : config_command(request, output);
    if (support::ascii_iequals(request.name, "MI32Option"))
        return option_command(request, output);
    return CommandStatus::unknown;
}
} // namespace commands
} // namespace newmota
