/*
  adc.cpp - Bouffalo ADC controller scheduling

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.cpp
 * @brief Bouffalo ADC controller scheduling.
 */

#include "newmota/platform/adc.h"
extern "C" {
#include "bflb_adc.h"
#include "bflb_gpio.h"
}

namespace newmota::platform::adc {
namespace {
bflb_device_s* device{};
bool pending{};
std::uint32_t opened{};

/** @brief BL602 external routes; skip reserved channel aliases 6/8 and non-ADC pads. */
int channel(PinId pin) {
#if defined(BL602)
    constexpr unsigned pins[]{12, 4, 14, 13, 5, 6, 255, 9, 255, 10, 11, 15};
    for (unsigned i = 0; i < 12; ++i)
        if (pins[i] == pin.value())
            return i;
#endif
    return -1;
}
} // namespace

Result open(PinId pin) {
    if (!gpio_available(pin) || channel(pin) < 0)
        return Result::unsupported;
    if (!device) {
        device = bflb_device_get_by_name("adc");
        if (!device)
            return Result::unsupported;
        bflb_adc_config_s configuration{};
        configuration.clk_div = ADC_CLK_DIV_32;
        configuration.continuous_conv_mode = true;
        configuration.resolution = ADC_RESOLUTION_16B;
        configuration.vref = ADC_VREF_3P2V;
        bflb_adc_init(device, &configuration);
    }
    opened |= std::uint32_t{1} << pin.value();
    bflb_gpio_init(
        bflb_device_get_by_name("gpio"), pin.value(), GPIO_ANALOG | GPIO_SMT_EN | GPIO_DRV_0);
    return Result::success;
}

Result start(PinId pin) {
    if (!device || channel(pin) < 0)
        return Result::invalid_argument;
    if (pending)
        return Result::busy;
    bflb_adc_channel_s selected{};
    selected.pos_chan = channel(pin);
    selected.neg_chan = ADC_CHANNEL_GND;
    bflb_adc_feature_control(device, ADC_CMD_CLR_FIFO, 0);
    if (bflb_adc_channel_config(device, &selected, 1) < 0)
        return Result::failed;
    bflb_adc_start_conversion(device);
    pending = true;
    return Result::success;
}

Result finish(Reading& reading) {
    if (!pending)
        return Result::invalid_argument;
    if (!bflb_adc_get_count(device))
        return Result::busy;
    auto raw = bflb_adc_read_raw(device);
    bflb_adc_stop_conversion(device);
    pending = false;
    bflb_adc_result_s result{};
    bflb_adc_parse_result(device, &raw, &result, 1);
    reading.raw = result.value;
    reading.millivolts = result.millivolt > 0 ? result.millivolt : 0;
    reading.reference = Reference::sdk_calibrated;
    return Result::success;
}

Result cancel() {
    if (device)
        bflb_adc_stop_conversion(device);
    pending = false;
    return Result::success;
}

Result close(PinId pin) {
    if (pin.value() >= 32 || !(opened & (std::uint32_t{1} << pin.value())))
        return Result::success;
    if (gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    opened &= ~(std::uint32_t{1} << pin.value());
    if (!opened && device) {
        bflb_adc_deinit(device);
        device = nullptr;
    }
    return Result::success;
}
} // namespace newmota::platform::adc
