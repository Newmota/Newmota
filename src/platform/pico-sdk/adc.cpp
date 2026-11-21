/*
  adc.cpp - Pico single-ended ADC conversions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.cpp
 * @brief Pico single-ended ADC conversions.
 */

#include "newmota/platform/adc.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"

namespace newmota::platform::adc {
namespace {
bool pending{}, initialized{};
std::uint64_t opened{};

/** @brief RP2350B exposes GPIO40..47; A packages and RP2040 expose GPIO26..29. */
int channel(PinId pin) {
#if defined(CYW43_USES_VSYS_PIN) && CYW43_USES_VSYS_PIN
    // This pad is the Wi-Fi clock between SDK-owned VSYS measurement windows.
    if (pin.value() == PICO_VSYS_PIN)
        return -1;
#endif
#if PICO_RP2350 && NUM_BANK0_GPIOS > 32
    return pin.value() >= 40 && pin.value() <= 47 ? pin.value() - 40 : -1;
#else
    return pin.value() >= 26 && pin.value() <= 29 ? pin.value() - 26 : -1;
#endif
}
} // namespace

Result open(PinId pin) {
    if (channel(pin) < 0)
        return Result::unsupported;
    if (!initialized) {
        adc_init();
        initialized = true;
    }
    opened |= std::uint64_t{1} << pin.value();
    adc_gpio_init(pin.value());
    return Result::success;
}

Result start(PinId pin) {
    if (channel(pin) < 0)
        return Result::unsupported;
    if (pending)
        return Result::busy;
    adc_select_input(channel(pin));
    hw_set_bits(&adc_hw->cs, ADC_CS_START_ONCE_BITS);
    pending = true;
    return Result::success;
}

Result finish(Reading& value) {
    if (!pending)
        return Result::invalid_argument;
    if (!(adc_hw->cs & ADC_CS_READY_BITS))
        return Result::busy;
    pending = false;
    if (adc_hw->cs & ADC_CS_ERR_BITS)
        return Result::failed;
    value.raw = adc_hw->result;
    value.millivolts = value.raw * 3300 / 4095;
    value.reference = Reference::nominal;
    return Result::success;
}

Result cancel() {
    if (pending && !(adc_hw->cs & ADC_CS_READY_BITS))
        return Result::failed;
    pending = false;
    return Result::success;
}

Result close(PinId pin) {
    if (channel(pin) < 0 || !(opened & (std::uint64_t{1} << pin.value())))
        return Result::success;
    gpio_init(pin.value());
    gpio_set_dir(pin.value(), false);
    gpio_disable_pulls(pin.value());
    opened &= ~(std::uint64_t{1} << pin.value());
    if (!opened) {
        hw_clear_bits(&adc_hw->cs, ADC_CS_EN_BITS);
        initialized = false;
    }
    return Result::success;
}
} // namespace newmota::platform::adc
