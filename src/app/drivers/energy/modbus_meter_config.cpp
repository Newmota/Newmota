/*
  modbus_meter_config.cpp - Bounded Tasmota-style configurable Modbus maps

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  User-field resolution reference: Tasmota xnrg_29_modbus.ino,
  Copyright (C) 2022 Theo Arends, GPL version 3 or later.
*/

/** @file modbus_meter_config.cpp
 * @brief Parses bounded JSON maps for the configurable Modbus energy driver.
 */

#include "drivers/energy/modbus_meter.h"
#include "support/json.h"
#include "support/number.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace newmota::energy::registers {
namespace {
using support::JsonKind;
using support::JsonReader;
using support::JsonToken;

std::size_t object_member(const JsonReader& json, std::size_t object, std::string_view name) {
    return json.member(object, name);
}

/** @brief Maps the Tasmota type number to its supported wire encoding.
 * @param json Tokenized map.
 * @param object Register object with optional T property.
 * @param encoding Receives the selected encoding; missing or unknown types default to float32.
 * @return false when a present T property is not an unsigned integer.
 */
bool parse_encoding(const JsonReader& json, std::size_t object, Encoding& encoding) {
    const auto type = object_member(json, object, "T");
    if (type >= json.token_count()) {
        encoding = Encoding::float32;
        return true;
    }
    std::uint32_t value{};
    if (!support::json_unsigned(json, type, UINT32_MAX, value))
        return false;
    switch (value) {
    case 0:
        encoding = Encoding::float32;
        break;
    case 1:
        encoding = Encoding::signed16;
        break;
    case 2:
        encoding = Encoding::signed32;
        break;
    case 3:
        encoding = Encoding::unsigned16;
        break;
    case 4:
        encoding = Encoding::unsigned32;
        break;
    case 6:
        encoding = Encoding::signed32_swapped;
        break;
    case 8:
        encoding = Encoding::unsigned32_swapped;
        break;
    case 5:
    case 7:
        encoding = Encoding::float32;
        break;
    default:
        encoding = Encoding::float32;
        break;
    }
    return true;
}

/** @brief Converts one Tasmota F/M scale into its exactly representable decimal exponent.
 * @param json Tokenized map containing the scale properties.
 * @param object Register object to inspect.
 * @param exponent Receives F or the negative base-10 exponent represented by M.
 * @return false for zero, non-power-of-ten, or out-of-range divisors.
 */
bool parse_factor(const JsonReader& json, std::size_t object, std::int8_t& exponent) {
    const auto factor = object_member(json, object, "F");
    const auto divider = object_member(json, object, "M");
    std::int32_t value{};
    if (factor < json.token_count()) {
        if (!support::json_integer(json, factor, -6, 6, value))
            return false;
        exponent = static_cast<std::int8_t>(value);
    }
    if (divider < json.token_count()) {
        std::uint32_t divisor{};
        if (!support::json_unsigned(json, divider, 1000000, divisor) || divisor == 0)
            return false;
        value = 0;
        while (divisor > 1) {
            // Only exact decimal divisors can be represented by the stored exponent.
            if (divisor % 10 != 0)
                return false;
            divisor /= 10;
            --value;
        }
        if (value < -6)
            return false;
        exponent = static_cast<std::int8_t>(value);
    }
    return true;
}

/** @brief Reads one register address or an ordered list of up to three channel addresses.
 * @param json Tokenized map.
 * @param value Scalar or array token containing addresses.
 * @param addresses Receives the parsed 16-bit addresses.
 * @param count Receives the number of addresses.
 * @return true when at least one address is valid and no more than three are supplied.
 */
bool parse_register_addresses(const JsonReader& json, std::size_t value,
    std::array<std::uint16_t, 3>& addresses, std::size_t& count) {
    count = 0;
    if (value >= json.token_count())
        return false;
    if (json.token(value).kind == JsonKind::array) {
        for (std::size_t item = 0; item < 3; ++item) {
            const auto token = json.array_item(value, item);
            if (token >= json.token_count())
                break;
            std::uint32_t address{};
            if (!support::json_unsigned(json, token, 65535, address))
                return false;
            addresses[count++] = static_cast<std::uint16_t>(address);
        }
        return count != 0 && json.array_item(value, 3) >= json.token_count();
    }
    std::uint32_t address{};
    if (!support::json_unsigned(json, value, 65535, address))
        return false;
    addresses[count++] = static_cast<std::uint16_t>(address);
    return true;
}

/** @brief Appends one register and expands its decimal exponent into an exact rational scale.
 * @param output Bounded profile being built.
 * @param address Register address; 0xFFFF denotes an unused slot and is ignored.
 * @param field Canonical destination field.
 * @param channel Channel index or common for shared values.
 * @param encoding Wire representation.
 * @param exponent Decimal scale exponent from F or M.
 * @param user_index Custom field index, or 255 for a built-in field.
 * @return false when the fixed register capacity is exceeded.
 */
bool add_register(ConfiguredProfile& output, std::uint16_t address, Field field,
    std::uint8_t channel, Encoding encoding, std::int8_t exponent, std::uint8_t user_index = 255) {
    if (address == 0xffff)
        return true;
    if (output.register_count >= output.registers.size())
        return false;
    Register reg{};
    reg.address = address;
    reg.field = field;
    reg.channel = channel;
    reg.encoding = encoding;
    reg.user_index = user_index;
    reg.user_exponent = exponent;
    if (field != Field::custom) {
        reg.numerator = 1000;
        reg.denominator = 1;
        if (exponent >= 0) {
            for (int i = 0; i < exponent; ++i)
                reg.numerator *= 10;
        } else {
            for (int i = 0; i > exponent; --i)
                reg.denominator *= 10;
        }
    }
    output.registers[output.register_count++] = reg;
    return true;
}

/** @brief Expands one configured field into its channel-specific register rows.
 * @param output Bounded profile being built.
 * @param json Tokenized map.
 * @param value Register scalar, list, or object token.
 * @param field Canonical destination field.
 * @param channels Configured channel count.
 * @param address_count Explicit number of channel unit IDs, if supplied.
 * @param user_index Custom field index, or 255 for a built-in field.
 * @return false when encoding, addressing, channel shape, or capacity is invalid.
 */
bool add_field(ConfiguredProfile& output, const JsonReader& json, std::size_t value, Field field,
    std::uint8_t channels, std::uint8_t address_count, std::uint8_t user_index = 255) {
    std::size_t register_value = value;
    Encoding encoding = Encoding::float32;
    std::int8_t exponent{};
    if (json.token(value).kind == JsonKind::object) {
        register_value = object_member(json, value, "R");
        if (register_value >= json.token_count())
            return false;
        if (!parse_encoding(json, value, encoding) || !parse_factor(json, value, exponent))
            return false;
    }
    std::array<std::uint16_t, 3> addresses{};
    std::size_t address_size{};
    if (!parse_register_addresses(json, register_value, addresses, address_size))
        return false;
    if (address_size > channels)
        return false;
    if (address_count > 1) {
        if (address_count != channels || (address_size != 1 && address_size != channels))
            return false;
        for (std::uint8_t channel = 0; channel < channels; ++channel)
            if (!add_register(output, addresses[address_size == 1 ? 0 : channel], field, channel,
                    encoding, exponent, user_index))
                return false;
        return true;
    }
    if (address_size == 1 && channels > 1 && (field == Field::voltage || field == Field::frequency))
        return add_register(output, addresses[0], field, common, encoding, exponent, user_index);
    for (std::size_t channel = 0; channel < address_size; ++channel)
        if (!add_register(output, addresses[channel], field, static_cast<std::uint8_t>(channel),
                encoding, exponent, user_index))
            return false;
    return true;
}

/** @brief Validates and adds one Tasmota-compatible custom measurement definition.
 * @param json Tokenized map.
 * @param object User-field object token.
 * @param output Bounded profile receiving metadata and register rows.
 * @param channels Configured channel count.
 * @param address_count Explicit number of channel unit IDs, if supplied.
 * @return false for duplicate/reserved names, invalid metadata, or capacity overflow.
 */
bool parse_user(const JsonReader& json, std::size_t object, ConfiguredProfile& output,
    std::uint8_t channels, std::uint8_t address_count) {
    if (json.token(object).kind != JsonKind::object || output.user_count >= output.users.size())
        return false;
    auto& field = output.users[output.user_count];
    std::size_t length{};
    const auto name = object_member(json, object, "J");
    if (name >= json.token_count() ||
        !support::json_copy_string(
            json, name, field.json_name.data(), field.json_name.size(), length) ||
        !length)
        return false;
    constexpr std::array<std::string_view, 9> reserved{"Voltage", "Current", "Power",
        "ApparentPower", "ReactivePower", "Factor", "Frequency", "Total", "ExportActive"};
    for (std::size_t i = 0; i < output.user_count; ++i)
        if (field.json_name == output.users[i].json_name)
            return false;
    for (const auto name_value : reserved)
        if (std::string_view(field.json_name.data()) == name_value)
            return false;

    auto copy_optional = [&](std::string_view key, auto& destination) {
        const auto token = object_member(json, object, key);
        if (token >= json.token_count())
            return true;
        std::size_t copied{};
        return support::json_copy_string(
            json, token, destination.data(), destination.size(), copied);
    };
    if (!copy_optional("G", field.gui_name) || !copy_optional("U", field.unit))
        return false;
    const auto decimals = object_member(json, object, "D");
    if (decimals < json.token_count()) {
        std::int32_t value{};
        if (!support::json_integer(json, decimals, 0, 29, value))
            return false;
        field.decimals = static_cast<std::uint8_t>(value <= 20 ? value : 3);
#if USE_MODBUS_USER_PRECISION
        if (value >= 21)
            field.resolution_selector = static_cast<std::uint8_t>(value);
#endif
    }
    const auto user_index = static_cast<std::uint8_t>(output.user_count++);
    return add_field(output, json, object, Field::custom, channels, address_count, user_index);
}

/** @brief Parses optional 8N1, 8N2, 8E1/2, or 8O1/2 framing text.
 * @param json Tokenized map.
 * @param token Framing string, or an absent token to keep defaults.
 * @param parity Receives the decoded parity.
 * @param stop_bits Receives the decoded stop-bit count.
 * @return false when framing is not one of the supported eight-data-bit forms.
 */
bool parse_framing(const JsonReader& json, std::size_t token, platform::serial::Parity& parity,
    std::uint8_t& stop_bits) {
    if (token >= json.token_count())
        return true;
    if (json.token(token).kind != JsonKind::string)
        return false;
    const auto value = json.text(token);
    if (value.size() != 3 || value[0] != '8' || (value[2] != '1' && value[2] != '2'))
        return false;
    stop_bits = static_cast<std::uint8_t>(value[2] - '0');
    switch (value[1]) {
    case 'N':
        parity = platform::serial::Parity::none;
        break;
    case 'E':
        parity = platform::serial::Parity::even;
        break;
    case 'O':
        parity = platform::serial::Parity::odd;
        break;
    default:
        return false;
    }
    return true;
}

} // namespace

void ConfiguredProfile::bind() {
    profile.registers = registers.data();
    profile.size = register_count;
    profile.user_fields = users.data();
    profile.user_field_count = user_count;
}

bool parse_configured_profile(const Configuration& config, ConfiguredProfile& output) {
    output = {};
    if (config.modbus_profile.empty() ||
        config.modbus_profile.size() > Configuration::kMaximumModbusProfile)
        return false;
    const auto text = config.modbus_profile;
    std::array<JsonToken, 256> tokens{};
    JsonReader json{text, tokens.data(), tokens.size()};
    std::size_t root{};
    if (!json.parse(root) || json.token(root).kind != JsonKind::object)
        return false;
    auto& profile = output.profile;
    profile.channels = config.channels;
#if USE_BERRY_ENERGY_API
    profile.observation_scope = ObservationScope::mapped_phase;
#endif
    profile.function = 4;
    profile.interval_ms = 200;
    profile.parity = platform::serial::Parity::none;
    profile.stop_bits = 1;
    profile.baud = 9600;

    const auto baud_token = json.member(root, "Baud");
    if (baud_token < json.token_count()) {
        std::uint32_t baud{};
        if (!support::json_unsigned(json, baud_token, 115200, baud) || baud < 300)
            return false;
        profile.baud = baud;
    }
    const auto config_token = json.member(root, "Config");
    if (!parse_framing(json, config_token, profile.parity, profile.stop_bits))
        return false;
    const auto poll_token = json.member(root, "Poll");
    if (poll_token < json.token_count()) {
        std::uint32_t interval{};
        if (!support::json_unsigned(json, poll_token, 5000, interval))
            return false;
        profile.interval_ms = static_cast<std::uint16_t>(interval < 100 ? 200 : interval);
    }
    const auto function_token = json.member(root, "Function");
    if (function_token < json.token_count()) {
        std::uint32_t function{};
        if (!support::json_unsigned(json, function_token, 4, function) ||
            (function != 3 && function != 4))
            return false;
        profile.function = static_cast<std::uint8_t>(function);
    }
    const auto addresses_token = json.member(root, "Address");
    if (addresses_token < json.token_count()) {
        if (json.token(addresses_token).kind == JsonKind::array) {
            for (std::size_t i = 0; i < profile.addresses.size(); ++i) {
                const auto item = json.array_item(addresses_token, i);
                if (item >= json.token_count())
                    break;
                std::uint32_t address{};
                if (!support::json_unsigned(json, item, 247, address) || !address)
                    return false;
                profile.addresses[profile.address_count++] = static_cast<std::uint8_t>(address);
            }
            if (!profile.address_count || json.array_item(addresses_token, 3) < json.token_count())
                return false;
        } else {
            std::uint32_t address{};
            if (!support::json_unsigned(json, addresses_token, 247, address) || !address)
                return false;
            profile.addresses[0] = static_cast<std::uint8_t>(address);
            profile.address_count = 1;
        }
    }
    if (profile.address_count && profile.address_count != 1 &&
        profile.address_count != config.channels)
        return false;

    struct Builtin {
        std::string_view key;
        Field field;
    };

    constexpr Builtin builtins[]{{"Voltage", Field::voltage}, {"Current", Field::current},
        {"Power", Field::power}, {"ApparentPower", Field::apparent},
        {"ReactivePower", Field::reactive}, {"Factor", Field::factor},
        {"Frequency", Field::frequency}, {"Total", Field::total_active},
        {"ExportActive", Field::export_counter}};
    for (const auto& item : builtins) {
        const auto token = json.member(root, item.key);
        if (token >= json.token_count())
            continue;
        if (!add_field(output, json, token, item.field, config.channels, profile.address_count))
            return false;
        if (item.field == Field::total_active)
            profile.total_is_import = true;
    }
    const auto user_token = json.member(root, "User");
    if (user_token < json.token_count()) {
        if (json.token(user_token).kind == JsonKind::array) {
            for (std::size_t i = 0; i <= maximum_user_fields; ++i) {
                const auto item = json.array_item(user_token, i);
                if (item >= json.token_count())
                    break;
                if (i == maximum_user_fields ||
                    !parse_user(json, item, output, config.channels, profile.address_count))
                    return false;
            }
        } else if (!parse_user(json, user_token, output, config.channels, profile.address_count))
            return false;
    }
    if (!output.register_count)
        return false;
    output.bind();
    return true;
}
} // namespace newmota::energy::registers
