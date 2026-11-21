/*
  ulp.cpp - ESP-IDF ULP-FSM, ULP-RISC-V and LP core control

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Adapted from Tasmota xdrv_52_3_berry_ulp.ino,
  Copyright (C) 2021 Stephan Hadinger & Christian Baars, GPL version 3 or later.
*/

/** @file ulp.cpp
 * @brief The coprocessor type is the one the build selected for the chip.
 *
 * Only the ESP32-P4 has an ADC the LP core can reach; on the other LP-core chips the ADC sits
 * in the powered-down domain.
 */

#include "newmota/platform/ulp.h"

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "soc/soc_caps.h"
#include "ulp_common.h"

#if CONFIG_ULP_COPROC_TYPE_FSM
#include "esp32/ulp.h"
#include "ulp_adc.h"
#elif CONFIG_ULP_COPROC_TYPE_RISCV
#include "ulp_adc.h"
#include "ulp_riscv.h"
#elif CONFIG_ULP_COPROC_TYPE_LP_CORE
#include "ulp_lp_core.h"
#if SOC_LP_ADC_SUPPORTED
#include "ulp_lp_core_lp_adc_shared.h"
#endif
#endif

#include <cstddef>
#include <cstdint>

namespace newmota::platform::ulp {
namespace {

#if CONFIG_ULP_COPROC_TYPE_LP_CORE
ulp_lp_core_cfg_t lp_core_config{};
#endif

} // namespace

void run(const std::int32_t entry) {
#if CONFIG_ULP_COPROC_TYPE_FSM
    (void)ulp_run(static_cast<std::uint32_t>(entry)); // The entry should start the program
#elif CONFIG_ULP_COPROC_TYPE_RISCV
    (void)entry;
    (void)ulp_riscv_run();
#else
    (void)entry;
    (void)ulp_lp_core_run(&lp_core_config);
#endif
}

void set_wakeup_period(const std::int32_t index, const std::int32_t microseconds) {
#if CONFIG_ULP_COPROC_TYPE_LP_CORE
    (void)index;
    lp_core_config.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER;
    lp_core_config.lp_timer_sleep_duration_us = static_cast<std::uint32_t>(microseconds);
#else
    (void)ulp_set_wakeup_period(
        static_cast<std::size_t>(index), static_cast<std::uint32_t>(microseconds));
#endif
}

std::size_t memory_words() {
    return CONFIG_ULP_COPROC_RESERVE_MEM / sizeof(std::uint32_t);
}

void set_memory(const std::size_t position, const std::int32_t value) {
    RTC_SLOW_MEM[position] = static_cast<std::uint32_t>(value);
}

std::int32_t memory(const std::size_t position) {
#if CONFIG_ULP_COPROC_TYPE_FSM
    return static_cast<std::int32_t>(RTC_SLOW_MEM[position] & 0xffffU); // Only low 16 bits used
#else
    return static_cast<std::int32_t>(RTC_SLOW_MEM[position]); // Full 32 bits for RISC-V
#endif
}

std::int32_t gpio_init(const std::int32_t pin, const std::int32_t mode) {
    const auto gpio = static_cast<gpio_num_t>(pin);
    if (!rtc_gpio_is_valid_gpio(gpio))
        return -1;
    (void)rtc_gpio_init(gpio);
    (void)rtc_gpio_set_direction(gpio, static_cast<rtc_gpio_mode_t>(mode));
    return rtc_io_number_get(gpio);
}

std::int32_t adc_config(const std::int32_t channel, const std::int32_t attenuation,
    const std::int32_t width, bool& supported) {
    supported = true;
#if CONFIG_ULP_COPROC_TYPE_FSM || CONFIG_ULP_COPROC_TYPE_RISCV
    ulp_adc_cfg_t config{};
    config.adc_n = ADC_UNIT_1;
    config.channel = static_cast<adc_channel_t>(channel);
    config.atten = static_cast<adc_atten_t>(attenuation);
    config.width = static_cast<adc_bitwidth_t>(width);
#if CONFIG_ULP_COPROC_TYPE_FSM
    config.ulp_mode = ADC_ULP_MODE_FSM;
#else
    config.ulp_mode = ADC_ULP_MODE_RISCV;
#endif
    return ulp_adc_init(&config);
#elif SOC_LP_ADC_SUPPORTED
    const esp_err_t initialized = lp_core_lp_adc_init(ADC_UNIT_1);
    if (initialized != ESP_OK)
        return initialized;
    lp_core_lp_adc_chan_cfg_t config{};
    config.atten = static_cast<adc_atten_t>(attenuation);
    config.bitwidth = static_cast<adc_bitwidth_t>(width);
    return lp_core_lp_adc_config_channel(ADC_UNIT_1, static_cast<adc_channel_t>(channel), &config);
#else
    (void)channel;
    (void)attenuation;
    (void)width;
    supported = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

std::int32_t load(const std::uint8_t* const image, const std::size_t size) {
#if CONFIG_ULP_COPROC_TYPE_FSM
    // FSM image with its header; the size is in 32-bit words.
    return ulp_load_binary(0U, image, size / sizeof(std::uint32_t));
#elif CONFIG_ULP_COPROC_TYPE_RISCV
    return ulp_riscv_load_binary(image, size); // Raw binary without a header
#else
    return ulp_lp_core_load_binary(image, size);
#endif
}

} // namespace newmota::platform::ulp
