/*
  adc.cpp - ESP-IDF oneshot ADC routes and calibration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.cpp
 * @brief ESP-IDF oneshot ADC routes and calibration.
 */

#include "newmota/platform/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/soc_caps.h"
#include <array>

namespace newmota::platform::adc {
namespace {
struct Route {
    PinId pin{};
    adc_cali_handle_t calibration{};
    adc_unit_t unit{};
    adc_channel_t channel{};
    bool active{};
};

std::array<Route, 16> routes;
std::array<adc_oneshot_unit_handle_t, SOC_ADC_PERIPH_NUM> units{};
bool pending{};
Reading completed{};
Result completion{Result::failed};

Route* find(PinId pin) {
    for (auto& route : routes)
        if (route.active && route.pin.value() == pin.value())
            return &route;
    return nullptr;
}
} // namespace

Result open(PinId pin) {
    if (!gpio_available(pin))
        return Result::unsupported;
    if (find(pin))
        return Result::busy;
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel(pin.value(), &unit, &channel) != ESP_OK)
        return Result::unsupported;
    Route* route{};
    for (auto& entry : routes)
        if (!entry.active) {
            route = &entry;
            break;
        }
    if (!route)
        return Result::busy;
    if (!units[unit]) {
        adc_oneshot_unit_init_cfg_t configuration{};
        configuration.unit_id = unit;
        const auto result = adc_oneshot_new_unit(&configuration, &units[unit]);
        if (result != ESP_OK)
            return result == ESP_ERR_NOT_SUPPORTED ? Result::unsupported : Result::failed;
    }
    *route = {};
    route->pin = pin;
    route->unit = unit;
    route->channel = channel;
    route->active = true;
    adc_oneshot_chan_cfg_t configuration{};
    configuration.atten = ADC_ATTEN_DB_12;
    configuration.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(units[unit], channel, &configuration) != ESP_OK)
        return Result::failed;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration{};
    calibration.unit_id = unit;
    calibration.chan = channel;
    calibration.atten = configuration.atten;
    calibration.bitwidth = configuration.bitwidth;
    const auto calibrated = adc_cali_create_scheme_curve_fitting(&calibration, &route->calibration);
    if (calibrated != ESP_OK && calibrated != ESP_ERR_NOT_SUPPORTED)
        return Result::failed;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t calibration{};
    calibration.unit_id = unit;
    calibration.atten = configuration.atten;
    calibration.bitwidth = configuration.bitwidth;
#if CONFIG_IDF_TARGET_ESP32
    calibration.default_vref = 1100;
#endif
    const auto calibrated = adc_cali_create_scheme_line_fitting(&calibration, &route->calibration);
    if (calibrated != ESP_OK && calibrated != ESP_ERR_NOT_SUPPORTED)
        return Result::failed;
#endif
    return Result::success;
}

Result start(PinId pin) {
    auto* route = find(pin);
    if (!route)
        return Result::invalid_argument;
    if (pending)
        return Result::busy;
    int raw{}, millivolts{};
    const auto result = adc_oneshot_read(units[route->unit], route->channel, &raw);
    if (result != ESP_OK)
        return result == ESP_ERR_TIMEOUT ? Result::busy : Result::failed;
    completed = {};
    completed.raw = raw;
    if (route->calibration) {
        completion = adc_cali_raw_to_voltage(route->calibration, raw, &millivolts) == ESP_OK
                         ? Result::success
                         : Result::failed;
        completed.millivolts = millivolts > 0 ? millivolts : 0;
        completed.reference = Reference::sdk_calibrated;
    } else {
        completed.millivolts =
            static_cast<std::uint32_t>(raw) * 3300 / ((1U << SOC_ADC_RTC_MAX_BITWIDTH) - 1);
        completion = Result::success;
    }
    pending = true;
    return Result::success;
}

Result finish(Reading& reading) {
    if (!pending)
        return Result::invalid_argument;
    pending = false;
    reading = completed;
    return completion;
}

Result cancel() {
    pending = false;
    return Result::success;
}

Result close(PinId pin) {
    auto* route = find(pin);
    if (!route)
        return Result::success;
    if (route->calibration) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (adc_cali_delete_scheme_curve_fitting(route->calibration) != ESP_OK)
            return Result::failed;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (adc_cali_delete_scheme_line_fitting(route->calibration) != ESP_OK)
            return Result::failed;
#endif
        route->calibration = nullptr;
    }
    const auto unit = route->unit;
    bool retained{};
    for (const auto& other : routes)
        if (&other != route && other.active && other.unit == unit)
            retained = true;
    if (!retained && units[unit]) {
        if (adc_oneshot_del_unit(units[unit]) != ESP_OK)
            return Result::failed;
        units[unit] = nullptr;
    }
    if (gpio_configure_input(pin, GpioPull::none) != GpioResult::success)
        return Result::failed;
    *route = {};
    return Result::success;
}
} // namespace newmota::platform::adc
