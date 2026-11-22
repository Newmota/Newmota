/*
  ha_discovery.cpp - Home Assistant MQTT discovery for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ha_discovery.cpp
 * @brief One retained `homeassistant/<platform>/<MAC>/<id>/config` message per entity.
 *
 * Entities: relays (switch), sensor readings (sensor) and the device status (diagnostic
 * sensors, a Restart button). State comes from the STATE and SENSOR topics Newmota already
 * publishes; commands go to the normal command topics. The ids published last are kept in
 * `/.newmota/ha_discovery.txt`, so an entity that disappears is removed even after a restart.
 */

#include "services/mqtt/ha_discovery.h"

#include "core/command/command.h"
#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include "newmota/platform/network.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include "services/mqtt/mqtt_service.h"
#include "services/power/power_service.h"
#include "services/settings/settings.h"
#if USE_STATE_TEXT
#include "services/settings/state_text.h"
#endif
#if USE_SENSORS || USE_ENERGY
#include "drivers/sensors/sensor_service.h"
#endif
#include "support/text.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace newmota::ha_discovery {
namespace {

constexpr std::string_view kFile = "/.newmota/ha_discovery.txt";
constexpr std::size_t kIdCapacity = 48U;
constexpr std::size_t kMaximumEntities = 64U;
constexpr std::size_t kPayloadCapacity = CanonicalMessage::kPayloadCapacity;
constexpr std::uint32_t kChangeCheckMilliseconds = 1000U;

enum class Kind : std::uint8_t { relay, sensor, rssi, signal, uptime, address, restart };

struct Entity final {
    Kind kind{};
    const char* platform{};
    std::array<char, kIdCapacity> id{};
    std::uint8_t relay{};
#if USE_SENSORS || USE_ENERGY
    std::array<char, kSensorNameCapacity> sensor{};
    std::array<char, 40> field{}; ///< Field name in the SENSOR message, without a phase number.
    std::int8_t element{-1};      ///< Array index of a phase in ENERGY, or -1.
    SensorQuantity quantity{};
    const char* unit{};
#endif
};

enum class Stage : std::uint8_t { idle, publish, remove, save };

struct State final {
    Stage stage{};
    bool was_connected{};
    bool scheduled{};
    std::uint32_t due{};
    std::uint32_t last_check{};
    std::uint32_t published_fingerprint{};
    std::uint32_t run_fingerprint{};
    std::size_t cursor{};
    std::uint32_t file_offset{};
};

State state{};

/** @brief Copies text into fixed storage, cut at the capacity. */
template <std::size_t N> void copy(std::array<char, N>& target, const std::string_view text) {
    const auto size = text.size() < N ? text.size() : N - 1U;
    std::memcpy(target.data(), text.data(), size);
    target[size] = '\0';
}

/** @brief Appends text reduced to Home Assistant's id characters [a-zA-Z0-9_-]. */
void append_id(std::array<char, kIdCapacity>& id, const std::string_view text) {
    auto size = std::strlen(id.data());
    for (const char c : text) {
        if (size + 1U >= id.size())
            break;
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        id[size++] = allowed ? c : '_';
    }
    id[size] = '\0';
}

void fnv(std::uint32_t& hash, const std::string_view text) {
    for (const char c : text) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
    }
    hash = (hash ^ 0xFFU) * 16777619U;
}

[[nodiscard]] bool enabled(const Settings& settings) {
    std::uint8_t off = 0U;
    (void)settings.option(19U, off);
    return off == 0U;
}

/** @brief Returns the station MAC as 12 uppercase hex digits. */
[[nodiscard]] std::array<char, 13> device_id(const platform::NetworkInfo& network) {
    std::array<char, 13> id{};
    std::size_t size = 0U;
    for (const char c : std::string_view{network.mac}) {
        if (c != ':' && size + 1U < id.size())
            id[size++] = c;
    }
    return id;
}

#if USE_SENSORS || USE_ENERGY
[[nodiscard]] bool usable(const SensorReading& reading) {
    if (reading.quantity == SensorQuantity::state)
        return false;
    // The energy meter's readings are dashboard-only; its MQTT fields carry the same names.
    return !reading.dashboard_only || std::string_view{reading.sensor} == "ENERGY";
}

struct SensorCursor final {
    std::size_t wanted;
    std::size_t seen;
    Entity* entity;
};

/** @brief Fills the entity for one usable reading; a phase number becomes an array index. */
void fill_sensor(Entity& entity, const SensorReading& reading) {
    entity.kind = Kind::sensor;
    entity.platform = "sensor";
    copy(entity.sensor, reading.sensor);
    std::string_view field{reading.field};
    entity.element = -1;
    if (std::string_view{reading.sensor} == "ENERGY") {
        std::size_t digits = field.size();
        while (digits > 0U && field[digits - 1U] >= '0' && field[digits - 1U] <= '9')
            --digits;
        if (digits > 0U && digits < field.size() && field.size() - digits <= 2U) {
            unsigned number = 0U;
            for (std::size_t i = digits; i < field.size(); ++i)
                number = number * 10U + unsigned(field[i] - '0');
            if (number >= 1U && number <= 32U)
                entity.element = static_cast<std::int8_t>(number - 1U);
            field = field.substr(0U, digits);
        }
    }
    copy(entity.field, field);
    entity.quantity = reading.quantity;
    entity.unit = sensor_reading_unit(reading, sensor_service().presentation());
    entity.id.fill('\0');
    append_id(entity.id, "s_");
    append_id(entity.id, reading.sensor);
    append_id(entity.id, "_");
    append_id(entity.id, reading.field);
}

bool visit_sensor(void* context, const SensorReading& reading) {
    auto& cursor = *static_cast<SensorCursor*>(context);
    if (!usable(reading))
        return true;
    if (cursor.entity != nullptr && cursor.seen == cursor.wanted) {
        fill_sensor(*cursor.entity, reading);
        ++cursor.seen;
        return false;
    }
    ++cursor.seen;
    return true;
}

/** @brief Counts the usable readings, or fills the one at @p wanted. */
std::size_t sensors(std::size_t wanted, Entity* entity) {
    SensorCursor cursor{wanted, 0U, entity};
    (void)sensor_service().visit_readings(visit_sensor, &cursor);
    return cursor.seen;
}
#endif

/** @brief Returns the relay instances that are not owned by the Light. */
[[nodiscard]] std::size_t relays(const PowerService& power, std::array<std::uint8_t, 32>& list) {
    std::size_t count = 0U;
    for (std::uint8_t i = 1U; i <= 32U; ++i) {
        if (power.has_relay(i))
            list[count++] = i;
    }
    return count;
}

/** @brief Builds entity @p index of the current set; false past the last one. */
[[nodiscard]] bool entity_at(const PowerService& power, std::size_t index, Entity& entity) {
    entity = {};
    std::array<std::uint8_t, 32> relay_list{};
    const auto relay_count = relays(power, relay_list);
    if (index < relay_count) {
        entity.kind = Kind::relay;
        entity.platform = "switch";
        entity.relay = relay_list[index];
        char text[8];
        std::snprintf(text, sizeof(text), "r%u", unsigned(entity.relay));
        append_id(entity.id, text);
        return true;
    }
    index -= relay_count;
#if USE_SENSORS || USE_ENERGY
    const auto sensor_count = sensors(SIZE_MAX, nullptr);
    if (index < sensor_count) {
        (void)sensors(index, &entity);
        return entity.kind == Kind::sensor;
    }
    index -= sensor_count;
#endif
    static constexpr Kind kStatus[] = {
#if USE_TASMOTA_STATE
#if USE_WIFI
        Kind::rssi,
        Kind::signal,
#endif
        Kind::uptime,
        Kind::address,
#endif
        Kind::restart,
    };
    static constexpr const char* kStatusIds[] = {
#if USE_TASMOTA_STATE
#if USE_WIFI
        "rssi",
        "signal",
#endif
        "uptime",
        "ip",
#endif
        "restart",
    };
    if (index >= sizeof(kStatus) / sizeof(kStatus[0]))
        return false;
    entity.kind = kStatus[index];
    entity.platform = entity.kind == Kind::restart ? "button" : "sensor";
    append_id(entity.id, kStatusIds[index]);
    return true;
}

[[nodiscard]] std::size_t entity_count(const PowerService& power) {
    std::size_t count = 0U;
    Entity entity{};
    while (count < kMaximumEntities && entity_at(power, count, entity))
        ++count;
    return count;
}

/** @brief Bounded writer for one payload. */
struct Buffer final {
    std::array<char, kPayloadCapacity> data{};
    std::size_t size{};

    static bool write(void* context, const std::string_view text) {
        auto& self = *static_cast<Buffer*>(context);
        if (text.size() > self.data.size() - self.size)
            return false;
        std::memcpy(self.data.data() + self.size, text.data(), text.size());
        self.size += text.size();
        return true;
    }
};

[[nodiscard]] bool field(CommandOutput& out, const char* key, const std::string_view value) {
    return out.write(",\"") && out.write(key) && out.write("\":") && out.json_string(value);
}

[[nodiscard]] bool topic_field(
    CommandOutput& out, const char* key, const std::string_view base, const std::string_view leaf) {
    return out.write(",\"") && out.write(key) && out.write("\":\"") && out.write(base) &&
           out.write(leaf) && out.write("\"");
}

#if USE_SENSORS || USE_ENERGY
/** @brief Home Assistant's device and state class for a reading; empty for none. */
void sensor_classes(const Entity& entity, const char*& device_class, const char*& state_class) {
    device_class = "";
    state_class = "measurement";
    switch (entity.quantity) {
    case SensorQuantity::temperature:
    case SensorQuantity::dew_point:
        device_class = "temperature";
        break;
    case SensorQuantity::humidity:
        device_class = "humidity";
        break;
    case SensorQuantity::pressure:
        device_class = "atmospheric_pressure";
        break;
    case SensorQuantity::illuminance:
        device_class = "illuminance";
        break;
    case SensorQuantity::battery:
        device_class = "battery";
        break;
    case SensorQuantity::moisture:
        device_class = "moisture";
        break;
    case SensorQuantity::conductivity:
        device_class = "conductivity";
        break;
    case SensorQuantity::weight:
        device_class = "weight";
        break;
    case SensorQuantity::signal_strength:
        device_class = "signal_strength";
        break;
    case SensorQuantity::age:
        device_class = "duration";
        break;
    case SensorQuantity::voltage:
        device_class = "voltage";
        break;
    case SensorQuantity::current:
        device_class = "current";
        break;
    case SensorQuantity::power:
        device_class = "power";
        break;
    case SensorQuantity::energy:
        device_class = "energy";
        // Yesterday is a fixed value for a whole day; the others only count up.
        state_class =
            std::string_view{entity.field.data()} == "Yesterday" ? "" : "total_increasing";
        break;
    case SensorQuantity::carbon_dioxide:
        device_class = "carbon_dioxide";
        break;
    case SensorQuantity::volatile_organic:
        device_class = "volatile_organic_compounds";
        break;
    case SensorQuantity::distance:
        device_class = "distance";
        break;
    case SensorQuantity::apparent_power:
        device_class = "apparent_power";
        break;
    case SensorQuantity::reactive_power:
        device_class = "reactive_power";
        break;
    case SensorQuantity::power_factor:
        device_class = "power_factor";
        break;
    case SensorQuantity::frequency:
        device_class = "frequency";
        break;
    case SensorQuantity::impedance:
    case SensorQuantity::particulate:
    case SensorQuantity::count:
    case SensorQuantity::uv_index:
    case SensorQuantity::state:
    case SensorQuantity::other:
        break;
    }
}
#endif

struct Identity final {
    std::array<char, 13> id{};
    platform::NetworkInfo network{};
    std::string_view name;
    std::string_view model;
    std::string_view version;
};

/** @brief Writes the entity part of one discovery payload. */
[[nodiscard]] bool write_entity(
    CommandOutput& out, const Entity& entity, const MqttService& mqtt, const PowerService& power) {
    const auto command = mqtt.command_prefix();
    const auto state_topic = mqtt.state_topic();
    switch (entity.kind) {
    case Kind::relay: {
        char key[16];
        const bool single = power.endpoint_count() == 1U && entity.relay == 1U;
        std::snprintf(key, sizeof(key), single ? "POWER" : "POWER%u", unsigned(entity.relay));
        char name[16];
        std::snprintf(name, sizeof(name), "Relay %u", unsigned(entity.relay));
        char leaf[16];
        std::snprintf(leaf, sizeof(leaf), "POWER%u", unsigned(entity.relay));
#if USE_STATE_TEXT
        const auto on = state_text(1U);
        const auto off = state_text(0U);
#else
        const std::string_view on = "ON";
        const std::string_view off = "OFF";
#endif
        // One power output takes the device name, as Home Assistant names a sole entity.
        return (single ? out.write(",\"name\":null") : field(out, "name", name)) &&
               topic_field(out, "cmd_t", command, leaf) &&
               topic_field(out, "stat_t", state_topic, "") &&
               out.write(",\"val_tpl\":\"{{value_json.") && out.write(key) &&
               out.write("}}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"") && field(out, "stat_on", on) &&
               field(out, "stat_off", off);
    }
    case Kind::sensor: {
#if USE_SENSORS || USE_ENERGY
        char name[kSensorNameCapacity + 48];
        std::snprintf(name, sizeof(name), "%s %s", entity.sensor.data(), entity.field.data());
        if (entity.element >= 0)
            std::snprintf(name + std::strlen(name), sizeof(name) - std::strlen(name), " %d",
                entity.element + 1);
        char tpl[kSensorNameCapacity + 80];
        if (entity.element >= 0)
            std::snprintf(tpl, sizeof(tpl), "{{value_json['%s']['%s'][%d]}}", entity.sensor.data(),
                entity.field.data(), entity.element);
        else
            std::snprintf(tpl, sizeof(tpl), "{{value_json['%s']['%s']}}", entity.sensor.data(),
                entity.field.data());
        const char* device_class = "";
        const char* state_class = "";
        sensor_classes(entity, device_class, state_class);
        return field(out, "name", name) && topic_field(out, "stat_t", mqtt.sensor_topic(), "") &&
               field(out, "val_tpl", tpl) &&
               (*device_class == '\0' || field(out, "dev_cla", device_class)) &&
               (*state_class == '\0' || field(out, "stat_cla", state_class)) &&
               (entity.unit == nullptr || *entity.unit == '\0' ||
                   field(out, "unit_of_meas", entity.unit));
    }
#else
        return false;
    }
#endif
    case Kind::rssi:
        return out.write(",\"name\":\"RSSI\"") && topic_field(out, "stat_t", state_topic, "") &&
               out.write(",\"val_tpl\":\"{{value_json.Wifi.RSSI}}\",\"unit_of_meas\":\"%\","
                         "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
    case Kind::signal:
        return out.write(",\"name\":\"Signal\"") && topic_field(out, "stat_t", state_topic, "") &&
               out.write(",\"val_tpl\":\"{{value_json.Wifi.Signal}}\",\"dev_cla\":"
                         "\"signal_strength\",\"unit_of_meas\":\"dBm\",\"stat_cla\":"
                         "\"measurement\",\"ent_cat\":\"diagnostic\"");
    case Kind::uptime:
        return out.write(",\"name\":\"Uptime\"") && topic_field(out, "stat_t", state_topic, "") &&
               out.write(",\"val_tpl\":\"{{value_json.UptimeSec}}\",\"dev_cla\":"
                         "\"duration\",\"unit_of_meas\":\"s\",\"stat_cla\":"
                         "\"total_increasing\",\"ent_cat\":\"diagnostic\"");
    case Kind::address:
        return out.write(",\"name\":\"IP address\"") &&
               topic_field(out, "stat_t", state_topic, "") &&
               out.write(",\"val_tpl\":\"{{value_json.IPAddress}}\",\"ent_cat\":\"diagnostic\"");
    case Kind::restart:
        return out.write(",\"name\":\"Restart\"") &&
               topic_field(out, "cmd_t", command, "Restart") &&
               out.write(",\"pl_prs\":\"1\",\"dev_cla\":\"restart\",\"ent_cat\":\"config\"");
    }
    return false;
}

/** @brief Builds one complete discovery payload. */
[[nodiscard]] bool build_payload(Buffer& buffer, const Entity& entity, const Identity& identity,
    const MqttService& mqtt, const PowerService& power) {
    CommandOutput out{Buffer::write, &buffer};
    const std::string_view id{identity.id.data()};
    const std::string_view address{identity.network.address};
    return out.write("{\"uniq_id\":\"") && out.write(id) && out.write("_") &&
           out.write(entity.id.data()) && out.write("\"") &&
           write_entity(out, entity, mqtt, power) && field(out, "avty_t", mqtt.lwt_topic()) &&
           out.write(
               ",\"pl_avail\":\"Online\",\"pl_not_avail\":\"Offline\",\"dev\":{\"ids\":[\"") &&
           out.write(id) && out.write("\"]") &&
           field(out, "name",
               identity.name.empty() ? std::string_view{identity.network.hostname}
                                     : identity.name) &&
           out.write(",\"mf\":\"Newmota\"") && field(out, "mdl", identity.model) &&
           field(out, "sw", identity.version) && out.write(",\"cns\":[[\"mac\",") &&
           out.json_string(identity.network.mac) && out.write("]]") &&
#if USE_WEB
           (address.empty() ||
               (out.write(",\"cu\":\"http://") && out.write(address) && out.write("/\""))) &&
#endif
           out.write("},\"o\":{\"name\":\"Newmota\"") && field(out, "sw", identity.version) &&
           out.write(",\"url\":\"https://newmota.github.io/\"}}") && !out.failed();
}

/** @brief Builds `homeassistant/<platform>/<MAC>/<id>/config`. */
[[nodiscard]] bool build_topic(std::array<char, MqttService::kTopicCapacity>& topic,
    const std::string_view platform, const std::string_view id, const std::string_view entity) {
    const int size = std::snprintf(topic.data(), topic.size(), "%.*s/%.*s/%.*s/%.*s/config",
        int(kPrefix.size()), kPrefix.data(), int(platform.size()), platform.data(), int(id.size()),
        id.data(), int(entity.size()), entity.data());
    return size > 0 && static_cast<std::size_t>(size) < topic.size();
}

[[nodiscard]] Identity identity(const Settings& settings, const std::string_view model) {
    Identity result{};
    result.network = platform::network_info();
    result.id = device_id(result.network);
    result.name = settings.device_name();
    result.model = model;
    result.version = platform::info().framework_version();
    return result;
}

/** @brief Hashes everything the published messages depend on. */
[[nodiscard]] std::uint32_t fingerprint(const MqttService& mqtt, const Settings& settings,
    const PowerService& power, const std::string_view model) {
    std::uint32_t hash = 2166136261U;
    const auto who = identity(settings, model);
    fnv(hash, enabled(settings) ? "1" : "0");
    fnv(hash, who.id.data());
    fnv(hash, who.name);
    fnv(hash, who.network.hostname);
    fnv(hash, who.network.address);
    fnv(hash, who.model);
    fnv(hash, who.version);
    fnv(hash, mqtt.command_prefix());
    fnv(hash, mqtt.state_topic());
    fnv(hash, mqtt.sensor_topic());
    fnv(hash, mqtt.lwt_topic());
#if USE_STATE_TEXT
    fnv(hash, state_text(0U));
    fnv(hash, state_text(1U));
#endif
    Entity entity{};
    for (std::size_t i = 0U; i < kMaximumEntities && entity_at(power, i, entity); ++i) {
        fnv(hash, entity.platform);
        fnv(hash, entity.id.data());
#if USE_SENSORS || USE_ENERGY
        if (entity.kind == Kind::sensor && entity.unit != nullptr)
            fnv(hash, entity.unit);
#endif
    }
    fnv(hash, power.endpoint_count() == 1U ? "single" : "many");
    return hash;
}

/** @brief Reads the next `platform/id` line of the saved list; false at its end. */
[[nodiscard]] bool next_saved(std::array<char, kIdCapacity + 16>& line) {
    filesystem::FileReader reader{};
    if (filesystem::open_reader(kFile, reader) != filesystem::ReadResult::succeeded)
        return false;
    bool found = false;
    if (filesystem::seek(reader, state.file_offset) == filesystem::ReadResult::succeeded) {
        std::size_t size = 0U;
        if (filesystem::read(reader, line.data(), line.size() - 1U, size) ==
                filesystem::ReadResult::succeeded &&
            size != 0U) {
            const std::string_view text{line.data(), size};
            const auto end = text.find('\n');
            const auto length = end == std::string_view::npos ? size : end;
            line[length] = '\0';
            state.file_offset +=
                static_cast<std::uint32_t>(end == std::string_view::npos ? size : end + 1U);
            found = true;
        }
    }
    (void)filesystem::close(reader);
    return found;
}

/** @brief Writes the ids of the current entity set, or removes the list when it is empty. */
void save(const PowerService& power, const bool on) {
    if (!on) {
        (void)filesystem::remove_if_exists(kFile);
        return;
    }
    (void)filesystem::ensure_directory("/.newmota");
    filesystem::FileWriter writer{};
    if (filesystem::open_writer(kFile, writer) != filesystem::MutationResult::succeeded) {
        AddLog(LogLevel::error, "HAD: cannot save the published entity list");
        return;
    }
    Entity entity{};
    bool okay = true;
    for (std::size_t i = 0U; okay && i < kMaximumEntities && entity_at(power, i, entity); ++i) {
        char line[kIdCapacity + 16];
        const int size =
            std::snprintf(line, sizeof(line), "%s/%s\n", entity.platform, entity.id.data());
        okay = size > 0 && filesystem::write(writer, line, std::size_t(size)) ==
                               filesystem::MutationResult::succeeded;
    }
    if (!filesystem::close(writer) || !okay)
        AddLog(LogLevel::error, "HAD: saving the published entity list failed");
}

/** @brief Reports whether `platform/id` is part of the current set. */
[[nodiscard]] bool current(const PowerService& power, const std::string_view saved) {
    Entity entity{};
    for (std::size_t i = 0U; i < kMaximumEntities && entity_at(power, i, entity); ++i) {
        const std::string_view platform{entity.platform};
        if (saved.size() == platform.size() + 1U + std::strlen(entity.id.data()) &&
            saved.substr(0U, platform.size()) == platform && saved[platform.size()] == '/' &&
            saved.substr(platform.size() + 1U) == std::string_view{entity.id.data()})
            return true;
    }
    return false;
}

void schedule(const std::uint32_t at) {
    state.scheduled = true;
    state.due = at;
}

} // namespace

void poll(MqttService& mqtt, const Settings& settings, const PowerService& power,
    const std::string_view model, const std::uint32_t now) {
    if (!mqtt.connected()) {
        state.was_connected = false;
        state.stage = Stage::idle;
        return;
    }
    if (!state.was_connected) {
        state.was_connected = true;
        state.stage = Stage::idle;
        // Home Assistant asks devices to spread their discovery after a broker restart.
        const auto who = identity(settings, model);
        const auto spread = static_cast<std::uint32_t>(
            (static_cast<std::uint8_t>(who.id[10]) * 31U + static_cast<std::uint8_t>(who.id[11])) %
            3000U);
        schedule(now + 2000U + spread);
        state.last_check = now;
    }
    if (state.stage == Stage::idle) {
        if (static_cast<std::int32_t>(now - state.last_check) >=
            std::int32_t(kChangeCheckMilliseconds)) {
            state.last_check = now;
            if (!state.scheduled &&
                fingerprint(mqtt, settings, power, model) != state.published_fingerprint)
                schedule(now + kChangeCheckMilliseconds);
        }
        if (!state.scheduled || static_cast<std::int32_t>(now - state.due) < 0)
            return;
        state.scheduled = false;
        state.run_fingerprint = fingerprint(mqtt, settings, power, model);
        state.cursor = 0U;
        state.file_offset = 0U;
        state.stage = enabled(settings) ? Stage::publish : Stage::remove;
    }

    const bool on = enabled(settings);
    const auto who = identity(settings, model);
    std::array<char, MqttService::kTopicCapacity> topic{};
    switch (state.stage) {
    case Stage::publish: {
        Entity entity{};
        if (state.cursor >= kMaximumEntities || !entity_at(power, state.cursor, entity)) {
            state.stage = Stage::remove;
            return;
        }
        Buffer buffer{};
        if (!build_topic(topic, entity.platform, who.id.data(), entity.id.data()) ||
            !build_payload(buffer, entity, who, mqtt, power)) {
            AddLog(LogLevel::warning, "HAD: %s/%s does not fit one message", entity.platform,
                entity.id.data());
            ++state.cursor;
            return;
        }
        if (mqtt.queue_publish(topic.data(), {buffer.data.data(), buffer.size}, true))
            ++state.cursor;
        return;
    }
    case Stage::remove: {
        const auto offset = state.file_offset;
        std::array<char, kIdCapacity + 16> line{};
        if (!next_saved(line)) {
            state.stage = Stage::save;
            return;
        }
        const std::string_view saved{line.data()};
        const auto slash = saved.find('/');
        if (slash == std::string_view::npos || (on && current(power, saved)))
            return;
        // An empty retained config removes the entity from Home Assistant.
        if (!build_topic(topic, saved.substr(0U, slash), who.id.data(), saved.substr(slash + 1U)) ||
            !mqtt.queue_publish(topic.data(), {}, true))
            state.file_offset = offset; // try the same line again
        return;
    }
    case Stage::save:
        save(power, on);
        state.published_fingerprint = state.run_fingerprint;
        state.stage = Stage::idle;
        if (on) {
            AddLog(LogLevel::info, "HAD: published %u entities", unsigned(entity_count(power)));
            mqtt.request_telemetry(); // the new entities get their state at once
        } else {
            AddLog(LogLevel::info, "HAD: removed");
        }
        return;
    case Stage::idle:
        return;
    }
}

} // namespace newmota::ha_discovery
