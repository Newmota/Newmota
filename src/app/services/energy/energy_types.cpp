/*
  energy_types.cpp - Portable energy resource identity operations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_types.cpp
 * @brief Portable energy resource identity operations.
 */

#include "services/energy/energy_types.h"
#include "support/text.h"
#include <array>

namespace newmota::energy {
namespace {
constexpr std::array<const char*, 30> names{"none", "rx", "tx", "cf", "cf1", "sel", "irq", "reset",
    "cs", "miso", "mosi", "sck", "adc", "de", "cs2", "irq2", "v1", "v2", "v3", "v4", "v5", "v6",
    "v7", "i1", "i2", "i3", "i4", "i5", "i6", "i7"};
}

const char* pin_function_name(PinFunction function) {
    const auto index = static_cast<std::size_t>(function);
    return index < names.size() ? names[index] : "invalid";
}

PinFunction parse_pin_function(const char* text, std::size_t size) {
    for (std::size_t index = 1; index < names.size(); ++index)
        if (support::ascii_iequals({text, size}, names[index]))
            return static_cast<PinFunction>(index);
    return PinFunction::none;
}

PinId PhysicalIdentity::pin(PinFunction function) const {
    for (const auto& binding : pins)
        if (binding.function == function)
            return binding.pin;
    return {};
}

bool PhysicalIdentity::valid() const {
    for (std::size_t i = 0; i < pins.size(); ++i) {
        const auto& binding = pins[i];
        if (unsigned(binding.function) >= names.size() ||
            (binding.function == PinFunction::none) == binding.pin.valid())
            return false;
        if (binding.function == PinFunction::none)
            continue;
        for (std::size_t j = 0; j < i; ++j)
            if (pins[j].function == binding.function || pins[j].pin.value() == binding.pin.value())
                return false;
    }
    return true;
}

bool PhysicalIdentity::bind(PinFunction function, PinId pin) {
    if (function == PinFunction::none || unsigned(function) >= names.size())
        return false;
    auto candidate = *this;
    PinBinding* target{};
    for (auto& binding : candidate.pins) {
        if (binding.function == function) {
            target = &binding;
            break;
        }
        if (!target && binding.function == PinFunction::none)
            target = &binding;
    }
    if (!target)
        return !pin.valid();
    *target = pin.valid() ? PinBinding{function, pin} : PinBinding{};
    if (!candidate.valid())
        return false;
    *this = candidate;
    return true;
}

bool PhysicalIdentity::operator==(const PhysicalIdentity& other) const {
    if (driver != other.driver || bus != other.bus || address != other.address)
        return false;
    for (std::size_t role = 1; role < names.size(); ++role)
        if (pin(static_cast<PinFunction>(role)).value() !=
            other.pin(static_cast<PinFunction>(role)).value())
            return false;
    return true;
}

bool PhysicalIdentity::has_resources() const {
    if (bus || address)
        return true;
    for (const auto& binding : pins)
        if (binding.pin.valid())
            return true;
    return false;
}

} // namespace newmota::energy
