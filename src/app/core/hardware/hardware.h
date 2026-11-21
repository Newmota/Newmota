/*
  hardware.h - portable GPIO assignment resolver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/core/hardware/pin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

inline constexpr std::size_t kMaximumHardwarePins = 64;
inline constexpr std::size_t kMaximumTemplateSlots = 64;
inline constexpr std::size_t kMaximumGpioOverrides = 64;
inline constexpr std::size_t kHardwareArchitectureCapacity = 16;
inline constexpr std::size_t kUserTemplateNameCapacity = 32;

enum class PinRole : std::uint8_t {
    none,
    input,
    fixed_output,
    button,
    relay,
    status_led,
    pwm,
    i2c_scl,
    i2c_sda,
    led_strip,
    dht11,
    dht22,
    si7021,
    i2s_dout,
    i2s_bclk,
    i2s_ws,
    i2s_din,
    i2s_mclk,
    link_led,
    gps_rx,
    gps_tx,
    energy_rx,
    energy_tx,
    energy_cf,
    energy_cf1,
    energy_sel,
    energy_irq,
    energy_reset,
    energy_cs,
    energy_voltage,
    energy_current,
    energy_option,
    switch_input,
    buzzer, ///< Driven by Berry: gpio.set_pwm() or digital output on the template pin.
};

enum class PinModifier : std::uint8_t {
    inverted = 1U << 0U,
    no_pull = 1U << 1U,
    pull_down = 1U << 2U,
    bistable = 1U << 3U,
    fixed = 1U << 4U,
    initial_high = 1U << 5U,
    initial_low = 1U << 6U,
};

enum class BindingState : std::uint8_t {
    active,
    unknown_function,
    unavailable_pin,
    missing_capability,
    duplicate_function,
    duplicate_pad,
};

enum class ResolutionIssueCode : std::uint8_t {
    unknown_function,
    unavailable_pin,
    missing_capability,
    duplicate_function,
    duplicate_pad,
    invalid_override_pin,
    duplicate_override,
    override_not_user,
};

enum class ResolutionPolicy : std::uint8_t {
    tolerant,
    strict,
};

enum class ResolutionError : std::uint8_t {
    none,
    invalid_template_size,
    too_many_overrides,
    module_not_found,
    invalid_assignment,
};

enum class PinInitializationError : std::uint8_t {
    invalid_role,
    unavailable,
    unsupported,
    failed,
};

struct GpioOverride final {
    PinId pin;
    std::uint16_t function;
};

struct HardwareConfiguration final {
    std::array<char, kHardwareArchitectureCapacity> architecture_storage{};
    std::array<char, kUserTemplateNameCapacity> user_template_name_storage{};
    std::array<std::uint16_t, kMaximumTemplateSlots> user_template{};
    std::array<GpioOverride, kMaximumGpioOverrides> overrides{};
    std::uint32_t user_template_flag{};
    std::uint8_t module{};
    std::uint8_t architecture_length{};
    std::uint8_t user_template_name_length{};
    std::uint8_t user_template_base{};
    std::uint8_t user_template_size{};
    std::uint8_t override_count{};

    /** @brief Returns the architecture identity retained with this configuration. */
    [[nodiscard]] std::string_view architecture() const;
    /** @brief Replaces the bounded architecture identity. */
    [[nodiscard]] bool set_architecture(std::string_view value);
    /** @brief Returns the persistent user Template display name. */
    [[nodiscard]] std::string_view user_template_name() const;
    /** @brief Replaces the bounded user Template display name. */
    [[nodiscard]] bool set_user_template_name(std::string_view value);
};

/** @brief Compares every persistent hardware-configuration field. */
[[nodiscard]] bool equal_hardware_configuration(
    const HardwareConfiguration& left, const HardwareConfiguration& right);

struct NormalizedFunction final {
    PinRole role{PinRole::none};
    std::uint8_t instance{};
    std::uint8_t modifiers{};
    std::uint16_t requirements{};
};

struct PinBinding final {
    PinId pin;
    std::uint16_t function{};
    PinRole role{PinRole::none};
    std::uint8_t instance{};
    std::uint8_t modifiers{};
    BindingState state{BindingState::unknown_function};

    /** @brief Reports whether this binding is safe for a driver to acquire. */
    [[nodiscard]] constexpr bool active() const { return state == BindingState::active; }
};

struct ResolutionIssue final {
    PinId pin;
    std::uint16_t function{};
    ResolutionIssueCode code{ResolutionIssueCode::unknown_function};
};

struct ResolutionResult final {
    ResolutionError error{ResolutionError::none};
    std::uint8_t issue_count{};

    /** @brief Reports whether the candidate may become the live configuration. */
    [[nodiscard]] constexpr bool accepted() const { return error == ResolutionError::none; }
};

struct PinInitializationFailure final {
    PinId pin;
    std::uint16_t function{};
    PinInitializationError error{PinInitializationError::failed};
};

struct HardwareStartupResult final {
    ResolutionResult resolution{};
    std::uint8_t initialized_count{};
    std::uint8_t failure_count{};
    // The template assigned a console pin, so the console stopped before GPIO setup.
    PinId console_pin{};

    /** @brief Reports whether the configuration could be resolved at startup. */
    [[nodiscard]] constexpr bool resolved() const { return resolution.accepted(); }
};

class Hardware final {
  public:
    /** @brief Returns the selected external module number. */
    [[nodiscard]] constexpr std::uint8_t module() const { return module_; }

    /** @brief Returns the number of effective pin bindings, including inactive ones. */
    [[nodiscard]] constexpr std::uint8_t binding_count() const { return binding_count_; }

    /** @brief Returns the number of bindings available to application owners. */
    [[nodiscard]] constexpr std::uint8_t active_count() const { return active_count_; }

    /** @brief Returns the number of retained resolution issues. */
    [[nodiscard]] constexpr std::uint8_t issue_count() const { return issue_count_; }

    /**
     * @brief Returns Tasmota's Option_A flags; bit 0 is Option A1.
     *
     * An Option A value on a valid pin sets its flag instead of creating a binding, so its
     * carrier pin stays free, as in Tasmota.
     */
    [[nodiscard]] constexpr std::uint32_t option_a() const { return option_a_; }

    /** @brief Returns a pin-sorted binding, or nullptr when @p index is out of range. */
    [[nodiscard]] const PinBinding* binding(std::size_t index) const;
    /** @brief Returns an active function-sorted binding, or nullptr past the end. */
    [[nodiscard]] const PinBinding* function_binding(std::size_t index) const;
    /** @brief Returns a retained issue, or nullptr when @p index is out of range. */
    [[nodiscard]] const ResolutionIssue* issue(std::size_t index) const;
    /** @brief Finds an active binding by physical pin. */
    [[nodiscard]] const PinBinding* find_pin(PinId pin) const;
    /** @brief Finds an active binding by normalized role and one-based instance. */
    [[nodiscard]] const PinBinding* find_function(PinRole role, std::uint8_t instance) const;

  private:
    friend class HardwareResolver;

    std::array<PinBinding, kMaximumHardwarePins> bindings_{};
    std::array<std::uint8_t, kMaximumHardwarePins> function_order_{};
    std::array<ResolutionIssue, kMaximumHardwarePins> issues_{};
    std::uint32_t option_a_{};
    std::uint8_t module_{};
    std::uint8_t binding_count_{};
    std::uint8_t active_count_{};
    std::uint8_t issue_count_{};
};

class HardwareResolver final {
  public:
    /**
     * @brief Resolves a hardware candidate without heap allocation.
     *
     * The caller owns @p output as a candidate object and replaces its live
     * Hardware only after strict resolution has been accepted.
     */
    [[nodiscard]] static ResolutionResult resolve(
        const HardwareConfiguration& configuration, ResolutionPolicy policy, Hardware& output);
    /** @brief Normalizes a Tasmota wire value into its qualified runtime role. */
    [[nodiscard]] static bool normalize(std::uint16_t function, NormalizedFunction& output);
};

class HardwareRuntime final {
  public:
    /**
     * @brief Resolves persistent hardware and initializes core-owned GPIO roles.
     * @return Resolution and native initialization counts for startup logging.
     */
    [[nodiscard]] HardwareStartupResult initialize(const HardwareConfiguration& configuration);

    /** @brief Returns the immutable live hardware view resolved at startup. */
    [[nodiscard]] constexpr const Hardware& hardware() const { return hardware_; }

    /** @brief Decodes one retained native initialization failure. */
    [[nodiscard]] bool failure(std::size_t index, PinInitializationFailure& output) const;

  private:
    Hardware hardware_{};
    std::array<std::uint8_t, kMaximumHardwarePins> failures_{};
    std::uint8_t failure_count_{};
};

} // namespace newmota
