/*
  ulp.h - ULP coprocessor control for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ulp.h
 * @brief The ULP-FSM (ESP32), ULP-RISC-V (S2, S3) or LP core (C5, C6, P4, S31) behind Tasmota's
 * Berry ULP module. Values and error codes are ESP-IDF's.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform::ulp {

/** @brief Starts the loaded program; the FSM jumps to @p entry, the others start at 0. */
void run(std::int32_t entry);

/** @brief Sets the wake period; the LP core keeps one timer and ignores @p index. */
void set_wakeup_period(std::int32_t index, std::int32_t microseconds);

/** @brief Number of 32-bit words of RTC slow memory reserved for the program. */
[[nodiscard]] std::size_t memory_words();

/** @brief Writes one word of the program memory; @p position must be below memory_words(). */
void set_memory(std::size_t position, std::int32_t value);

/** @brief Reads one word; the FSM uses only the low 16 bits. */
[[nodiscard]] std::int32_t memory(std::size_t position);

/**
 * @brief Hands a pad to the RTC/LP domain with an rtc_gpio_mode_t direction.
 * @return The RTC/LP IO number, or -1 for a pad without one.
 */
[[nodiscard]] std::int32_t gpio_init(std::int32_t pin, std::int32_t mode);

/**
 * @brief Gives ADC1 @p channel to the coprocessor.
 * @param supported false where the coprocessor cannot reach an ADC.
 * @return An esp_err_t.
 */
[[nodiscard]] std::int32_t adc_config(
    std::int32_t channel, std::int32_t attenuation, std::int32_t width, bool& supported);

/** @brief Loads a program image; the FSM expects its header, the others a raw binary.
 * @return An esp_err_t.
 */
[[nodiscard]] std::int32_t load(const std::uint8_t* image, std::size_t size);

} // namespace newmota::platform::ulp
