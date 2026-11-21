/*
  adc.cpp - AmebaD calibrated nonblocking ADC conversions

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.cpp
 * @brief AmebaD calibrated nonblocking ADC conversions.
 */

#include "newmota/platform/adc.h"
extern "C" {
#include "ameba_soc.h"
}

namespace newmota::platform::adc {
namespace {
bool pending{}, initialized{};
std::uint64_t opened{};

int channel(PinId pin) {
    const auto value = pin.value();
    if (value >= 36 && value <= 39)
        return value - 36;
    if (value >= 33 && value <= 35)
        return value - 29;
    return -1;
}
} // namespace

Result open(PinId pin) {
    if (channel(pin) < 0)
        return Result::unsupported;
    if (!initialized) {
        ADC_InitTypeDef configuration;
        ADC_StructInit(&configuration);
        configuration.ADC_CvlistLen = 0;
        ADC_Init(&configuration);
        ADC_Cmd(ENABLE);
        initialized = true;
    }
    opened |= std::uint64_t{1} << pin.value();
    PAD_CMD(pin.value(), DISABLE);
    return Result::success;
}

Result start(PinId pin) {
    if (channel(pin) < 0)
        return Result::unsupported;
    if (pending)
        return Result::busy;
    ADC->ADC_CHSW_LIST[0] = channel(pin);
    ADC_ClearFIFO();
    ADC_SWTrigCmd(ENABLE);
    pending = true;
    return Result::success;
}

Result finish(Reading& value) {
    if (!pending)
        return Result::invalid_argument;
    if (!ADC_Readable())
        return Result::busy;
    ADC_SWTrigCmd(DISABLE);
    pending = false;
    value.raw = ADC_Read() & BIT_MASK_DAT_GLOBAL;
    const auto millivolts = ADC_GetVoltage(value.raw);
    value.millivolts = millivolts > 0 ? millivolts : 0;
    value.reference = Reference::sdk_calibrated;
    return Result::success;
}

Result cancel() {
    ADC_SWTrigCmd(DISABLE);
    ADC_ClearFIFO();
    pending = false;
    return Result::success;
}

Result close(PinId pin) {
    if (channel(pin) >= 0 && (opened & (std::uint64_t{1} << pin.value()))) {
        PAD_CMD(pin.value(), ENABLE);
        PAD_PullCtrl(pin.value(), GPIO_PuPd_NOPULL);
        opened &= ~(std::uint64_t{1} << pin.value());
        if (!opened) {
            ADC_Cmd(DISABLE);
            initialized = false;
        }
    }
    return Result::success;
}
} // namespace newmota::platform::adc
