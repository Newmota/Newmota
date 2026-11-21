/*
  measurement_document.h - Composition of independent measurement owners

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file measurement_document.h
 * @brief Composition of independent measurement owners.
 */

#pragma once
#include "newmota_features.h"

#include <cstddef>

namespace newmota {
class CommandOutput;

namespace telemetry {
/** @brief Streams selected sensor and energy owners as fields of one measurement object.
 * @param output Destination for the JSON object fields.
 * @param period Includes a previously captured publication-only energy interval preview.
 * @return true when every selected owner wrote its fields successfully.
 */
bool write_measurement_fields(CommandOutput& output, bool period = false);
#if USE_SWITCHES
/** @brief Binds the writer of the switch states that lead the fields, as in Tasmota's
 * MqttShowSensor; null unbinds it.
 * @param writer Writes `"Switch1":"ON"` style fields without a leading comma.
 */
void bind_switch_fields(bool (*writer)(CommandOutput& output));
#endif
/** @brief Serializes atomically into bounded storage, returning empty output on overflow.
 * @param output Destination buffer; its first byte is cleared before serialization.
 * @param capacity Number of bytes available in output, including the trailing NUL.
 * @param size Receives the serialized byte length on success, otherwise zero.
 * @param capacity_exceeded Optional flag distinguishing overflow from a writer failure.
 * @param period Includes the captured publication-only energy preview; default reads are pure.
 * @return true when a complete JSON object and trailing NUL fit in output.
 */
bool serialize_measurements(char* output, std::size_t capacity, std::size_t& size,
    bool* capacity_exceeded = nullptr, bool period = false);
} // namespace telemetry
} // namespace newmota
