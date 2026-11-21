/*
  sensor_service.h - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file sensor_service.h
 * @brief Owns canonical sensor observations and independent presentation/visitor routes.
 */
#pragma once
#include "drivers/sensors/sensor_settings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {
class CommandOutput;
class Hardware;

// Drivers retain their own samples. Invalid samples are omitted immediately.
struct SensorSample {
    std::uint8_t bus{}, address{};
    std::int32_t value{}; // Signed milli-units.
    bool valid{};
    bool detected{};
};

// Values are signed milli-units in canonical driver units: lx, Celsius, percent relative humidity,
// hPa, percent battery charge, percent soil moisture, µS/cm, kg, ohm, dBm, seconds since the
// last observation, 0/1 for a binary state (motion, door, leak), V, A, W, kWh, ppm, µg/m³ for
// volatile organic compounds and particulates, m, a plain count, the UV index, VA, var, a power
// factor, Hz, and a value in the unit the reading names itself.
enum class SensorQuantity : std::uint8_t {
    illuminance,
    temperature,
    humidity,
    pressure,
    dew_point,
    battery,
    moisture,
    conductivity,
    weight,
    impedance,
    signal_strength,
    age,
    state,
    voltage,
    current,
    power,
    energy,
    carbon_dioxide,
    volatile_organic,
    particulate,
    distance,
    count,
    uv_index,
    apparent_power,
    reactive_power,
    power_factor,
    frequency,
    other
};

/** @brief Bytes for a sensor name including its terminator; a longer name fails the visit. */
inline constexpr std::size_t kSensorNameCapacity = 32;

struct SensorReading {
    const char* sensor;
    const char* field;
    SensorQuantity quantity;
    std::int32_t value;    // Signed milli-units, never converted by drivers.
    bool dashboard_only{}; // Browser readings only; never StatusSNS or MQTT SENSOR fields.
    // Preformatted JSON members after this field in StatusSNS and MQTT SENSOR; lives for the call.
    const char* members{};
    bool no_history{}; // Keeps the reading out of the 24-hour dashboard history.
    // For SensorQuantity::other: the unit shown with the value, and its decimals (at most three).
    const char* unit{};
    std::uint8_t decimals{};
};

struct SensorPresentation {
    std::uint8_t temperature_resolution{1}, humidity_resolution{1}, pressure_resolution{1};
    bool fahrenheit{}, pressure_conversion{}, pressure_inch{};
    // Tasmota offsets in tenths; temperature offset uses the selected output unit.
    std::int16_t temperature_offset{}, humidity_offset{};
    // EnergyRes, WattRes, VoltRes, AmpRes and FreqRes, shared with the energy meter as in Tasmota.
    std::uint8_t energy_resolution{3}, wattage_resolution{}, voltage_resolution{},
        current_resolution{3}, frequency_resolution{};
};

using SensorReadingVisitor = bool (*)(void*, const SensorReading&);
/** @brief Copies one complete unsolicited sensor message; false when it cannot be taken now. */
using SensorEventSink = bool (*)(void*, std::string_view);

struct SensorDriverHooks {
    void (*reset)();
    void (*initialize)(std::uint32_t, const Hardware*);
    void (*poll)(std::uint32_t);
    // Emit each device's fields contiguously; names/fields live through the visitor call.
    bool (*visit_readings)(SensorReadingVisitor, void*);
    // Optional, for a remote driver with many devices. With `every` false these walk the devices
    // shown in StatusSNS and MQTT; with it true, every registry slot for the dashboard.
    std::size_t (*device_count)(bool every){};
    bool (*visit_device)(std::size_t index, bool every, SensorReadingVisitor, void*){};
};

struct SensorDriverDescriptor {
    std::uint8_t id;
    const char* name;
    const char* output_field;
    const char* output_routes;
    const SensorDriverHooks* hooks;
    bool remote; ///< Measures somewhere else (radio); never feeds the global temperature.
};

class SensorService {
  public:
    /** @brief Initializes only compiled sensor drivers. */
    void initialize(
        std::uint32_t now, const SensorSettings& settings = {}, const Hardware* hardware = nullptr);
    /** @brief Returns a compiled driver by enumeration position, or nullptr. */
    [[nodiscard]] const SensorDriverDescriptor* driver(std::size_t index) const;
    /** @brief Applies persisted policy, clearing disabled samples before any further output. */
    void configure(const SensorSettings& settings, std::uint32_t now);
    /** @brief Refreshes sensor data on the cooperative scheduler. */
    void poll(std::uint32_t now);

    /** @brief Applies output units and precision without altering canonical samples. */
    void set_presentation(const SensorPresentation& presentation);

    /** @brief Visits current canonical readings in the same registry/device order as output.
     * @param visitor Synchronous consumer; false stops enumeration without modifying samples.
     * @param context Caller-owned state, retained only during this call.
     * @param local_only Skips remote drivers, for consumers of the device's own ambient values.
     * @return true after all enabled drivers, false on invalid visitor or consumer failure.
     */
    [[nodiscard]] bool visit_readings(
        SensorReadingVisitor visitor, void* context, bool local_only = false) const;

    /** @brief Writes valid retained sensor fields without outer braces.
     * @param reserved Bytes the caller still appends to the same bounded message. Remote devices
     *        that would not fit are left out, the first of them ending the list.
     * @param temperature_unit Set when the fields end with TempUnit.
     */
    [[nodiscard]] bool write_fields(
        CommandOutput& output, std::size_t reserved = 0, bool* temperature_unit = nullptr) const;
    /** @brief Appends `,"TempUnit":"C"` or `"F"`, for a temperature another owner reported. */
    [[nodiscard]] bool write_temperature_unit(CommandOutput& output) const;
    /** @brief Returns a counter that advances whenever a sensor has new data. */
    [[nodiscard]] std::uint32_t change_revision() const;
    /** @brief Marks one remote device as changed for every dashboard viewer. */
    void mark_device_dirty(std::size_t slot);
    /** @brief Writes the changed sensors of one viewer as StatusSNS members, without outer braces.
     * @param viewer Web session index; the mask stays with it between calls.
     * @param everything Marks every sensor first, for a page that draws all of them.
     * @param capacity Total size @p output may reach; sensors beyond it wait for the next call.
     * @param more Set when changed sensors are still waiting.
     * @param reset Set when the page's sensor set may be out of date, so it asks for everything.
     */
    [[nodiscard]] bool write_updates(std::size_t viewer, bool everything, CommandOutput& output,
        std::size_t capacity, bool& more, bool& reset);
    /** @brief Writes valid readings with display metadata for generic consumers. */
    [[nodiscard]] bool write_readings(CommandOutput& output) const;

    /** @brief Writes one value with the units, offsets and precision of StatusSNS.
     * @param value Signed milli-units in the quantity's canonical unit.
     * @return False for a derived quantity (dew point) or when the output is full.
     */
    [[nodiscard]] bool write_value(
        CommandOutput& output, SensorQuantity quantity, std::int32_t value) const;

    /** @brief Installs the copying sink for unsolicited sensor messages; nullptr disables them.
     * @param context Caller-owned state, retained until replaced.
     */
    void set_event_sink(SensorEventSink sink, void* context) {
        event_sink_ = sink;
        event_context_ = context;
    }

    /** @brief Publishes one device's event as `{"<sensor>":{...}}` through the event sink.
     * @param readings Numeric fields, projected with the same units and precision as StatusSNS;
     *        their sensor name is replaced by @p sensor.
     * @param members Preformatted JSON members without braces, appended after the readings.
     * @return true once the sink copied the message; false without a sink, on overflow or when
     *         the transport cannot take it now, so the caller may retry.
     */
    [[nodiscard]] bool publish_event(const char* sensor, const SensorReading* readings,
        std::size_t count, std::string_view members) const;
    /** @brief Serializes complete bounded canonical sensor JSON. */
    [[nodiscard]] bool serialize(char* output, std::size_t capacity, std::size_t& size,
        bool* capacity_exceeded = nullptr) const;

  private:
    static constexpr std::size_t kEventCapacity = 512;
    SensorSettings settings_{};
    SensorPresentation presentation_{};
    const Hardware* hardware_{};
    SensorEventSink event_sink_{};
    void* event_context_{};
};

/** @brief Identifies one reading's history series: FNV-1a over sensor, a separator and field.
 *  Stable while the sensor name is; never 0.
 */
[[nodiscard]] std::uint32_t sensor_series_key(const char* sensor, const char* field);

/** @brief Returns the singleton sensor owner. */
SensorService& sensor_service();
} // namespace newmota
