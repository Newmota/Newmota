/*
  adc.h - Configured ADC conversion contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc.h
 * @brief Configured ADC conversion contract.
 */

#pragma once
#include "newmota/platform/gpio.h"
#include <cstdint>

namespace newmota::platform::adc {
/** @brief Cleanup failure retains the pin reservation; busy never blocks the application. */
enum class Result : std::uint8_t { success, busy, unsupported, invalid_argument, failed };
/** @brief Reference provenance, not an accuracy or board-calibration guarantee. */
enum class Reference : std::uint8_t { nominal, sdk_calibrated };

/** @brief One native single-ended conversion in millivolts at the physical ADC pin. */
struct Reading {
    std::uint32_t millivolts{}, raw{};
    Reference reference{Reference::nominal};
};

/** @brief Claims/configures a native ADC route after the common owner reserves its pad.
 * @param pin Physical input, never an internal temperature/battery channel.
 * @return unsupported/invalid_argument/busy leave native resources untouched;
 * failed may retain partial setup and requires confirmed close before releasing a claim.
 */
Result open(PinId pin);
/** @brief Starts one conversion on the exclusively scheduled ADC controller.
 * @param pin An open ADC pad.
 * @return success when queued, busy for controller contention, or a route/conversion error.
 */
Result start(PinId pin);
/** @brief Polls the pending conversion without waiting for hardware readiness.
 * @param reading Receives raw and reference-converted millivolts on success.
 * @return busy until ready; success consumes the conversion exactly once.
 */
Result finish(Reading& reading);
/** @brief Cancels an outstanding conversion before another channel can run.
 * @return success only when no callback/conversion can still use its previous state.
 */
Result cancel();
/** @brief Releases a native route, restoring the pad to a neutral input.
 * @param pin Previously opened ADC pad.
 * @return success only after native cleanup is confirmed.
 */
Result close(PinId pin);
} // namespace newmota::platform::adc
