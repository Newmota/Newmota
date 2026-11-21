/*
  hardware.cpp - portable GPIO assignment resolver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/hardware/hardware.h"

#include "gpio_ids.h"
#include "pin_topology.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {
namespace {

using generated::gpio::FunctionDescriptor;
using generated::gpio::ModuleDescriptor;
using generated::hardware::PinTopology;

static_assert(generated::hardware::kPinCount <= kMaximumHardwarePins);
static_assert(generated::hardware::kTemplateSlotCount <= kMaximumTemplateSlots);
static_assert(static_cast<std::uint8_t>(PinRole::none) == generated::gpio::kRoleNone);
static_assert(static_cast<std::uint8_t>(PinRole::input) == generated::gpio::kRoleInput);
static_assert(
    static_cast<std::uint8_t>(PinRole::fixed_output) == generated::gpio::kRoleFixedOutput);
static_assert(static_cast<std::uint8_t>(PinRole::button) == generated::gpio::kRoleButton);
static_assert(static_cast<std::uint8_t>(PinRole::switch_input) == generated::gpio::kRoleSwitch);
static_assert(static_cast<std::uint8_t>(PinRole::buzzer) == generated::gpio::kRoleBuzzer);
static_assert(static_cast<std::uint8_t>(PinRole::relay) == generated::gpio::kRoleRelay);
static_assert(static_cast<std::uint8_t>(PinRole::status_led) == generated::gpio::kRoleStatusLed);
static_assert(static_cast<std::uint8_t>(PinRole::link_led) == generated::gpio::kRoleLinkLed);
static_assert(static_cast<std::uint8_t>(PinRole::dht11) == generated::gpio::kRoleDht11);
static_assert(static_cast<std::uint8_t>(PinRole::dht22) == generated::gpio::kRoleDht22);
static_assert(static_cast<std::uint8_t>(PinRole::si7021) == generated::gpio::kRoleSi7021);
static_assert(static_cast<std::uint16_t>(PinCapability::input) == generated::hardware::kInput);
static_assert(static_cast<std::uint16_t>(PinCapability::output) == generated::hardware::kOutput);
static_assert(static_cast<std::uint16_t>(PinCapability::pull_up) == generated::hardware::kPullUp);
static_assert(
    static_cast<std::uint16_t>(PinCapability::pull_down) == generated::hardware::kPullDown);
static_assert(
    static_cast<std::uint16_t>(PinCapability::open_drain) == generated::hardware::kOpenDrain);
static_assert(
    static_cast<std::uint16_t>(PinCapability::interrupt) == generated::hardware::kInterrupt);
static_assert(static_cast<std::uint16_t>(PinCapability::analog) == generated::hardware::kAnalog);
static_assert(
    static_cast<std::uint8_t>(PinModifier::inverted) == generated::gpio::kModifierInverted);
static_assert(static_cast<std::uint8_t>(PinModifier::no_pull) == generated::gpio::kModifierNoPull);
static_assert(
    static_cast<std::uint8_t>(PinModifier::pull_down) == generated::gpio::kModifierPullDown);
static_assert(
    static_cast<std::uint8_t>(PinModifier::bistable) == generated::gpio::kModifierBistable);
static_assert(static_cast<std::uint8_t>(PinModifier::fixed) == generated::gpio::kModifierFixed);
static_assert(
    static_cast<std::uint8_t>(PinModifier::initial_high) == generated::gpio::kModifierInitialHigh);
static_assert(
    static_cast<std::uint8_t>(PinModifier::initial_low) == generated::gpio::kModifierInitialLow);

/** @brief Finds generated topology facts for one physical pin. */
[[nodiscard]] const PinTopology* topology_for(const PinId pin) {
    for (const auto& candidate : generated::hardware::kPins) {
        if (candidate.id == pin.value()) {
            return &candidate;
        }
    }
    return nullptr;
}

/** @brief Finds a generated built-in module by its external number. */
[[nodiscard]] const ModuleDescriptor* module_for(const std::uint8_t number) {
    for (const auto& candidate : generated::gpio::kModules) {
        if (candidate.number == number) {
            return &candidate;
        }
    }
    return nullptr;
}

/** @brief Maps an inactive binding state to its diagnostic issue code. */
[[nodiscard]] ResolutionIssueCode issue_for(const BindingState state) {
    switch (state) {
    case BindingState::unknown_function:
        return ResolutionIssueCode::unknown_function;
    case BindingState::unavailable_pin:
        return ResolutionIssueCode::unavailable_pin;
    case BindingState::missing_capability:
        return ResolutionIssueCode::missing_capability;
    case BindingState::duplicate_function:
        return ResolutionIssueCode::duplicate_function;
    case BindingState::duplicate_pad:
        return ResolutionIssueCode::duplicate_pad;
    case BindingState::active:
        break;
    }
    return ResolutionIssueCode::unknown_function;
}

} // namespace

/** @brief Returns the bounded architecture identity as a non-owning view. */
std::string_view HardwareConfiguration::architecture() const {
    return {architecture_storage.data(), architecture_length};
}

/** @brief Validates and copies one architecture identity into fixed storage. */
bool HardwareConfiguration::set_architecture(const std::string_view value) {
    if (value.empty() || value.size() > architecture_storage.size() ||
        value.find('\0') != std::string_view::npos) {
        return false;
    }
    architecture_storage = {};
    for (std::size_t index = 0; index < value.size(); ++index) {
        architecture_storage[index] = value[index];
    }
    architecture_length = static_cast<std::uint8_t>(value.size());
    return true;
}

/** @brief Returns the bounded user Template name as a non-owning view. */
std::string_view HardwareConfiguration::user_template_name() const {
    return {user_template_name_storage.data(), user_template_name_length};
}

/** @brief Validates and copies one user Template name into fixed storage. */
bool HardwareConfiguration::set_user_template_name(const std::string_view value) {
    if (value.size() > user_template_name_storage.size() ||
        value.find('\0') != std::string_view::npos) {
        return false;
    }
    user_template_name_storage = {};
    for (std::size_t index = 0; index < value.size(); ++index) {
        user_template_name_storage[index] = value[index];
    }
    user_template_name_length = static_cast<std::uint8_t>(value.size());
    return true;
}

/** @brief Compares every field represented by the persistent hardware tag. */
bool equal_hardware_configuration(
    const HardwareConfiguration& left, const HardwareConfiguration& right) {
    if (left.architecture_storage != right.architecture_storage ||
        left.user_template_name_storage != right.user_template_name_storage ||
        left.user_template != right.user_template ||
        left.user_template_flag != right.user_template_flag || left.module != right.module ||
        left.architecture_length != right.architecture_length ||
        left.user_template_name_length != right.user_template_name_length ||
        left.user_template_base != right.user_template_base ||
        left.user_template_size != right.user_template_size ||
        left.override_count != right.override_count) {
        return false;
    }
    for (std::size_t index = 0; index < left.overrides.size(); ++index) {
        if (left.overrides[index].pin.value() != right.overrides[index].pin.value() ||
            left.overrides[index].function != right.overrides[index].function) {
            return false;
        }
    }
    return true;
}

/** @brief Returns a pin-sorted binding, or nullptr for an invalid index. */
const PinBinding* Hardware::binding(const std::size_t index) const {
    return index < binding_count_ ? &bindings_[index] : nullptr;
}

/** @brief Returns one active binding through the function-sorted index. */
const PinBinding* Hardware::function_binding(const std::size_t index) const {
    return index < active_count_ ? &bindings_[function_order_[index]] : nullptr;
}

/** @brief Returns a retained issue, or nullptr for an invalid index. */
const ResolutionIssue* Hardware::issue(const std::size_t index) const {
    return index < issue_count_ ? &issues_[index] : nullptr;
}

/** @brief Finds an active binding by its physical pin identifier. */
const PinBinding* Hardware::find_pin(const PinId pin) const {
    for (std::size_t index = 0; index < binding_count_; ++index) {
        const auto& candidate = bindings_[index];
        if (candidate.pin.value() == pin.value()) {
            if (candidate.active()) {
                return &candidate;
            }
            continue;
        }
        if (candidate.pin.value() > pin.value()) {
            break;
        }
    }
    return nullptr;
}

/** @brief Finds an active binding by normalized role and instance. */
const PinBinding* Hardware::find_function(const PinRole role, const std::uint8_t instance) const {
    for (std::size_t order = 0; order < active_count_; ++order) {
        const auto& candidate = bindings_[function_order_[order]];
        if (candidate.role == role && candidate.instance == instance) {
            return &candidate;
        }
    }
    return nullptr;
}

/** @brief Normalizes a qualified Tasmota GPIO value using generated descriptors. */
bool HardwareResolver::normalize(const std::uint16_t function, NormalizedFunction& output) {
    for (const FunctionDescriptor& descriptor : generated::gpio::kFunctions) {
        if (function < descriptor.first) {
            break;
        }
        const auto ordinal = static_cast<std::uint16_t>(function - descriptor.first);
        if (ordinal >= descriptor.instances) {
            continue;
        }
        output.role = static_cast<PinRole>(descriptor.role);
        output.instance = static_cast<std::uint8_t>(ordinal + 1U);
        output.modifiers = descriptor.modifiers;
        output.requirements = descriptor.requirements;
        return true;
    }
    output = {};
    return false;
}

/** @brief Resolves module, Template, and sparse GPIO layers into fixed arrays. */
ResolutionResult HardwareResolver::resolve(
    const HardwareConfiguration& configuration, const ResolutionPolicy policy, Hardware& output) {
    output.module_ = configuration.module;
    output.binding_count_ = 0U;
    output.active_count_ = 0U;
    output.issue_count_ = 0U;
    output.option_a_ = 0U;

    std::array<std::uint16_t, kMaximumTemplateSlots> assignments{};
    if (configuration.module == 0U) {
        if (configuration.user_template_size != generated::hardware::kTemplateSlotCount) {
            return {ResolutionError::invalid_template_size, 0U};
        }
        for (std::size_t slot = 0; slot < generated::hardware::kTemplateSlotCount; ++slot) {
            assignments[slot] = configuration.user_template[slot];
        }
    } else {
        const ModuleDescriptor* const module = module_for(configuration.module);
        if (module == nullptr) {
            return {ResolutionError::module_not_found, 0U};
        }
        for (std::size_t index = 0; index < module->assignment_count; ++index) {
            const auto& assignment =
                generated::gpio::kModuleAssignments[module->assignment_offset + index];
            assignments[assignment.slot] = assignment.function;
        }
    }

    if (configuration.override_count > kMaximumGpioOverrides) {
        return {ResolutionError::too_many_overrides, 0U};
    }

    auto add_issue = [&output](const PinId pin, const std::uint16_t function,
                         const ResolutionIssueCode code) {
        if (output.issue_count_ < output.issues_.size()) {
            output.issues_[output.issue_count_++] = {pin, function, code};
        }
    };
    std::array<bool, kMaximumTemplateSlots> overridden{};
    for (std::size_t index = 0; index < configuration.override_count; ++index) {
        const auto& override = configuration.overrides[index];
        const PinTopology* const topology = topology_for(override.pin);
        if (topology == nullptr ||
            topology->template_slot == generated::hardware::kNoTemplateSlot) {
            add_issue(override.pin, override.function, ResolutionIssueCode::invalid_override_pin);
            continue;
        }
        const std::size_t slot = topology->template_slot;
        if (overridden[slot]) {
            add_issue(override.pin, override.function, ResolutionIssueCode::duplicate_override);
            continue;
        }
        overridden[slot] = true;
        if (assignments[slot] != generated::gpio::kTemplateUserValue) {
            add_issue(override.pin, override.function, ResolutionIssueCode::override_not_user);
            continue;
        }
        assignments[slot] = override.function;
    }

    for (std::size_t slot = 0; slot < generated::hardware::kTemplateSlotCount; ++slot) {
        const std::uint16_t function = assignments[slot];
        if (function == 0U || function == generated::gpio::kTemplateUserValue) {
            continue;
        }

        PinBinding binding{};
        binding.pin = PinId{generated::hardware::kTemplateToPhysical[slot]};
        binding.function = function;
        NormalizedFunction normalized{};
        if (!normalize(function, normalized)) {
            binding.state = BindingState::unknown_function;
        } else {
            binding.role = normalized.role;
            binding.instance = normalized.instance;
            binding.modifiers = normalized.modifiers;
            const PinTopology* const topology = topology_for(binding.pin);
            if (topology == nullptr ||
                (topology->flags & generated::hardware::kPinAvailable) == 0U) {
                binding.state = BindingState::unavailable_pin;
            } else if ((topology->capabilities & normalized.requirements) !=
                       normalized.requirements) {
                binding.state = BindingState::missing_capability;
            } else {
                binding.state = BindingState::active;
            }
        }
        if (binding.active() && binding.role == PinRole::energy_option) {
            // Tasmota consumes Option_A as a flag; a repeated value sets the same bit.
            output.option_a_ |= std::uint32_t{1} << (binding.instance - 1U);
            continue;
        }
        output.bindings_[output.binding_count_++] = binding;
        if (!binding.active()) {
            add_issue(binding.pin, binding.function, issue_for(binding.state));
        }
    }

    std::array<bool, kMaximumHardwarePins> duplicate_pads{};
    std::array<bool, kMaximumHardwarePins> duplicate_functions{};
    for (std::size_t left = 0; left < output.binding_count_; ++left) {
        const auto& first = output.bindings_[left];
        if (!first.active()) {
            continue;
        }
        for (std::size_t right = left + 1U; right < output.binding_count_; ++right) {
            const auto& second = output.bindings_[right];
            if (!second.active()) {
                continue;
            }
            if (first.pin.value() == second.pin.value()) {
                duplicate_pads[left] = true;
                duplicate_pads[right] = true;
            }
            // Role-less functions such as SPI or webcam pins are distinct by their value.
            const bool same_function = first.role == PinRole::none
                                           ? first.function == second.function
                                           : first.instance == second.instance;
            if (first.role == second.role && same_function) {
                duplicate_functions[left] = true;
                duplicate_functions[right] = true;
            }
        }
    }

    for (std::size_t index = 0; index < output.binding_count_; ++index) {
        auto& binding = output.bindings_[index];
        if (!binding.active()) {
            continue;
        }
        if (duplicate_pads[index]) {
            binding.state = BindingState::duplicate_pad;
            add_issue(binding.pin, binding.function, ResolutionIssueCode::duplicate_pad);
        } else if (duplicate_functions[index]) {
            binding.state = BindingState::duplicate_function;
            add_issue(binding.pin, binding.function, ResolutionIssueCode::duplicate_function);
        }
    }

    for (std::size_t index = 1; index < output.binding_count_; ++index) {
        const PinBinding value = output.bindings_[index];
        std::size_t position = index;
        while (position > 0U && output.bindings_[position - 1U].pin.value() > value.pin.value()) {
            output.bindings_[position] = output.bindings_[position - 1U];
            --position;
        }
        output.bindings_[position] = value;
    }

    for (std::size_t index = 0; index < output.binding_count_; ++index) {
        if (!output.bindings_[index].active()) {
            continue;
        }
        std::size_t position = output.active_count_;
        while (position > 0U) {
            const auto previous = output.function_order_[position - 1U];
            const auto& previous_binding = output.bindings_[previous];
            const auto& value = output.bindings_[index];
            if (previous_binding.role < value.role ||
                (previous_binding.role == value.role &&
                    previous_binding.instance <= value.instance)) {
                break;
            }
            output.function_order_[position] = previous;
            --position;
        }
        output.function_order_[position] = static_cast<std::uint8_t>(index);
        ++output.active_count_;
    }

    const bool invalid = output.issue_count_ != 0U;
    return {invalid && policy == ResolutionPolicy::strict ? ResolutionError::invalid_assignment
                                                          : ResolutionError::none,
        output.issue_count_};
}

} // namespace newmota
