/*
  light_web.h - browser view of configured Light endpoints

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file light_web.h
 * @brief Source-neutral JSON contract for the Home light controls.
 *
 * The page sees each endpoint only as a control kind (`dimmer`, `ct`, `rgb`,
 * `rgbw`, `rgbct`) plus its state, and `Mode` (`white` or `color`) for a light that switches
 * between them. Output backends such as PWM or WS2812 stay
 * below LightService; another light source would provide the same contract here.
 */
#pragma once

#include <string_view>

namespace newmota {
class CommandOutput;
class LightService;

namespace light_web {

enum class Result {
    ok,
    invalid,
    unavailable,
    failed,
};

/** @brief Writes `{"Lights":[...]}` for every configured endpoint; empty when none. */
[[nodiscard]] bool write_lights(CommandOutput& output, const LightService* light);

/**
 * @brief Applies one form-encoded endpoint mutation and writes the resulting endpoint.
 * @param body `endpoint=<n>` plus any of `power`, `bri`, `hue`, `sat`, `ct`, `rgb`, `channels`.
 */
[[nodiscard]] Result apply(LightService* light, std::string_view body, CommandOutput& output);

} // namespace light_web
} // namespace newmota
