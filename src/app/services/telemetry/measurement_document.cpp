/*
  measurement_document.cpp - Composition of independent measurement owners

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file measurement_document.cpp
 * @brief Composition of independent measurement owners.
 */

#include "services/telemetry/measurement_document.h"
#include "core/command/command.h"
#include "newmota_features.h"
#if USE_SENSORS
#include "drivers/sensors/sensor_service.h"
#endif
#if USE_ENERGY
#include "services/energy/energy_service.h"
#endif
#include <cstring>

namespace newmota::telemetry {
#if USE_SWITCHES
namespace {
bool (*switch_writer)(CommandOutput&){};
} // namespace

void bind_switch_fields(bool (*writer)(CommandOutput&)) {
    switch_writer = writer;
}
#endif

bool write_measurement_fields(CommandOutput& output, bool period) {
    (void)period;
    const auto start = output.size();
#if USE_SWITCHES
    // Switch states lead, as in Tasmota's MqttShowSensor.
    if (switch_writer != nullptr && !switch_writer(output))
        return false;
#endif
    const auto leading = output.size() - start;
#if USE_ENERGY
    // As Tasmota does for the whole message, a Temperature key in ENERGY earns a TempUnit. Keys
    // are written in one piece, so each written part can be searched on its own.
    bool energy_temperature = false;
    CommandOutput counter{[](void* found, std::string_view text) {
                              if (text.find("\"Temperature\"") != std::string_view::npos)
                                  *static_cast<bool*>(found) = true;
                              return true;
                          },
        &energy_temperature};
    if (!energy::service().write_fields(counter, period))
        return false;
    const bool energy = counter.size() != 0;
    bool temperature_unit = false;
#endif
#if USE_SENSORS
    // Sensors follow earlier fields after a comma, written only once they write something; the
    // earlier fields come out of their budget.
    struct Separated {
        CommandOutput* output;
        bool pending;
    } separated{&output, leading != 0U};

    CommandOutput sensor_output{[](void* context, std::string_view text) {
                                    auto& target = *static_cast<Separated*>(context);
                                    if (target.pending && !text.empty()) {
                                        if (!target.output->write(","))
                                            return false;
                                        target.pending = false;
                                    }
                                    return target.output->write(text);
                                },
        &separated};
    const std::size_t prefix = leading != 0U ? leading + 1U : 0U;
#if USE_ENERGY
    // Energy follows in the same message, so remote sensors only get the room it leaves.
    const std::size_t reserved =
        prefix + (energy ? counter.size() + 1U + (energy_temperature ? 16U : 0U) : 0U);
    if (!sensor_service().write_fields(sensor_output, reserved, &temperature_unit))
        return false;
#else
    if (!sensor_service().write_fields(sensor_output, prefix))
        return false;
#endif
#endif
#if USE_ENERGY
    if (energy && output.size() != start && !output.write(","))
        return false;
    if (energy && !energy::service().write_fields(output, period))
        return false;
#if USE_SENSORS
    if (energy_temperature && !temperature_unit && !sensor_service().write_temperature_unit(output))
        return false;
#endif
#endif
    (void)leading;
    return true;
}

bool serialize_measurements(
    char* output, std::size_t capacity, std::size_t& size, bool* capacity_exceeded, bool period) {
    size = 0;
    if (capacity_exceeded)
        *capacity_exceeded = false;
    if (!output || !capacity)
        return false;
    output[0] = '\0';

    struct Sink {
        char* data;
        std::size_t capacity, size{};
        bool overflow{};
    } sink{output, capacity};

    CommandOutput writer{[](void* context, std::string_view text) {
                             auto& sink = *static_cast<Sink*>(context);
                             if (text.size() >= sink.capacity - sink.size) {
                                 sink.overflow = true;
                                 return false;
                             }
                             std::memcpy(sink.data + sink.size, text.data(), text.size());
                             sink.size += text.size();
                             sink.data[sink.size] = '\0';
                             return true;
                         },
        &sink};
    if (!writer.write("{") || !write_measurement_fields(writer, period) || !writer.write("}")) {
        output[0] = '\0';
        if (capacity_exceeded)
            *capacity_exceeded = sink.overflow;
        return false;
    }
    size = sink.size;
    return true;
}
} // namespace newmota::telemetry
