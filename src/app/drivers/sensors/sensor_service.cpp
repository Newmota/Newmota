/*
  sensor_service.cpp - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file sensor_service.cpp
 * @brief Owns selected canonical sensor observations and independent presentation routes.
 */
#include "newmota_features.h"
#if USE_GLOBAL_TEMPERATURE
#include "services/temperature/global_temperature.h"
#endif
#include "drivers/sensors/sensor_service.h"
#include "core/command/command.h"
#include "core/message/message.h"
#include "newmota/log.h"
#include "support/number.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace newmota {
// Generated declarations and constant composition; no runtime discovery.
#define DECLARE_SENSOR(symbol, id, name, field, routes, remote)                                    \
    extern const SensorDriverHooks symbol;
NEWMOTA_SENSOR_DRIVERS(DECLARE_SENSOR)
#undef DECLARE_SENSOR

namespace {
#define SENSOR_DESCRIPTOR(symbol, id, name, field, routes, remote)                                 \
    SensorDriverDescriptor{id, name, field, routes, &symbol, remote},
constexpr std::array<SensorDriverDescriptor, NEWMOTA_SENSOR_DRIVER_COUNT> drivers{
    {NEWMOTA_SENSOR_DRIVERS(SENSOR_DESCRIPTOR)}};
#undef SENSOR_DESCRIPTOR
SensorService service;

// Dashboard change tracking, one mask per web session: bit n is remote device slot n, bit 32 + n
// is local driver n.
constexpr std::size_t kViewers = 2;
constexpr unsigned kLocalDriverBit = 32;
static_assert(drivers.size() <= 64 - kLocalDriverBit);
std::array<std::uint64_t, kViewers> viewer_dirty{};
std::array<bool, kViewers> viewer_reset{};
std::array<std::uint32_t, NEWMOTA_SENSOR_DRIVER_COUNT> driver_hash{};
std::uint32_t changes{};

void reset_viewers() {
    viewer_reset.fill(true);
}

void mark_dirty(const std::uint64_t bit) {
    ++changes;
    for (auto& dirty : viewer_dirty)
        dirty |= bit;
}

[[nodiscard]] bool has_device_walk(const SensorDriverDescriptor& driver) {
    return driver.remote && driver.hooks->device_count && driver.hooks->visit_device;
}
} // namespace

std::uint32_t sensor_series_key(const char* sensor, const char* field) {
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](const char* text) {
        for (; text != nullptr && *text != '\0'; ++text)
            hash = (hash ^ static_cast<std::uint8_t>(*text)) * 16777619U;
    };
    mix(sensor);
    hash = (hash ^ 0x1FU) * 16777619U; // unit separator: "ab"+"c" differs from "a"+"bc"
    mix(field);
    return hash == 0 ? 1 : hash;
}

/** @brief Returns the singleton owner of selected sensor driver composition. */
SensorService& sensor_service() {
    return service;
}

/** @brief Initializes each compiled driver once at startup. */
void SensorService::initialize(
    std::uint32_t now, const SensorSettings& settings, const Hardware* hardware) {
    settings_ = settings;
    hardware_ = hardware;
    for (const auto& driver : drivers) {
        driver.hooks->reset();
        if (settings_.enabled(driver.id))
            driver.hooks->initialize(now, hardware_);
    }
}

const SensorDriverDescriptor* SensorService::driver(std::size_t index) const {
    return index < drivers.size() ? &drivers[index] : nullptr;
}

void SensorService::configure(const SensorSettings& settings, std::uint32_t now) {
    for (const auto& driver : drivers) {
        if (settings_.enabled(driver.id) == settings.enabled(driver.id))
            continue;
        driver.hooks->reset();
        if (settings.enabled(driver.id))
            driver.hooks->initialize(now, hardware_);
        reset_viewers();
    }
    settings_ = settings;
}

void SensorService::set_presentation(const SensorPresentation& presentation) {
    presentation_ = presentation;
    ++changes; // An offset or resolution changes the reported value without a new reading.
    reset_viewers();
}

std::uint32_t SensorService::change_revision() const {
    return changes;
}

void SensorService::mark_device_dirty(const std::size_t slot) {
    if (slot < kLocalDriverBit)
        mark_dirty(std::uint64_t{1} << slot);
}

/** @brief Refreshes selected driver samples on the cooperative scheduler. */
void SensorService::poll(std::uint32_t now) {
    for (std::size_t index = 0; index < drivers.size(); ++index) {
        const auto& driver = drivers[index];
        if (settings_.enabled(driver.id)) {
            driver.hooks->poll(now);
            if (!driver.remote) {
                // A local driver has no packets: new data for the dashboard is a changed value.
                struct Digest {
                    std::uint32_t hash{2166136261U};
                    bool any{};
                } digest;

                (void)driver.hooks->visit_readings(
                    [](void* context, const SensorReading& reading) {
                        auto& d = *static_cast<Digest*>(context);
                        d.hash =
                            (d.hash ^ sensor_series_key(reading.sensor, reading.field)) * 16777619U;
                        d.hash = (d.hash ^ static_cast<std::uint32_t>(reading.value)) * 16777619U;
                        d.any = true;
                        return true;
                    },
                    &digest);
                if (digest.hash != driver_hash[index]) {
                    driver_hash[index] = digest.hash;
                    mark_dirty(std::uint64_t{1} << (kLocalDriverBit + index));
                    if (!digest.any)
                        reset_viewers(); // its cards are gone, which a write cannot express
                }
            }
#if USE_GLOBAL_TEMPERATURE
            /** Default global selection follows accepted canonical visitation order;
             * humidity/pressure refresh the shared validity clock, never temperature units.
             * A remote thermometer is not the device's ambient temperature.
             */
            if (!driver.remote)
                (void)driver.hooks->visit_readings(
                    [](void* context, const SensorReading& reading) {
                        const auto observed = *static_cast<const std::uint32_t*>(context);
                        // A dashboard-only reading is a display copy; its owner reports it.
                        if (reading.dashboard_only)
                            return true;
                        if (reading.quantity == SensorQuantity::temperature)
                            global_temperature().observe(reading.value, observed);
                        else if (reading.quantity == SensorQuantity::humidity ||
                                 reading.quantity == SensorQuantity::pressure)
                            global_temperature().refresh(observed);
                        return true;
                    },
                    &now);
#endif
        }
    }
}

bool SensorService::visit_readings(
    SensorReadingVisitor visitor, void* context, bool local_only) const {
    if (!visitor)
        return false;
    for (const auto& driver : drivers)
        if (settings_.enabled(driver.id) && !(local_only && driver.remote) &&
            !driver.hooks->visit_readings(visitor, context))
            return false;
    return true;
}

namespace {
const char* pressure_unit(const SensorPresentation& settings) {
    return settings.pressure_conversion ? (settings.pressure_inch ? "inHg" : "mmHg") : "hPa";
}

const char* unit(SensorQuantity quantity, const SensorPresentation& settings) {
    switch (quantity) {
    case SensorQuantity::temperature:
    case SensorQuantity::dew_point:
        return settings.fahrenheit ? "°F" : "°C";
    case SensorQuantity::humidity:
        return "%";
    case SensorQuantity::pressure:
        return pressure_unit(settings);
    case SensorQuantity::illuminance:
        return "lx";
    case SensorQuantity::battery:
    case SensorQuantity::moisture:
        return "%";
    case SensorQuantity::conductivity:
        return "µS/cm";
    case SensorQuantity::weight:
        return "kg";
    case SensorQuantity::impedance:
        return "Ω";
    case SensorQuantity::signal_strength:
        return "dBm";
    case SensorQuantity::age:
        return "s";
    case SensorQuantity::voltage:
        return "V";
    case SensorQuantity::current:
        return "A";
    case SensorQuantity::power:
        return "W";
    case SensorQuantity::energy:
        return "kWh";
    case SensorQuantity::carbon_dioxide:
        return "ppm";
    case SensorQuantity::volatile_organic:
    case SensorQuantity::particulate:
        return "µg/m³";
    case SensorQuantity::distance:
        return "m";
    case SensorQuantity::apparent_power:
        return "VA";
    case SensorQuantity::reactive_power:
        return "var";
    case SensorQuantity::frequency:
        return "Hz";
    case SensorQuantity::power_factor:
    case SensorQuantity::other:
    case SensorQuantity::state:
    case SensorQuantity::count:
    case SensorQuantity::uv_index:
        break;
    }
    return "";
}

const char* reading_unit(const SensorReading& reading, const SensorPresentation& settings) {
    return reading.quantity == SensorQuantity::other ? (reading.unit ? reading.unit : "")
                                                     : unit(reading.quantity, settings);
}

// Integer ratios and symmetric rounding avoid native printf floating-point dependencies.
std::int64_t rounded_divide(std::int64_t value, std::int64_t divisor) {
    return value < 0 ? -((-value + divisor / 2) / divisor) : (value + divisor / 2) / divisor;
}

bool project_reading(const SensorReading& reading, const SensorPresentation& settings,
    const std::int32_t ambient_temperature, const std::int32_t ambient_humidity,
    const bool has_ambient_temperature, const bool has_ambient_humidity, std::int64_t& value,
    unsigned& precision) {
    value = reading.value;
    switch (reading.quantity) {
    case SensorQuantity::temperature:
        if (settings.fahrenheit)
            value = rounded_divide(value * 9, 5) + 32000;
        value += std::int64_t(settings.temperature_offset) * 100;
        precision = settings.temperature_resolution;
        break;
    case SensorQuantity::dew_point: {
        // Tasmota computes dew point from temperature and humidity after compensation.
        if (!has_ambient_temperature || !has_ambient_humidity)
            return false;
        double temperature = static_cast<double>(ambient_temperature) / 1000.0;
        const double humidity = static_cast<double>(ambient_humidity) / 1000.0 +
                                static_cast<double>(settings.humidity_offset) / 10.0;
        if (settings.fahrenheit)
            temperature =
                (temperature * 1.8 + 32.0 + settings.temperature_offset / 10.0 - 32.0) / 1.8;
        else
            temperature += settings.temperature_offset / 10.0;
        if (!std::isfinite(temperature) || !std::isfinite(humidity) || humidity <= 0.0 ||
            temperature <= -243.5)
            return false;
        constexpr double a = 17.62;
        constexpr double b = 243.5;
        const double gamma = std::log(humidity / 100.0) + a * temperature / (b + temperature);
        double dew_point = b * gamma / (a - gamma);
        if (!std::isfinite(dew_point))
            return false;
        if (settings.fahrenheit)
            dew_point = dew_point * 1.8 + 32.0;
        value = static_cast<std::int64_t>(::llround(dew_point * 1000.0));
        precision = settings.temperature_resolution;
        break;
    }
    case SensorQuantity::humidity:
        value += std::int64_t(settings.humidity_offset) * 100;
        precision = settings.humidity_resolution;
        break;
    case SensorQuantity::pressure:
        if (settings.pressure_conversion)
            value = settings.pressure_inch ? rounded_divide(value * 295299, 10000000)
                                           : rounded_divide(value * 7500637, 10000000);
        precision = settings.pressure_resolution;
        break;
    case SensorQuantity::weight:
        precision =
            2; // Scales resolve 5 g (kg) or 10 g (lb, jin); Tasmota's MI32 prints two places.
        break;
    case SensorQuantity::voltage:
        precision = settings.voltage_resolution;
        break;
    case SensorQuantity::current:
        precision = settings.current_resolution;
        break;
    case SensorQuantity::energy:
        precision = settings.energy_resolution; // Milli-kWh limits the shown decimals to three.
        break;
    case SensorQuantity::power:
    case SensorQuantity::apparent_power:
    case SensorQuantity::reactive_power:
        precision = settings.wattage_resolution;
        break;
    case SensorQuantity::frequency:
        precision = settings.frequency_resolution;
        break;
    case SensorQuantity::power_factor:
        precision = 2;
        break;
    case SensorQuantity::other:
        precision = reading.decimals;
        break;
    case SensorQuantity::distance:
        precision = 3;
        break;
    case SensorQuantity::uv_index:
        precision = 1;
        break;
    default:
        break;
    }
    return true;
}

bool number(CommandOutput& output, std::int64_t value, unsigned precision) {
    precision = std::min(precision, 3U);
    constexpr std::int64_t divisors[]{1000, 100, 10, 1};
    constexpr unsigned scales[]{1, 10, 100, 1000};
    value = rounded_divide(value, divisors[precision]);
    // Negating after the shift keeps INT64_MIN defined.
    const auto magnitude = value < 0 ? std::uint64_t(-(value + 1)) + 1 : std::uint64_t(value);
    if (!precision)
        return output.format(
            "%s%s", value < 0 ? "-" : "", newmota::support::decimal(magnitude).text);
    return output.format("%s%s.%0*u", value < 0 ? "-" : "",
        newmota::support::decimal(std::uint64_t(magnitude / scales[precision])).text,
        static_cast<int>(precision), static_cast<unsigned>(magnitude % scales[precision]));
}

/** @brief Tracks one device's latest temperature and humidity for its dew point. */
struct Ambient {
    char sensor[kSensorNameCapacity]{};
    std::int32_t temperature{}, humidity{};
    bool has_temperature{}, has_humidity{};

    /** @brief Projects one reading; false omits it. Tracking restarts with another sensor name,
     *  which the caller has checked against kSensorNameCapacity.
     */
    bool project(const SensorReading& reading, const SensorPresentation& settings,
        std::int64_t& value, unsigned& precision) {
        if (!sensor[0] || std::strcmp(sensor, reading.sensor) != 0) {
            std::strcpy(sensor, reading.sensor);
            has_temperature = has_humidity = false;
        }
        if (reading.quantity == SensorQuantity::temperature) {
            temperature = reading.value;
            has_temperature = true;
        } else if (reading.quantity == SensorQuantity::humidity) {
            humidity = reading.value;
            has_humidity = true;
        }
        value = 0;
        precision = 0;
        return project_reading(reading, settings, temperature, humidity, has_temperature,
            has_humidity, value, precision);
    }
};
} // namespace

const char* sensor_reading_unit(
    const SensorReading& reading, const SensorPresentation& presentation) {
    return reading_unit(reading, presentation);
}

namespace {
/** @brief Builds StatusSNS members, one object per sensor name. */
struct FieldWriter {
    CommandOutput& output;
    const SensorPresentation& settings;
    bool dashboard; ///< Includes the dashboard-only readings.
    char sensor[kSensorNameCapacity]{};
    Ambient ambient{};
    bool open{}, temperature{}, pressure{};
};

bool write_field(void* opaque, const SensorReading& reading) {
    auto& c = *static_cast<FieldWriter*>(opaque);
    if (!reading.sensor || std::strlen(reading.sensor) >= sizeof(c.sensor))
        return false;
    if (reading.dashboard_only && !c.dashboard)
        return true;
    std::int64_t value = 0;
    unsigned precision = 0;
    if (!c.ambient.project(reading, c.settings, value, precision))
        return true;
    if (!c.open || std::strcmp(c.sensor, reading.sensor) != 0) {
        if (c.open && !c.output.write("},"))
            return false;
        if (!c.output.json_string(reading.sensor) || !c.output.write(":{"))
            return false;
        std::strcpy(c.sensor, reading.sensor);
        c.open = true;
    } else if (!c.output.write(","))
        return false;
    c.temperature |= reading.quantity == SensorQuantity::temperature ||
                     reading.quantity == SensorQuantity::dew_point;
    c.pressure |= reading.quantity == SensorQuantity::pressure;
    // The dashboard needs a self-named unit; StatusSNS and MQTT carry the plain value.
    const bool own_unit = c.dashboard && reading.quantity == SensorQuantity::other;
    return c.output.json_string(reading.field) && c.output.write(own_unit ? ":{\"v\":" : ":") &&
           number(c.output, value, precision) &&
           (!own_unit || (c.output.write(",\"u\":") &&
                             c.output.json_string(reading_unit(reading, c.settings)) &&
                             c.output.write("}"))) &&
           (!reading.members || (c.output.write(",") && c.output.write(reading.members)));
}

bool finish_fields(FieldWriter& c) {
    if (c.open && !c.output.write("}"))
        return false;
    if (c.temperature && (!c.output.write(",\"TempUnit\":") ||
                             !c.output.json_string(c.settings.fahrenheit ? "F" : "C")))
        return false;
    return !c.pressure || (c.output.write(",\"PressureUnit\":") &&
                              c.output.json_string(pressure_unit(c.settings)));
}

/** @brief Runs @p visit against a counting output; 0 when it would write nothing. */
template <typename Visit>
[[nodiscard]] std::size_t measured_bytes(
    const SensorPresentation& settings, bool dashboard, const Visit& visit) {
    CommandOutput counter{[](void*, std::string_view) { return true; }, nullptr};
    FieldWriter writer{counter, settings, dashboard};
    if (!visit(writer))
        return SIZE_MAX / 2;
    return writer.open ? counter.size() + 2U : 0U; // the closing "},"
}

// Room in a canonical message for its wrapper and the unit members.
constexpr std::size_t kFieldsLimit = CanonicalMessage::kPayloadCapacity - 64U;
} // namespace

bool SensorService::write_fields(
    CommandOutput& output, const std::size_t reserved, bool* temperature_unit) const {
    static std::size_t omitted_logged = 0;
    FieldWriter context{output, presentation_, false};
    const auto start = output.size();
    std::size_t omitted = 0;

    for (const auto& driver : drivers) {
        if (!settings_.enabled(driver.id))
            continue;
        if (!has_device_walk(driver)) {
            if (!driver.hooks->visit_readings(write_field, &context))
                return false;
            continue;
        }
        const auto count = driver.hooks->device_count(false);
        for (std::size_t index = 0; index < count; ++index) {
            const auto visit = [&](FieldWriter& writer) {
                return driver.hooks->visit_device(index, false, write_field, &writer);
            };
            const auto bytes = measured_bytes(presentation_, false, visit);
            if (bytes == 0)
                continue;
            if (omitted != 0 || output.size() - start + bytes + reserved > kFieldsLimit) {
                ++omitted;
                continue;
            }
            if (!visit(context))
                return false;
        }
    }
    if (omitted != omitted_logged) {
        if (omitted != 0)
            AddLog(LogLevel::info, "SNS: %u devices left out, message limit %u", unsigned(omitted),
                unsigned(CanonicalMessage::kPayloadCapacity));
        omitted_logged = omitted;
    }
    if (temperature_unit)
        *temperature_unit = context.temperature;
    return finish_fields(context);
}

bool SensorService::write_temperature_unit(CommandOutput& output) const {
    return output.write(",\"TempUnit\":") &&
           output.json_string(presentation_.fahrenheit ? "F" : "C");
}

bool SensorService::write_updates(std::size_t viewer, const bool everything, CommandOutput& output,
    const std::size_t capacity, bool& more, bool& reset) {
    viewer %= kViewers;
    auto& dirty = viewer_dirty[viewer];
    more = false;
    reset = viewer_reset[viewer] && !everything;
    viewer_reset[viewer] = false;
    FieldWriter writer{output, presentation_, true};

    for (std::size_t index = 0; index < drivers.size() && !more; ++index) {
        const auto& driver = drivers[index];
        if (!settings_.enabled(driver.id))
            continue;
        // Sends one marked sensor when it fits; a sensor that does not fit waits with the rest.
        const auto send = [&](const std::uint64_t bit, const auto& visit) {
            if (!(dirty & bit))
                return true;
            const auto bytes = measured_bytes(presentation_, true, visit);
            if (bytes != 0 && output.size() + bytes > capacity) {
                more = true;
                return true;
            }
            if (bytes != 0 && !visit(writer))
                return false;
            dirty &= ~bit;
            return true;
        };
        if (has_device_walk(driver)) {
            const auto count =
                std::min<std::size_t>(driver.hooks->device_count(true), kLocalDriverBit);
            if (everything)
                dirty |= (std::uint64_t{1} << count) - 1U;
            for (std::size_t slot = 0; slot < count && !more; ++slot) {
                const auto visit = [&](FieldWriter& target) {
                    return driver.hooks->visit_device(slot, true, write_field, &target);
                };
                if (!send(std::uint64_t{1} << slot, visit))
                    return false;
            }
        } else {
            const auto bit = std::uint64_t{1} << (kLocalDriverBit + index);
            if (everything)
                dirty |= bit;
            const auto visit = [&](FieldWriter& target) {
                return driver.hooks->visit_readings(write_field, &target);
            };
            if (!send(bit, visit))
                return false;
        }
    }
    return finish_fields(writer);
}

bool SensorService::write_readings(CommandOutput& output) const {
    struct Context {
        CommandOutput& output;
        const SensorPresentation& settings;
        char sensor[kSensorNameCapacity]{};
        Ambient ambient{};
        bool first{true};
    } context{output, presentation_};

    if (!output.write("{\"readings\":["))
        return false;
    for (const auto& driver : drivers) {
        if (!settings_.enabled(driver.id))
            continue;
        if (!driver.hooks->visit_readings(
                [](void* opaque, const SensorReading& reading) {
                    auto& c = *static_cast<Context*>(opaque);
                    if (!reading.sensor || std::strlen(reading.sensor) >= sizeof(c.sensor))
                        return false;
                    std::int64_t value = 0;
                    unsigned precision = 0;
                    if (!c.ambient.project(reading, c.settings, value, precision))
                        return true;
                    if (!c.first && !c.output.write(","))
                        return false;
                    c.first = false;
                    return c.output.write("{\"sensor\":") && c.output.json_string(reading.sensor) &&
                           c.output.write(",\"field\":") && c.output.json_string(reading.field) &&
                           c.output.write(",\"label\":") && c.output.json_string(reading.field) &&
                           c.output.write(",\"unit\":") &&
                           c.output.json_string(reading_unit(reading, c.settings)) &&
                           c.output.write(",\"value\":") && number(c.output, value, precision) &&
                           // Dew point is derived and keeps no history to point at.
                           (reading.quantity == SensorQuantity::dew_point || reading.no_history ||
                               c.output.format(",\"series\":%lu",
                                   static_cast<unsigned long>(
                                       sensor_series_key(reading.sensor, reading.field)))) &&
                           c.output.write("}");
                },
                &context))
            return false;
    }
    return output.write("]}");
}

bool SensorService::write_value(
    CommandOutput& output, SensorQuantity quantity, std::int32_t value) const {
    if (quantity == SensorQuantity::dew_point)
        return false;
    std::int64_t projected = 0;
    unsigned precision = 0;
    const SensorReading reading{"", "", quantity, value};
    return project_reading(reading, presentation_, 0, 0, false, false, projected, precision) &&
           number(output, projected, precision);
}

bool SensorService::publish_event(const char* sensor, const SensorReading* readings,
    std::size_t count, std::string_view members) const {
    if (event_sink_ == nullptr || sensor == nullptr || std::strlen(sensor) >= kSensorNameCapacity)
        return false;

    struct Sink {
        std::array<char, kEventCapacity> data;
        std::size_t size;
    } sink{};

    CommandOutput output{[](void* context, std::string_view text) {
                             auto& sink = *static_cast<Sink*>(context);
                             if (text.size() > sink.data.size() - sink.size)
                                 return false;
                             std::memcpy(sink.data.data() + sink.size, text.data(), text.size());
                             sink.size += text.size();
                             return true;
                         },
        &sink};
    Ambient ambient{};
    bool okay = output.write("{") && output.json_string(sensor) && output.write(":{");
    bool first = true, temperature = false;
    for (std::size_t i = 0; okay && i < count; ++i) {
        auto reading = readings[i];
        reading.sensor = sensor;
        std::int64_t value = 0;
        unsigned precision = 0;
        if (!ambient.project(reading, presentation_, value, precision))
            continue;
        temperature |= reading.quantity == SensorQuantity::temperature ||
                       reading.quantity == SensorQuantity::dew_point;
        okay = (first || output.write(",")) && output.json_string(reading.field) &&
               output.write(":") && number(output, value, precision);
        first = false;
    }
    if (okay && !members.empty())
        okay = (first || output.write(",")) && output.write(members);
    okay = okay && output.write("}");
    if (okay && temperature)
        okay = output.write(",\"TempUnit\":") &&
               output.json_string(presentation_.fahrenheit ? "F" : "C");
    return okay && output.write("}") && event_sink_(event_context_, {sink.data.data(), sink.size});
}

/** @brief Serializes one complete bounded canonical sensor object. */
bool SensorService::serialize(
    char* output, std::size_t capacity, std::size_t& size, bool* const capacity_exceeded) const {
    size = 0;
    if (capacity_exceeded)
        *capacity_exceeded = false;
    if (!output || capacity == 0)
        return false;
    output[0] = '\0';

    struct Sink {
        char* output;
        std::size_t capacity;
        std::size_t* size;
        bool* capacity_exceeded;
    } sink{output, capacity, &size, capacity_exceeded};

    CommandOutput writer{[](void* context, std::string_view text) {
                             auto& sink = *static_cast<Sink*>(context);
                             if (text.size() >= sink.capacity - *sink.size) {
                                 if (sink.capacity_exceeded)
                                     *sink.capacity_exceeded = true;
                                 return false;
                             }
                             std::memcpy(sink.output + *sink.size, text.data(), text.size());
                             *sink.size += text.size();
                             sink.output[*sink.size] = '\0';
                             return true;
                         },
        &sink};
    if (!writer.write("{") || !write_fields(writer) || !writer.write("}")) {
        size = 0;
        output[0] = '\0';
        return false;
    }
    return true;
}
} // namespace newmota
