/*
  light_module.cpp - Berry binding for the Light owner service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Berry explicit-power source reference: Tasmota xdrv_52_3_berry_light.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file light_module.cpp
 * @brief Berry binding for the Light owner service.
 */

#include "newmota_features.h"

#if USE_PWM || USE_WS2812

#include "services/light/berry/light_module.h"

#include "services/light/light_api.h"
#include "services/light/light_service.h"

extern "C" {
#include "berry.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

newmota::LightService* light_service = nullptr;

struct GammaPoint final {
    std::uint16_t input;
    std::uint16_t output;
};

// Tasmota's script curve is separate from the Light owner's PWM transform.
constexpr std::array<GammaPoint, 9> gamma_points{{
    {1U, 1U},
    {4U, 1U},
    {209U, 13U},
    {312U, 41U},
    {457U, 106U},
    {626U, 261U},
    {762U, 450U},
    {895U, 703U},
    {1023U, 1023U},
}};

std::uint16_t gamma_scale(const std::uint16_t value, const std::uint16_t from_min,
    const std::uint16_t from_max, const std::uint16_t to_min, const std::uint16_t to_max) {
    if (from_min >= from_max || value <= from_min)
        return to_min;
    if (value >= from_max)
        return to_max;
    const std::uint32_t offset = value - from_min;
    const std::uint32_t span = from_max - from_min;
    const std::uint32_t delta = to_max - to_min;
    // Match changeUIntScale's distinct expansion and contraction rounding.
    const std::uint32_t scaled = delta > span
                                     ? ((offset * delta * 2U / span) + 1U) / 2U
                                     : (offset * 2U + 1U) * (delta + 1U) / ((span + 1U) * 2U);
    return static_cast<std::uint16_t>(to_min + scaled);
}

std::uint16_t gamma_curve(const std::uint16_t value, const bool reverse) {
    GammaPoint previous{0U, 0U};
    for (const auto& point : gamma_points) {
        const auto from_max = reverse ? point.output : point.input;
        if (value <= from_max)
            return gamma_scale(value, reverse ? previous.output : previous.input, from_max,
                reverse ? previous.input : previous.output, reverse ? point.input : point.output);
        previous = point;
    }
    return 1023U;
}

std::uint16_t gamma_argument(bvm* const vm, const std::uint16_t maximum) {
    if (be_top(vm) != 1 || !be_isint(vm, 1))
        be_raise(vm, "type_error", "light gamma expects one integer");
    const bint value = be_toint(vm, 1);
    return value < 0 ? 0U : value > maximum ? maximum : static_cast<std::uint16_t>(value);
}

int LightGamma10(bvm* const vm) {
    be_pushint(vm, gamma_curve(gamma_argument(vm, 1023U), false));
    be_return(vm);
}

int LightReverseGamma10(bvm* const vm) {
    be_pushint(vm, gamma_curve(gamma_argument(vm, 1023U), true));
    be_return(vm);
}

int LightGamma8(bvm* const vm) {
    const auto value = gamma_scale(gamma_argument(vm, 255U), 0U, 255U, 0U, 1023U);
    const auto corrected = gamma_curve(value, false);
    be_pushint(vm, corrected == 0U ? 0U : gamma_scale(corrected, 4U, 1023U, 1U, 255U));
    be_return(vm);
}

enum class Field : std::uint8_t {
    power,
    applied_power,
    brightness,
    hue,
    saturation,
    color_temperature,
    rgb,
    channels,
    color_mode,
};

struct FieldName final {
    const char* name;
    Field field;
};

constexpr std::array<FieldName, 9> fields{{
    {"power", Field::power},
    {"applied_power", Field::applied_power},
    {"bri", Field::brightness},
    {"hue", Field::hue},
    {"sat", Field::saturation},
    {"ct", Field::color_temperature},
    {"rgb", Field::rgb},
    {"channels", Field::channels},
    {"colormode", Field::color_mode},
}};

bool find_field(const char* const name, const std::size_t length, Field& field) {
    for (const auto& candidate : fields) {
        if (std::strlen(candidate.name) == length &&
            std::memcmp(candidate.name, name, length) == 0) {
            field = candidate.field;
            return true;
        }
    }
    return false;
}

bool push_map_value(bvm* const vm, const int map_index, const char* const key) {
    if (!be_getmember(vm, map_index, ".p"))
        return false;
    const int data_index = be_absindex(vm, -1);
    be_pushstring(vm, key);
    const bool found = be_getindex(vm, data_index);
    be_remove(vm, -2); // key
    be_remove(vm, -2); // internal map
    if (!found) {
        be_pop(vm, 1);
        return false;
    }
    return true;
}

void push_list(bvm* const vm, const std::uint8_t* const values, const std::uint8_t count) {
    be_newobject(vm, "list");
    const int data_index = be_absindex(vm, -1);
    for (std::uint8_t i = 0U; i < count; ++i) {
        be_pushint(vm, static_cast<bint>(values[i]));
        be_data_push(vm, data_index);
        be_pop(vm, 1);
    }
    be_remove(vm, data_index);
}

void insert_field(bvm* const vm, const int data_index, const char* const key, const Field field,
    const newmota::LightApiSnapshot& snapshot) {
    be_pushstring(vm, key);
    switch (field) {
    case Field::power:
        be_pushbool(vm, snapshot.power);
        break;
    case Field::applied_power:
        be_pushbool(vm, snapshot.applied_power);
        break;
    case Field::brightness:
        be_pushint(vm, static_cast<bint>(snapshot.brightness));
        break;
    case Field::hue:
        be_pushint(vm, static_cast<bint>(snapshot.hue));
        break;
    case Field::saturation:
        be_pushint(vm, static_cast<bint>(snapshot.saturation));
        break;
    case Field::color_temperature:
        be_pushint(vm, static_cast<bint>(snapshot.color_temperature));
        break;
    case Field::rgb:
        be_pushstring(vm, snapshot.rgb.data());
        break;
    case Field::channels:
        push_list(vm, snapshot.channels.data(), snapshot.channel_count);
        break;
    case Field::color_mode:
        be_pushstring(vm, snapshot.color_mode_rgb ? "rgb" : "ct");
        break;
    }
    (void)be_data_insert(vm, data_index);
    be_pop(vm, 2); // key and value; keep the underlying map
}

void push_snapshot(bvm* const vm, const newmota::LightApiSnapshot& snapshot) {
    be_newobject(vm, "map");
    const int data_index = be_absindex(vm, -1);
    insert_field(vm, data_index, "power", Field::power, snapshot);
    insert_field(vm, data_index, "applied_power", Field::applied_power, snapshot);
    insert_field(vm, data_index, "bri", Field::brightness, snapshot);
    if (snapshot.has_rgb) {
        insert_field(vm, data_index, "hue", Field::hue, snapshot);
        insert_field(vm, data_index, "sat", Field::saturation, snapshot);
        insert_field(vm, data_index, "rgb", Field::rgb, snapshot);
    }
    if (snapshot.has_color_temperature)
        insert_field(vm, data_index, "ct", Field::color_temperature, snapshot);
    if (snapshot.has_color_mode)
        insert_field(vm, data_index, "colormode", Field::color_mode, snapshot);
    insert_field(vm, data_index, "channels", Field::channels, snapshot);
    be_remove(vm, data_index);
}

void push_field(bvm* const vm, const Field field, const newmota::LightApiSnapshot& snapshot) {
    switch (field) {
    case Field::power:
        be_pushbool(vm, snapshot.power);
        break;
    case Field::applied_power:
        be_pushbool(vm, snapshot.applied_power);
        break;
    case Field::brightness:
        be_pushint(vm, static_cast<bint>(snapshot.brightness));
        break;
    case Field::hue:
        if (snapshot.has_rgb)
            be_pushint(vm, static_cast<bint>(snapshot.hue));
        else
            be_pushnil(vm);
        break;
    case Field::saturation:
        if (snapshot.has_rgb)
            be_pushint(vm, static_cast<bint>(snapshot.saturation));
        else
            be_pushnil(vm);
        break;
    case Field::color_temperature:
        if (snapshot.has_color_temperature)
            be_pushint(vm, static_cast<bint>(snapshot.color_temperature));
        else
            be_pushnil(vm);
        break;
    case Field::rgb:
        if (snapshot.has_rgb)
            be_pushstring(vm, snapshot.rgb.data());
        else
            be_pushnil(vm);
        break;
    case Field::channels:
        push_list(vm, snapshot.channels.data(), snapshot.channel_count);
        break;
    case Field::color_mode:
        if (snapshot.has_color_mode)
            be_pushstring(vm, snapshot.color_mode_rgb ? "rgb" : "ct");
        else
            be_pushnil(vm);
        break;
    }
}

bool parse_index(bvm* const vm, const int argument, std::uint8_t& endpoint) {
    if (!be_isint(vm, argument))
        return false;
    const bint value = be_toint(vm, argument);
    if (value < 0 || value > 255)
        return false;
    endpoint = static_cast<std::uint8_t>(value);
    return true;
}

bool parse_unsigned(
    bvm* const vm, const int argument, const std::uint16_t maximum, std::uint16_t& value) {
    if (!be_isint(vm, argument))
        be_raise(vm, "type_error", "light values must be integers");
    const bint parsed = be_toint(vm, argument);
    if (parsed < 0 || parsed > maximum)
        be_raise(vm, "value_error", "light integer is outside its valid range");
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_map_integer(bvm* const vm, const int map_index, const char* const key,
    const std::uint16_t maximum, bool& present, std::uint16_t& value) {
    const int original_top = be_top(vm);
    const bool found = push_map_value(vm, map_index, key);
    present = found;
    value = 0U;
    if (found)
        (void)parse_unsigned(vm, -1, maximum, value);
    const int extra = be_top(vm) - original_top;
    if (extra > 0)
        be_pop(vm, extra);
    return true;
}

void parse_rgb(bvm* const vm, const int map_index, newmota::LightApiMutation& mutation) {
    const bool found = push_map_value(vm, map_index, "rgb");
    mutation.set_rgb = found;
    if (found) {
        if (!be_isstring(vm, -1))
            be_raise(vm, "type_error", "light rgb must be a hex string");
        const char* text = be_tostring(vm, -1);
        std::size_t length = static_cast<std::size_t>(be_strlen(vm, -1));
        if (length == 7U && text[0] == '#') {
            ++text;
            --length;
        }
        if (length != 6U)
            be_raise(vm, "value_error", "light rgb must contain six hex digits");
        for (std::size_t i = 0U; i < mutation.rgb.size(); ++i) {
            const auto digit = [](const char ch) -> int {
                if (ch >= '0' && ch <= '9')
                    return ch - '0';
                if (ch >= 'a' && ch <= 'f')
                    return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F')
                    return ch - 'A' + 10;
                return -1;
            };
            const int high = digit(text[i * 2U]);
            const int low = digit(text[i * 2U + 1U]);
            if (high < 0 || low < 0)
                be_raise(vm, "value_error", "light rgb contains a non-hex digit");
            mutation.rgb[i] = static_cast<std::uint8_t>(high * 16 + low);
        }
    }
    if (found)
        be_pop(vm, 1);
}

void parse_channels(bvm* const vm, const int map_index, newmota::LightApiMutation& mutation) {
    const bool found = push_map_value(vm, map_index, "channels");
    mutation.set_channels = found;
    if (!found)
        return;
    if (!be_islistinstance(vm, -1))
        be_raise(vm, "type_error", "light channels must be a list");
    const int list_index = be_absindex(vm, -1);
    if (!be_getmember(vm, list_index, ".p") || !be_islist(vm, -1))
        be_raise(vm, "type_error", "light channels must be a list");
    const int data_index = be_absindex(vm, -1);
    const int count = be_data_size(vm, data_index);
    if (count < 0 || count > static_cast<int>(mutation.channels.size()))
        be_raise(vm, "value_error", "light channels list has an invalid length");
    mutation.channel_count = static_cast<std::uint8_t>(count);
    for (int i = 0; i < count; ++i) {
        be_pushint(vm, static_cast<bint>(i));
        (void)be_getindex(vm, data_index);
        std::uint16_t value = 0U;
        (void)parse_unsigned(vm, -1, 255U, value);
        mutation.channels[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
        be_pop(vm, 2);
    }
    be_pop(vm, 2); // underlying list and list instance
}

void parse_mutation(bvm* const vm, const int map_index, newmota::LightApiMutation& mutation) {
    if (push_map_value(vm, map_index, "power")) {
        if (!be_isbool(vm, -1))
            be_raise(vm, "type_error", "light power must be a boolean");
        mutation.set_power = true;
        mutation.power = be_tobool(vm, -1);
        be_pop(vm, 1);
    }
    std::uint16_t value = 0U;
    (void)parse_map_integer(vm, map_index, "ct", 500U, mutation.set_color_temperature, value);
    if (mutation.set_color_temperature) {
        if (value < 153U)
            be_raise(vm, "value_error", "light ct must be between 153 and 500");
        mutation.color_temperature = value;
    }
    (void)parse_map_integer(vm, map_index, "hue", 360U, mutation.set_hue, value);
    mutation.hue = value;
    (void)parse_map_integer(vm, map_index, "sat", 255U, mutation.set_saturation, value);
    mutation.saturation = static_cast<std::uint8_t>(value);
    parse_rgb(vm, map_index, mutation);
    parse_channels(vm, map_index, mutation);
    (void)parse_map_integer(vm, map_index, "bri", 255U, mutation.set_brightness, value);
    mutation.brightness = static_cast<std::uint8_t>(value);
}

void raise_api_error(bvm* const vm, const newmota::LightApiError error) {
    switch (error) {
    case newmota::LightApiError::unavailable:
        be_raise(vm, "runtime_error", "Light service is unavailable");
        break;
    case newmota::LightApiError::invalid_endpoint:
        be_raise(vm, "value_error", "Light endpoint index is out of range");
        break;
    case newmota::LightApiError::invalid_request:
        be_raise(vm, "value_error", "Light update is not supported by this endpoint");
        break;
    case newmota::LightApiError::hardware_failure:
        be_raise(vm, "runtime_error", "Light hardware update failed");
        break;
    case newmota::LightApiError::none:
        break;
    }
}

int LightGet(bvm* const vm) {
    if (be_top(vm) > 2)
        be_raise(vm, "type_error", "light.get expects an optional index and field name");
    std::uint8_t endpoint = 0U;
    const char* field_name = nullptr;
    std::size_t field_length = 0U;
    if (be_top(vm) >= 1) {
        if (be_isstring(vm, 1)) {
            field_name = be_tostring(vm, 1);
            field_length = static_cast<std::size_t>(be_strlen(vm, 1));
        } else if (!parse_index(vm, 1, endpoint)) {
            be_raise(vm, "type_error", "light.get index must be an integer");
        }
    }
    if (be_top(vm) == 2) {
        if (!be_isstring(vm, 2))
            be_raise(vm, "type_error", "light.get field name must be a string");
        field_name = be_tostring(vm, 2);
        field_length = static_cast<std::size_t>(be_strlen(vm, 2));
    }
    if (light_service == nullptr) {
        be_pushnil(vm);
        be_return(vm);
    }
    newmota::LightApiSnapshot snapshot{};
    const auto result = newmota::light_api_get(*light_service, endpoint, snapshot);
    if (result != newmota::LightApiError::none) {
        be_pushnil(vm);
        be_return(vm);
    }
    if (field_name == nullptr) {
        push_snapshot(vm, snapshot);
    } else {
        Field field{};
        if (!find_field(field_name, field_length, field)) {
            be_pushnil(vm);
        } else {
            push_field(vm, field, snapshot);
        }
    }
    be_return(vm);
}

int LightSet(bvm* const vm) {
    if ((be_top(vm) != 1 && be_top(vm) != 2) || !be_ismapinstance(vm, 1))
        be_raise(vm, "type_error", "light.set expects a settings map and optional index");
    std::uint8_t endpoint = 0U;
    if (be_top(vm) == 2 && !parse_index(vm, 2, endpoint))
        be_raise(vm, "type_error", "light.set index must be an integer");
    if (light_service == nullptr)
        be_raise(vm, "runtime_error", "Light service is unavailable");

    newmota::LightApiMutation mutation{};
    parse_mutation(vm, 1, mutation);
    newmota::LightApiSnapshot snapshot{};
    const auto result = newmota::light_api_set(
        *light_service, endpoint, mutation, snapshot, newmota::PowerOperationSource::berry);
    if (result != newmota::LightApiError::none)
        raise_api_error(vm, result);
    push_snapshot(vm, snapshot);
    be_return(vm);
}

} // namespace

extern "C" {

be_native_module_attr_table(light){
    be_native_module_function("get", LightGet),
    be_native_module_function("set", LightSet),
    be_native_module_function("gamma10", LightGamma10),
    be_native_module_function("reverse_gamma10", LightReverseGamma10),
    be_native_module_function("gamma8", LightGamma8),
};

extern const bntvmodule_t be_native_module_light = {
    "light", light_attrs, sizeof(light_attrs) / sizeof(light_attrs[0]), nullptr};

} // extern "C"

namespace newmota {

bool install_light_module(bvm* const vm, LightService& light) {
    light_service = &light;
    if (!be_getmodule(vm, "light")) {
        light_service = nullptr;
        return false;
    }
    be_setglobal(vm, "light");
    be_pop(vm, 1);
    return true;
}

void remove_light_module() {
    light_service = nullptr;
}

} // namespace newmota

#endif // USE_PWM || USE_WS2812
