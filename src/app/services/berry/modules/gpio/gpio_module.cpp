/*
  gpio_module.cpp - Berry GPIO module backed by the portable hardware model

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/berry/modules/gpio/gpio_module.h"
#include "core/hardware/hardware.h"
#include "core/hardware/runtime_pin_claims.h"
#include "gpio_ids.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/pwm.h"

extern "C" {
#include "berry.h"
}

namespace {
namespace gpio = newmota::platform;
const newmota::Hardware* hardware = nullptr;
enum class ScriptPinMode : std::uint8_t {
    unconfigured,
    input,
    output,
    pwm,
};

// Tasmota's default PWM carrier and its 10-bit duty range.
constexpr std::uint32_t kDefaultPwmFrequency = 977U;
constexpr bint kPwmRange = 1023;

struct ScriptPin final {
    newmota::PinId pin;
    ScriptPinMode mode{ScriptPinMode::unconfigured};
    bool faulted{};
    gpio::PwmHandle pwm{};
    std::uint32_t frequency{kDefaultPwmFrequency};
    std::uint16_t duty{};
};

// Claims survive VM teardown; uncertain native outputs cannot be reassigned.
std::array<ScriptPin, newmota::kMaximumHardwarePins> script_pins{};

ScriptPin* script_pin(newmota::PinId pin) {
    for (auto& claim : script_pins) {
        if (claim.pin.value() == pin.value())
            return &claim;
    }
    return nullptr;
}

ScriptPin* claim_script_pin(bvm* vm, newmota::PinId pin) {
    if (auto* claim = script_pin(pin); claim != nullptr)
        return claim;
    for (auto& candidate : script_pins) {
        if (!candidate.pin.valid()) {
            candidate.pin = pin;
            return &candidate;
        }
    }
    be_raise(vm, "runtime_error", "Berry GPIO pin capacity exceeded");
    return nullptr;
}

void arguments(bvm* vm, int minimum, int maximum) {
    if (be_top(vm) < minimum || be_top(vm) > maximum)
        be_raise(vm, "type_error", "gpio expects integer arguments");
    for (int index = 1; index <= be_top(vm); ++index) {
        if (!be_isint(vm, index))
            be_raise(vm, "type_error", "gpio expects integer arguments");
    }
}

/** @brief Converts Berry's base identity and zero-based index into a wire value. */
const newmota::PinBinding* mapping(bvm* vm) {
    arguments(vm, 1, 2);
    const bint base = be_toint(vm, 1);
    const bint index = be_top(vm) == 2 ? be_toint(vm, 2) : 0;
    if (base < 0 || base >= newmota::generated::gpio::kSensorEnd || index < 0 ||
        index > newmota::generated::gpio::kInstanceMask)
        be_raise(vm, "value_error", "invalid GPIO identity or index");
    if (hardware == nullptr)
        be_raise(vm, "runtime_error", "GPIO hardware is unavailable");
    const auto function = newmota::generated::gpio::encode(
        static_cast<std::uint16_t>(base), static_cast<std::uint16_t>(index + 1));
    for (std::size_t i = 0; i < hardware->active_count(); ++i) {
        const auto* binding = hardware->function_binding(i);
        if (binding->function == function)
            return binding;
    }
    return nullptr;
}

newmota::PinId physical_pin(bvm* vm, bool mutate) {
    const bint value = be_toint(vm, 1);
    if (value < 0 || value >= newmota::PinId::kInvalid)
        be_raise(vm, "value_error", "invalid physical GPIO");
    const newmota::PinId pin{static_cast<std::uint16_t>(value)};
    if (hardware == nullptr)
        be_raise(vm, "runtime_error", "GPIO hardware is unavailable");
    if (!gpio::gpio_available(pin))
        be_raise(vm, "value_error", "physical GPIO is unavailable");
    const auto owner = newmota::runtime_pins::owner(pin);
    if (mutate && owner && owner != newmota::runtime_pins::kGpio)
        be_raise(vm, "runtime_error", "physical GPIO is owned by a peripheral");
    auto* claim = script_pin(pin);
    if (mutate && claim != nullptr && claim->faulted)
        be_raise(vm, "runtime_error", "physical GPIO is faulted until restart");
    // Inactive assignments remain reserved: a failed owner is not a free pad. An active Buzzer
    // belongs to Berry.
    for (std::size_t i = 0; i < hardware->binding_count(); ++i) {
        const auto* binding = hardware->binding(i);
        if (binding->pin.value() == pin.value() && (mutate || !binding->active()) &&
            !(binding->role == newmota::PinRole::buzzer && binding->active()))
            be_raise(vm, "runtime_error", "physical GPIO is reserved by its configuration");
    }
    return pin;
}

/** @brief Reports whether @p pin is a Buzzer_i template pin. */
bool inverted_buzzer(newmota::PinId pin) {
    const auto* binding = hardware->find_pin(pin);
    return binding && binding->role == newmota::PinRole::buzzer &&
           (binding->modifiers & static_cast<std::uint8_t>(newmota::PinModifier::inverted));
}

/** @brief Stops a script pin's PWM output, if it has one. */
void stop_pwm(ScriptPin& claim) {
    if (claim.pwm.valid())
        (void)gpio::pwm_release(claim.pwm);
    claim.pwm = {};
}

/** @brief Starts a script pin's PWM output at its frequency and duty. */
void start_pwm(bvm* vm, ScriptPin& claim) {
    gpio::PwmRequest request{};
    request.logical_output = {gpio::PwmOutputOwner::berry, claim.pin.value()};
    request.pin = claim.pin;
    request.frequency_hz = claim.frequency;
    request.max_error_ppm = gpio::kMaximumPwmErrorPpm;
    request.inverted = inverted_buzzer(claim.pin);
    request.initial_duty = claim.duty;
    gpio::PwmPlanToken token{};
    gpio::PwmRequestResult allocation{};
    if (gpio::pwm_prepare(&request, 1U, token, &allocation) != gpio::PwmResult::success ||
        allocation.result != gpio::PwmResult::success ||
        gpio::pwm_activate(token, &allocation, 1U) != gpio::PwmResult::success ||
        allocation.result != gpio::PwmResult::success)
        be_raise(vm, "runtime_error", "PWM output cannot be started on this GPIO");
    claim.pwm = allocation.handle;
    claim.mode = ScriptPinMode::pwm;
}

/** @brief Claims a script pin for PWM, as Tasmota's analogWrite attaches a pin on first use. */
ScriptPin& pwm_pin(bvm* vm) {
    const auto pin = physical_pin(vm, true);
    const bool had_claim = script_pin(pin) != nullptr;
    auto* claim = claim_script_pin(vm, pin);
    if (!newmota::runtime_pins::claim(newmota::runtime_pins::kGpio, &pin, 1)) {
        if (!had_claim)
            *claim = ScriptPin{};
        be_raise(vm, "runtime_error", "physical GPIO cannot be claimed");
    }
    return *claim;
}

void result(bvm* vm, newmota::PinId pin, gpio::GpioResult value) {
    switch (value) {
    case gpio::GpioResult::success:
        return;
    case gpio::GpioResult::unavailable:
        be_raise(vm, "value_error", "physical GPIO is unavailable");
        break;
    case gpio::GpioResult::unsupported:
        be_raise(vm, "value_error", "GPIO operation is unsupported on this pin");
        break;
    case gpio::GpioResult::failed:
        if (auto* claim = script_pin(pin); claim != nullptr)
            claim->faulted = true;
        be_raise(vm, "runtime_error", "native GPIO operation failed");
        break;
    }
}

int PinUsed(bvm* vm) {
    bool used = mapping(vm) != nullptr;
    // GPIO_OPTION_A queries Tasmota's Option_A flags; its carrier is not a pin.
    if (be_toint(vm, 1) == 194)
        used = (hardware->option_a() >> (be_top(vm) == 2 ? be_toint(vm, 2) : 0)) & 1U;
    be_pushbool(vm, used);
    be_return(vm);
}

int Pin(bvm* vm) {
    const auto* binding = mapping(vm);
    be_pushint(vm, binding == nullptr ? -1 : binding->pin.value());
    be_return(vm);
}

int DigitalRead(bvm* vm) {
    arguments(vm, 1, 1);
    const auto pin = physical_pin(vm, false);
    bool high = false;
    result(vm, pin, gpio::gpio_read(pin, high));
    be_pushint(vm, high ? 1 : 0);
    be_return(vm);
}

int DigitalWrite(bvm* vm) {
    arguments(vm, 2, 2);
    const bint value = be_toint(vm, 2);
    if (value != 0 && value != 1)
        be_raise(vm, "value_error", "digital level must be LOW or HIGH");
    const auto pin = physical_pin(vm, true);
    auto* claim = script_pin(pin);
    if (claim == nullptr || claim->mode != ScriptPinMode::output)
        be_raise(vm, "runtime_error", "configure GPIO as OUTPUT before writing");
    result(vm, pin, gpio::gpio_write(pin, value != 0));
    be_return_nil(vm);
}

int PinMode(bvm* vm) {
    arguments(vm, 2, 2);
    const bint mode = be_toint(vm, 2);
    ScriptPinMode script_mode{};
    gpio::GpioPull pull = gpio::GpioPull::none;
    switch (mode) {
    case 1:
        script_mode = ScriptPinMode::input;
        break;
    case 2:
        script_mode = ScriptPinMode::output;
        break;
    case 4:
    case 5:
        script_mode = ScriptPinMode::input;
        pull = gpio::GpioPull::up;
        break;
    case 8:
    case 9:
        script_mode = ScriptPinMode::input;
        pull = gpio::GpioPull::down;
        break;
    case 16:
    case 18:
    case -1:
        be_raise(vm, "value_error", "GPIO open-drain and DAC modes are not implemented");
        break;
    default:
        be_raise(vm, "value_error", "invalid GPIO mode");
        break;
    }
    const auto pin = physical_pin(vm, true);
    const bool had_claim = script_pin(pin) != nullptr;
    auto* claim = claim_script_pin(vm, pin);
    if (!newmota::runtime_pins::claim(newmota::runtime_pins::kGpio, &pin, 1)) {
        if (!had_claim)
            *claim = ScriptPin{};
        be_raise(vm, "runtime_error", "physical GPIO cannot be claimed");
    }
    stop_pwm(*claim);
    const gpio::GpioResult configured = script_mode == ScriptPinMode::output
                                            ? gpio::gpio_configure_output(pin, false)
                                            : gpio::gpio_configure_input(pin, pull);
    if (configured == gpio::GpioResult::success) {
        claim->mode = script_mode;
    } else if (configured == gpio::GpioResult::failed) {
        claim->faulted = true;
        result(vm, pin, configured);
    } else {
        if (!had_claim) {
            *claim = ScriptPin{};
            newmota::runtime_pins::release(newmota::runtime_pins::kGpio, pin);
        }
        result(vm, pin, configured);
    }
    be_return_nil(vm);
}

/** @brief Tasmota's `gpio.set_pwm(pin, duty[, phase])`: duty 0..1023; phase must be 0. */
int SetPwm(bvm* vm) {
    arguments(vm, 2, 3);
    const bint duty = be_toint(vm, 2);
    if (duty < 0 || duty > kPwmRange)
        be_raise(vm, "value_error", "PWM duty must be 0..1023");
    if (be_top(vm) == 3 && be_toint(vm, 3) != 0)
        be_raise(vm, "value_error", "PWM phase is not supported");
    auto& claim = pwm_pin(vm);
    claim.duty = static_cast<std::uint16_t>(duty * 65535 / kPwmRange);
    if (!claim.pwm.valid()) {
        start_pwm(vm, claim);
        be_return_nil(vm);
    }
    const gpio::PwmDutyUpdate update{claim.pwm, claim.duty};
    gpio::PwmOperationResult applied{};
    if (gpio::pwm_write_batch(&update, 1U, &applied) != gpio::PwmResult::success ||
        applied.result != gpio::PwmResult::success)
        be_raise(vm, "runtime_error", "PWM duty write failed");
    be_return_nil(vm);
}

/** @brief Tasmota's `gpio.set_pwm_freq(pin, freq)`. The output restarts at the new carrier, on
 * its own timing resource when others share the old one; a pin not yet used for PWM starts at
 * duty 0.
 */
int SetPwmFreq(bvm* vm) {
    arguments(vm, 2, 2);
    const bint frequency = be_toint(vm, 2);
    if (frequency <= 0)
        be_raise(vm, "value_error", "PWM frequency must be positive");
    auto& claim = pwm_pin(vm);
    stop_pwm(claim);
    claim.frequency = static_cast<std::uint32_t>(frequency);
    start_pwm(vm, claim);
    be_return_nil(vm);
}
} // namespace

namespace newmota {
void install_gpio_module(const Hardware* view) {
    hardware = view;
    runtime_pins::initialize(view);
}

void remove_gpio_module() {
    hardware = nullptr;
}
} // namespace newmota

extern "C" {
#define GPIO_ID(name, value) be_native_module_int(name, value),
be_native_module_attr_table(gpio){be_native_module_function("pin_used", PinUsed),
    be_native_module_function("pin", Pin), be_native_module_function("digital_read", DigitalRead),
    be_native_module_function("digital_write", DigitalWrite),
    be_native_module_function("pin_mode", PinMode), be_native_module_function("set_pwm", SetPwm),
    be_native_module_function("set_pwm_freq", SetPwmFreq), be_native_module_int("LOW", 0),
    be_native_module_int("HIGH", 1), be_native_module_int("INPUT", 1),
    be_native_module_int("OUTPUT", 2), be_native_module_int("PULLUP", 4),
    be_native_module_int("INPUT_PULLUP", 5), be_native_module_int("PULLDOWN", 8),
    be_native_module_int("INPUT_PULLDOWN", 9), be_native_module_int("OPEN_DRAIN", 16),
    be_native_module_int("OUTPUT_OPEN_DRAIN", 18), be_native_module_int("DAC", -1),
    NEWMOTA_BERRY_GPIO_IDENTITIES(GPIO_ID)};
#undef GPIO_ID
extern const bntvmodule_t be_native_module_gpio = {
    "gpio", gpio_attrs, sizeof(gpio_attrs) / sizeof(gpio_attrs[0]), nullptr};
}
