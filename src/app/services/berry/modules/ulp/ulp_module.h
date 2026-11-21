/*
  ulp_module.h - Tasmota's Berry ULP module for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ulp_module.h
 * @brief `import ULP`: run, load, set_mem, get_mem, gpio_init, wake_period, sleep, adc_config.
 */

#pragma once

namespace newmota {

class RestartController;

/** @brief Gives ULP.sleep the controller that closes the network and powers down. */
void install_ulp_module(RestartController& restart);

} // namespace newmota
