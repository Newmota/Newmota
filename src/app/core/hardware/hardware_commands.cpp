/*
  hardware_commands.cpp - GPIO, Module, and Template commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/hardware/hardware_commands.h"

#include "core/hardware/hardware.h"
#include "core/system/restart.h"
#include "gpio_ids.h"
#include "hardware_names.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include "pin_topology.h"
#include "services/settings/settings.h"
#include "support/json.h"
#include "support/number.h"
#include "support/text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

namespace newmota::commands {
namespace {

using generated::gpio::FunctionDescriptor;
using generated::gpio::ModuleDescriptor;
using generated::hardware::PinTopology;

constexpr std::size_t kFunctionNameCapacity = 52;

/** @brief Holds a bounded bulk GPIO document before transactional application. */
struct GpioDocument final {
    std::array<std::uint16_t, kMaximumGpioOverrides> values{};
    std::uint8_t count{};
};

/** @brief Compares a key after decoding its escapes, as the Template and GPIO commands did. */
[[nodiscard]] bool key_is(
    const support::JsonReader& json, const std::size_t key, const std::string_view name) {
    std::array<char, 9> text{}; // keys of up to 8 characters, the old cursor's bound
    std::size_t length = 0;
    return support::json_copy_string(json, key, text.data(), text.size(), length) &&
           std::string_view{text.data(), length} == name;
}

/** @brief Reads a bounded array of 16-bit GPIO wire values. */
[[nodiscard]] bool parse_gpio_array(const support::JsonReader& json, const std::size_t array,
    std::uint16_t* const destination, const std::size_t capacity, std::uint8_t& count) {
    count = 0;
    if (json.token(array).kind != support::JsonKind::array)
        return false;
    for (auto item = json.first(array); item < json.token_count(); item = json.next(array, item)) {
        std::uint32_t value = 0;
        if (count == capacity ||
            !support::json_decimal(json, item, std::numeric_limits<std::uint16_t>::max(), value))
            return false;
        destination[count++] = static_cast<std::uint16_t>(value);
    }
    return true;
}

/** @brief Applies a partial Template JSON object to a caller-owned candidate. */
[[nodiscard]] bool parse_template(
    const std::string_view payload, HardwareConfiguration& candidate) {
    // Five members, the largest being GPIO with one token per template slot.
    std::array<support::JsonToken, 1 + 2 * 5 + kMaximumTemplateSlots> tokens{};
    support::JsonReader json{payload, tokens.data(), tokens.size()};
    std::size_t root = 0;
    if (!json.parse(root) || json.token(root).kind != support::JsonKind::object)
        return false;
    std::uint8_t seen = 0;
    for (auto key = json.first(root); key < json.token_count();) {
        const auto value = json.next(root, key);
        if (value >= json.token_count())
            return false;
        std::uint8_t field = 0;
        if (key_is(json, key, "NAME")) {
            field = 1U << 0U;
            std::array<char, kUserTemplateNameCapacity + 1> text{};
            std::size_t length = 0;
            // Escaped controls (\n, \t) in names were accepted before and are stored as is.
            if (!support::json_copy_string(json, value, text.data(), text.size(), length, true) ||
                !candidate.set_user_template_name({text.data(), length}))
                return false;
        } else if (key_is(json, key, "ARCH")) {
            field = 1U << 1U;
            std::array<char, kHardwareArchitectureCapacity + 1> text{};
            std::size_t length = 0;
            if (!support::json_copy_string(json, value, text.data(), text.size(), length) ||
                std::string_view{text.data(), length} != generated::hardware::kArchitecture)
                return false;
        } else if (key_is(json, key, "GPIO")) {
            field = 1U << 2U;
            std::uint8_t count = 0;
            if (!parse_gpio_array(json, value, candidate.user_template.data(),
                    candidate.user_template_size, count))
                return false;
        } else if (key_is(json, key, "FLAG")) {
            field = 1U << 3U;
            if (!support::json_decimal(json, value, std::numeric_limits<std::uint32_t>::max(),
                    candidate.user_template_flag))
                return false;
        } else if (key_is(json, key, "BASE")) {
            field = 1U << 4U;
            std::uint32_t base = 0;
            if (!support::json_decimal(json, value, 254U, base))
                return false;
            candidate.user_template_base = static_cast<std::uint8_t>(base);
        } else {
            return false;
        }
        if ((seen & field) != 0U)
            return false;
        seen |= field;
        key = json.next(root, value);
    }
    return seen != 0U;
}

/** @brief Parses one `{"GPIO":[...]}` bulk assignment document. */
[[nodiscard]] bool parse_gpio(const std::string_view payload, GpioDocument& document) {
    std::array<support::JsonToken, 3 + kMaximumGpioOverrides> tokens{};
    support::JsonReader json{payload, tokens.data(), tokens.size()};
    std::size_t root = 0;
    if (!json.parse(root) || json.token(root).kind != support::JsonKind::object)
        return false;
    const auto key = json.first(root);
    const auto value = json.next(root, key);
    return value < json.token_count() && key_is(json, key, "GPIO") &&
           json.next(root, value) >= json.token_count() &&
           parse_gpio_array(
               json, value, document.values.data(), document.values.size(), document.count) &&
           document.count != 0U;
}

/** @brief Finds a generated built-in module and optionally returns its table index. */
[[nodiscard]] const ModuleDescriptor* find_module(
    const std::uint8_t number, std::size_t* const table_index = nullptr) {
    for (std::size_t index = 0; index < generated::gpio::kModules.size(); ++index) {
        const auto& candidate = generated::gpio::kModules[index];
        if (candidate.number == number) {
            if (table_index != nullptr) {
                *table_index = index;
            }
            return &candidate;
        }
    }
    return nullptr;
}

/** @brief Returns one generated function display name by descriptor index. */
[[nodiscard]] std::string_view function_base_name(const std::size_t index) {
    const auto offset = generated::gpio::kFunctionNameOffsets[index];
    return &generated::gpio::kFunctionNames[offset];
}

/** @brief Returns one generated module display name by descriptor index. */
[[nodiscard]] std::string_view module_table_name(const std::size_t index) {
    const auto offset = generated::gpio::kModuleNameOffsets[index];
    return &generated::gpio::kModuleNames[offset];
}

/** @brief Finds the qualified descriptor containing one GPIO wire value. */
[[nodiscard]] const FunctionDescriptor* find_function(
    const std::uint16_t function, std::size_t* const table_index = nullptr) {
    for (std::size_t index = 0; index < generated::gpio::kFunctions.size(); ++index) {
        const auto& descriptor = generated::gpio::kFunctions[index];
        if (function < descriptor.first) {
            break;
        }
        if (static_cast<std::uint16_t>(function - descriptor.first) < descriptor.instances) {
            if (table_index != nullptr) {
                *table_index = index;
            }
            return &descriptor;
        }
    }
    return nullptr;
}

/** @brief Formats a qualified GPIO name including its one-based instance suffix. */
[[nodiscard]] std::string_view function_name(
    const std::uint16_t function, std::array<char, kFunctionNameCapacity>& storage) {
    std::size_t index = 0;
    const auto* const descriptor = find_function(function, &index);
    if (descriptor == nullptr) {
        return "Unknown";
    }
    const auto base = function_base_name(index);
    if (base.size() >= storage.size()) {
        return "Unknown";
    }
    std::size_t length = base.size();
    for (std::size_t position = 0; position < base.size(); ++position) {
        storage[position] = base[position];
    }
    if (descriptor->instances > 1U) {
        const auto instance = static_cast<unsigned int>(function - descriptor->first + 1U);
        const int written =
            std::snprintf(storage.data() + length, storage.size() - length, "%u", instance);
        if (written <= 0 || static_cast<std::size_t>(written) >= storage.size() - length) {
            return "Unknown";
        }
        length += static_cast<std::size_t>(written);
    }
    return {storage.data(), length};
}

/** @brief Returns generated topology facts for one physical pin. */
[[nodiscard]] const PinTopology* find_pin(const PinId pin) {
    for (const auto& candidate : generated::hardware::kPins) {
        if (candidate.id == pin.value()) {
            return &candidate;
        }
    }
    return nullptr;
}

/** @brief Expands the selected user or built-in template into slot order. */
[[nodiscard]] bool base_template(const HardwareConfiguration& configuration,
    std::array<std::uint16_t, kMaximumTemplateSlots>& assignments) {
    assignments = {};
    if (configuration.module == 0U) {
        if (configuration.user_template_size != generated::hardware::kTemplateSlotCount) {
            return false;
        }
        for (std::size_t slot = 0; slot < configuration.user_template_size; ++slot) {
            assignments[slot] = configuration.user_template[slot];
        }
        return true;
    }
    const auto* const module = find_module(configuration.module);
    if (module == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < module->assignment_count; ++index) {
        const auto& assignment =
            generated::gpio::kModuleAssignments[module->assignment_offset + index];
        assignments[assignment.slot] = assignment.function;
    }
    return true;
}

/** @brief Returns the effective wire value for one physical template pin. */
[[nodiscard]] std::uint16_t effective_function(const HardwareConfiguration& configuration,
    const std::array<std::uint16_t, kMaximumTemplateSlots>& assignments, const PinTopology& pin) {
    const std::uint16_t base = assignments[pin.template_slot];
    if (base != generated::gpio::kTemplateUserValue) {
        return base;
    }
    for (std::size_t index = 0; index < configuration.override_count; ++index) {
        if (configuration.overrides[index].pin.value() == pin.id) {
            return configuration.overrides[index].function;
        }
    }
    return 0U;
}

/** @brief Copies one built-in module into the persistent user Template fields. */
[[nodiscard]] bool copy_module_template(
    const std::uint8_t number, HardwareConfiguration& candidate) {
    std::size_t table_index = 0;
    const auto* const module = find_module(number, &table_index);
    if (module == nullptr) {
        return false;
    }
    candidate.user_template = {};
    candidate.user_template_size =
        static_cast<std::uint8_t>(generated::hardware::kTemplateSlotCount);
    for (std::size_t index = 0; index < module->assignment_count; ++index) {
        const auto& assignment =
            generated::gpio::kModuleAssignments[module->assignment_offset + index];
        candidate.user_template[assignment.slot] = assignment.function;
    }
    candidate.user_template_flag = module->flag;
    candidate.user_template_base = number;
    return candidate.set_user_template_name(module_table_name(table_index));
}

/** @brief Removes one pin and duplicate function before appending an override. */
[[nodiscard]] bool set_override(
    HardwareConfiguration& candidate, const PinId pin, const std::uint16_t function) {
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    const auto* const topology = find_pin(pin);
    if (topology == nullptr || (topology->flags & generated::hardware::kPinAvailable) == 0U ||
        topology->template_slot == generated::hardware::kNoTemplateSlot ||
        !base_template(candidate, assignments) ||
        assignments[topology->template_slot] != generated::gpio::kTemplateUserValue ||
        (function != 0U && find_function(function) == nullptr)) {
        return false;
    }

    std::uint8_t retained = 0;
    for (std::size_t index = 0; index < candidate.override_count; ++index) {
        const auto& existing = candidate.overrides[index];
        if (existing.pin.value() == pin.value() ||
            (function != 0U && existing.function == function)) {
            continue;
        }
        candidate.overrides[retained++] = existing;
    }
    candidate.override_count = retained;
    if (function != 0U) {
        if (candidate.override_count == candidate.overrides.size()) {
            return false;
        }
        candidate.overrides[candidate.override_count++] = {pin, function};
    }
    return true;
}

/** @brief Maps one settings transaction result into the command status contract. */
[[nodiscard]] CommandStatus update_status(const SettingsUpdateResult result) {
    switch (result) {
    case SettingsUpdateResult::pending:
    case SettingsUpdateResult::saved:
    case SettingsUpdateResult::unchanged:
        return CommandStatus::handled;
    case SettingsUpdateResult::invalid:
        return CommandStatus::invalid;
    case SettingsUpdateResult::failed:
        return CommandStatus::failed;
    }
    return CommandStatus::failed;
}

/** @brief Writes the selected Module number and display name. */
[[nodiscard]] bool write_module(CommandOutput& output, const HardwareConfiguration& configuration) {
    std::string_view name = configuration.user_template_name();
    if (configuration.module != 0U) {
        std::size_t index = 0;
        if (find_module(configuration.module, &index) == nullptr) {
            name = "Unknown";
        } else {
            name = module_table_name(index);
        }
    }
    return output.format("{\"Module\":{\"%u\":", static_cast<unsigned int>(configuration.module)) &&
           output.json_string(name) && output.write("}}");
}

/** @brief Handles Module queries and persistent module selection. */
[[nodiscard]] CommandStatus module_command(
    const CommandRequest& request, Settings& settings, RestartController&, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    if (!request.is_query()) {
        std::uint32_t number = 0;
        if (!support::parse_unsigned_decimal(request.payload, 254U, number) ||
            (number != 0U && find_module(static_cast<std::uint8_t>(number)) == nullptr)) {
            return CommandStatus::invalid;
        }
        auto candidate = settings.hardware_configuration();
        if (candidate.module != number) {
            candidate.module = static_cast<std::uint8_t>(number);
            candidate.override_count = 0U;
            const auto result = settings.set_hardware_configuration(candidate);
            const auto status = update_status(result);
            if (status != CommandStatus::handled) {
                return status;
            }
        }
    }
    return write_module(output, settings.hardware_configuration()) ? CommandStatus::handled
                                                                   : CommandStatus::failed;
}

/** @brief Streams every compiled module without a response-sized buffer. */
[[nodiscard]] CommandStatus modules_command(
    const CommandRequest& request, const Settings& settings, CommandOutput& output) {
    if (request.has_index || !request.is_query()) {
        return CommandStatus::invalid;
    }
    if (!output.write("{\"Modules\":{\"0\":") ||
        !output.json_string(settings.hardware_configuration().user_template_name())) {
        return CommandStatus::failed;
    }
    for (std::size_t index = 0; index < generated::gpio::kModules.size(); ++index) {
        if (!output.format(
                ",\"%u\":", static_cast<unsigned int>(generated::gpio::kModules[index].number)) ||
            !output.json_string(module_table_name(index))) {
            return CommandStatus::failed;
        }
    }
    return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Writes one GPIO value using Tasmota's nested name/value shape. */
[[nodiscard]] bool write_gpio_object(CommandOutput& output, const std::uint16_t pin,
    const std::uint16_t function, const bool comma) {
    std::array<char, kFunctionNameCapacity> storage{};
    return output.format("%s\"GPIO%u\":{", comma ? "," : "", static_cast<unsigned int>(pin)) &&
           output.json_string(function_name(function, storage)) &&
           output.format(":%u}", static_cast<unsigned int>(function));
}

/** @brief Streams one requested GPIO report shape from the effective template. */
[[nodiscard]] bool write_gpio_report(CommandOutput& output,
    const HardwareConfiguration& configuration, const std::uint8_t shape,
    const bool configured_only, const bool include_all) {
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    if (!base_template(configuration, assignments)) {
        return false;
    }
    if (!output.write(shape == 0U ? "{" : "{\"GPIO\":[")) {
        return false;
    }
    bool first = true;
    for (const auto& pin : generated::hardware::kPins) {
        if ((pin.flags & generated::hardware::kPinAvailable) == 0U ||
            pin.template_slot == generated::hardware::kNoTemplateSlot) {
            continue;
        }
        const auto base = assignments[pin.template_slot];
        const auto function = effective_function(configuration, assignments, pin);
        if ((!include_all && base == 0U) || (configured_only && function == 0U)) {
            continue;
        }
        if (shape == 0U) {
            if (!write_gpio_object(output, pin.id, function, !first)) {
                return false;
            }
        } else if (shape == 1U) {
            std::array<char, kFunctionNameCapacity> storage{};
            if (!output.format("%s[%u,%u,", first ? "" : ",", static_cast<unsigned int>(pin.id),
                    static_cast<unsigned int>(function)) ||
                !output.json_string(function_name(function, storage)) || !output.write("]")) {
                return false;
            }
        } else if (shape == 2U) {
            if (!output.format("%s[%u,%u]", first ? "" : ",", static_cast<unsigned int>(pin.id),
                    static_cast<unsigned int>(function))) {
                return false;
            }
        } else if (!output.format("%s%u", first ? "" : ",", static_cast<unsigned int>(function))) {
            return false;
        }
        first = false;
    }
    return output.write(shape == 0U ? "}" : "]}");
}

/** @brief Writes one indexed GPIO response from the effective configuration. */
[[nodiscard]] bool write_indexed_gpio(
    CommandOutput& output, const HardwareConfiguration& configuration, const PinId pin) {
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    const auto* const topology = find_pin(pin);
    if (topology == nullptr || (topology->flags & generated::hardware::kPinAvailable) == 0U ||
        topology->template_slot == generated::hardware::kNoTemplateSlot ||
        !base_template(configuration, assignments)) {
        return false;
    }
    return output.write("{") &&
           write_gpio_object(output, pin.value(),
               effective_function(configuration, assignments, *topology), false) &&
           output.write("}");
}

/** @brief Applies a complete or partial bulk GPIO array in one settings write. */
[[nodiscard]] CommandStatus apply_gpio_document(
    const GpioDocument& document, Settings& settings, RestartController&, CommandOutput& output) {
    auto candidate = settings.hardware_configuration();
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    if (!base_template(candidate, assignments)) {
        return CommandStatus::invalid;
    }
    std::size_t value_index = 0;
    for (const auto& topology : generated::hardware::kPins) {
        if (value_index == document.count) {
            break;
        }
        if ((topology.flags & generated::hardware::kPinAvailable) == 0U ||
            topology.template_slot == generated::hardware::kNoTemplateSlot ||
            assignments[topology.template_slot] != generated::gpio::kTemplateUserValue) {
            continue;
        }
        if (!set_override(candidate, PinId{topology.id}, document.values[value_index++])) {
            return CommandStatus::invalid;
        }
    }
    if (value_index != document.count) {
        return CommandStatus::invalid;
    }
    const auto result = settings.set_hardware_configuration(candidate);
    const auto status = update_status(result);
    if (status != CommandStatus::handled) {
        return status;
    }
    return write_gpio_report(output, settings.hardware_configuration(), 3U, false, false)
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Handles indexed mutations and the Tasmota GPIO report variants. */
[[nodiscard]] CommandStatus gpio_command(const CommandRequest& request, Settings& settings,
    RestartController& restart, CommandOutput& output) {
    if (request.has_index) {
        const PinId pin{request.index};
        if (!request.is_query()) {
            std::uint32_t function = 0;
            if (!support::parse_unsigned_decimal(
                    request.payload, std::numeric_limits<std::uint16_t>::max(), function)) {
                return CommandStatus::invalid;
            }
            auto candidate = settings.hardware_configuration();
            if (!set_override(candidate, pin, static_cast<std::uint16_t>(function))) {
                return CommandStatus::invalid;
            }
            const auto result = settings.set_hardware_configuration(candidate);
            const auto status = update_status(result);
            if (status != CommandStatus::handled) {
                return status;
            }
        }
        return write_indexed_gpio(output, settings.hardware_configuration(), pin)
                   ? CommandStatus::handled
                   : CommandStatus::invalid;
    }

    if (request.has_payload && !request.payload.empty() && request.payload.front() == '{') {
        GpioDocument document{};
        return parse_gpio(request.payload, document)
                   ? apply_gpio_document(document, settings, restart, output)
                   : CommandStatus::invalid;
    }

    std::uint32_t mode = 0;
    if (!request.is_query() && !support::parse_unsigned_decimal(request.payload, 255U, mode)) {
        return CommandStatus::invalid;
    }
    bool configured_only = false;
    if (mode >= 10U && mode <= 12U) {
        mode -= 10U;
        configured_only = true;
    }
    if (mode != 0U && mode != 1U && mode != 2U && mode != 3U && mode != 255U) {
        return CommandStatus::invalid;
    }
    return write_gpio_report(output, settings.hardware_configuration(),
               mode == 255U ? 0U : static_cast<std::uint8_t>(mode), configured_only, mode == 255U)
               ? CommandStatus::handled
               : CommandStatus::failed;
}

/** @brief Reads every active resolved binding without reconfiguring its pin. */
[[nodiscard]] CommandStatus gpio_read_command(
    const CommandRequest& request, const Settings& settings, CommandOutput& output) {
    if (request.has_index || !request.is_query()) {
        return CommandStatus::invalid;
    }
    Hardware hardware{};
    if (!HardwareResolver::resolve(
            settings.hardware_configuration(), ResolutionPolicy::tolerant, hardware)
            .accepted() ||
        !output.write("{\"GPIORead\":{")) {
        return CommandStatus::failed;
    }
    bool first = true;
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* const binding = hardware.binding(index);
        if (binding == nullptr || !binding->active()) {
            continue;
        }
        bool high = false;
        if (!output.format("%s\"GPIO%u\":", first ? "" : ",",
                static_cast<unsigned int>(binding->pin.value()))) {
            return CommandStatus::failed;
        }
        if (platform::gpio_read(binding->pin, high) == platform::GpioResult::success) {
            if (!output.write(high ? "1" : "0")) {
                return CommandStatus::failed;
            }
        } else if (!output.write("null")) {
            return CommandStatus::failed;
        }
        first = false;
    }
    return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Streams the GPIO function names compiled into this application. */
[[nodiscard]] CommandStatus gpios_command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index || (!request.is_query() && request.payload != "255") ||
        !output.write("{\"GPIOs\":{")) {
        return request.has_index || (!request.is_query() && request.payload != "255")
                   ? CommandStatus::invalid
                   : CommandStatus::failed;
    }
    for (std::size_t index = 0; index < generated::gpio::kFunctions.size(); ++index) {
        if (index != 0U && !output.write(",")) {
            return CommandStatus::failed;
        }
        if (!output.json_string(function_base_name(index)) ||
            !output.format(
                ":%u", static_cast<unsigned int>(generated::gpio::kFunctions[index].first))) {
            return CommandStatus::failed;
        }
    }
    return output.write("}}") ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Writes the persistent user Template using Tasmota-compatible keys. */
[[nodiscard]] bool write_template(
    CommandOutput& output, const HardwareConfiguration& configuration) {
    if (!output.write("{\"NAME\":") || !output.json_string(configuration.user_template_name()) ||
        !output.write(",\"ARCH\":") || !output.json_string(configuration.architecture()) ||
        !output.write(",\"GPIO\":[")) {
        return false;
    }
    for (std::size_t slot = 0; slot < configuration.user_template_size; ++slot) {
        if (!output.format("%s%u", slot == 0U ? "" : ",",
                static_cast<unsigned int>(configuration.user_template[slot]))) {
            return false;
        }
    }
    return output.format("],\"FLAG\":%lu,\"BASE\":%u}",
        static_cast<unsigned long>(configuration.user_template_flag),
        static_cast<unsigned int>(configuration.user_template_base));
}

/** @brief Merges the current effective module and overrides into the user Template. */
[[nodiscard]] bool merge_current_template(HardwareConfiguration& candidate) {
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    if (!base_template(candidate, assignments)) {
        return false;
    }
    for (const auto& topology : generated::hardware::kPins) {
        if (topology.template_slot == generated::hardware::kNoTemplateSlot) {
            continue;
        }
        const auto function = effective_function(candidate, assignments, topology);
        if (function != 0U) {
            assignments[topology.template_slot] = function;
        }
    }
    candidate.user_template = assignments;
    candidate.user_template_size =
        static_cast<std::uint8_t>(generated::hardware::kTemplateSlotCount);
    candidate.user_template_base = candidate.module;
    if (candidate.module == 0U) {
        candidate.override_count = 0U;
    }
    return candidate.set_user_template_name("Merged");
}

/** @brief Handles Template query, copy, merge, and partial JSON forms. */
[[nodiscard]] CommandStatus template_command(
    const CommandRequest& request, Settings& settings, RestartController&, CommandOutput& output) {
    if (request.has_index) {
        return CommandStatus::invalid;
    }
    auto candidate = settings.hardware_configuration();
    bool mutation = false;
    if (!request.is_query()) {
        if (!request.payload.empty() && request.payload.front() == '{') {
            if (!parse_template(request.payload, candidate)) {
                return CommandStatus::invalid;
            }
            mutation = true;
        } else {
            std::uint32_t selection = 0;
            if (!support::parse_unsigned_decimal(request.payload, 255U, selection)) {
                return CommandStatus::invalid;
            }
            if (selection == 0U) {
                if (candidate.module != 0U && !copy_module_template(candidate.module, candidate)) {
                    return CommandStatus::invalid;
                }
                mutation = candidate.module != 0U;
            } else if (selection == 255U) {
                if (!merge_current_template(candidate)) {
                    return CommandStatus::invalid;
                }
                mutation = true;
            } else {
                if (!copy_module_template(static_cast<std::uint8_t>(selection), candidate)) {
                    return CommandStatus::invalid;
                }
                mutation = true;
            }
        }
    }

    if (mutation) {
        const auto result = settings.set_hardware_configuration(candidate);
        const auto status = update_status(result);
        if (status != CommandStatus::handled) {
            return status;
        }
    }
    return write_template(output, settings.hardware_configuration()) ? CommandStatus::handled
                                                                     : CommandStatus::failed;
}

} // namespace

bool write_gpio_catalog(CommandOutput& output) {
    if (!output.write("{\"Arch\":") || !output.json_string(generated::hardware::kArchitecture) ||
        !output.write(",\"Functions\":[")) {
        return false;
    }
    bool first = true;
    for (std::size_t index = 0; index < generated::gpio::kFunctions.size(); ++index) {
        const auto& descriptor = generated::gpio::kFunctions[index];
        if (descriptor.first == 0U) {
            continue; // None is implicit; User is the Template-only value 1.
        }
        if (!output.format("%s[%u,%u,%u,", first ? "" : ",",
                static_cast<unsigned int>(descriptor.first),
                static_cast<unsigned int>(descriptor.instances),
                static_cast<unsigned int>(descriptor.requirements)) ||
            !output.json_string(function_base_name(index)) || !output.write("]")) {
            return false;
        }
        first = false;
    }
    if (!output.write("],\"Modules\":[")) {
        return false;
    }
    for (std::size_t index = 0; index < generated::gpio::kModules.size(); ++index) {
        if (!output.format("%s[%u,", index == 0U ? "" : ",",
                static_cast<unsigned int>(generated::gpio::kModules[index].number)) ||
            !output.json_string(module_table_name(index)) || !output.write("]")) {
            return false;
        }
    }
    return output.write("]}");
}

bool write_gpio_state(CommandOutput& output, const Settings& settings) {
    const auto& configuration = settings.hardware_configuration();
    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    const bool resolved = base_template(configuration, assignments);
    if (!output.format(
            "{\"Module\":%u,\"Template\":", static_cast<unsigned int>(configuration.module)) ||
        !write_template(output, configuration) || !output.write(",\"Pins\":[")) {
        return false;
    }
    bool first = true;
    for (const auto& pin : generated::hardware::kPins) {
        const bool slotted = pin.template_slot != generated::hardware::kNoTemplateSlot;
        const auto base = resolved && slotted ? assignments[pin.template_slot] : 0U;
        const auto function =
            resolved && slotted ? effective_function(configuration, assignments, pin) : 0U;
        if (!output.format("%s[%u,%u,%u,%u,%u,%u]", first ? "" : ",",
                static_cast<unsigned int>(pin.id), static_cast<unsigned int>(pin.template_slot),
                static_cast<unsigned int>(pin.flags), static_cast<unsigned int>(pin.capabilities),
                static_cast<unsigned int>(base), static_cast<unsigned int>(function))) {
            return false;
        }
        first = false;
    }
    return output.write("]}");
}

/** @brief Dispatches application-only GPIO, Module, and Template commands. */
CommandStatus dispatch_hardware(const CommandRequest& request, Settings& settings,
    RestartController& restart, CommandOutput& output) {
    (void)restart;
    if (support::ascii_iequals(request.name, "module")) {
        return module_command(request, settings, restart, output);
    }
    if (support::ascii_iequals(request.name, "modules")) {
        return modules_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "gpio")) {
        return gpio_command(request, settings, restart, output);
    }
    if (support::ascii_iequals(request.name, "gpioread")) {
        return gpio_read_command(request, settings, output);
    }
    if (support::ascii_iequals(request.name, "gpios")) {
        return gpios_command(request, output);
    }
    if (support::ascii_iequals(request.name, "template")) {
        return template_command(request, settings, restart, output);
    }
    return CommandStatus::unknown;
}

} // namespace newmota::commands
