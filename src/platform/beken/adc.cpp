/*
  adc.cpp - Beken configured ADC adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.cpp
 * @brief Beken configured ADC adapter.
 */

#include "newmota/platform/adc.h"
#include "adc_native.h"

namespace newmota::platform::adc {
Result open(PinId pin) {
    return gpio_available(pin) && newmota_beken_adc_channel(pin.value()) >= 0 ? Result::success
                                                                              : Result::unsupported;
}

Result start(PinId pin) {
    const auto result = newmota_beken_adc_start(pin.value());
    return result == 0 ? Result::success : result == 1 ? Result::busy : Result::failed;
}

Result finish(Reading& reading) {
    const auto result = newmota_beken_adc_finish(&reading.raw, &reading.millivolts);
    reading.reference = Reference::sdk_calibrated;
    return result == 0 ? Result::success : result == 1 ? Result::busy : Result::failed;
}

Result cancel() {
    return newmota_beken_adc_cancel() == 0 ? Result::success : Result::failed;
}

Result close(PinId pin) {
    return gpio_configure_input(pin, GpioPull::none) == GpioResult::success ? Result::success
                                                                            : Result::failed;
}
} // namespace newmota::platform::adc
