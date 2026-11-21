/*
  ulp_module.cpp - Tasmota's Berry ULP module for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Adapted from Tasmota be_ULP_lib.c and xdrv_52_3_berry_ulp.ino,
  Copyright (C) 2021 Stephan Hadinger & Christian Baars, GPL version 3 or later.
*/

#include "services/berry/modules/ulp/ulp_module.h"

#include "core/system/restart.h"
#include "newmota/log.h"
#include "newmota/platform/ulp.h"

#include <cstdint>
#include <cstdio>

extern "C" {
#include "berry.h"
}

namespace {
namespace ulp = newmota::platform::ulp;

newmota::RestartController* restarter = nullptr;

/** @brief Reads integer argument @p index, raising Tasmota's ctype type_error otherwise. */
std::int32_t integer(bvm* vm, const int index) {
    if (index > be_top(vm) || !be_isint(vm, index))
        be_raise(vm, "type_error", "expected int");
    return static_cast<std::int32_t>(be_toint(vm, index));
}

/** @brief An optional integer argument, 0 when absent. */
std::int32_t optional_integer(bvm* vm, const int index) {
    return index > be_top(vm) ? 0 : integer(vm, index);
}

/** @brief A word index inside the program memory. */
std::size_t position(bvm* vm, const int index) {
    const auto value = integer(vm, index);
    if (value < 0 || static_cast<std::size_t>(value) >= ulp::memory_words())
        be_raise(vm, "index_error", "ULP: position outside the reserved memory");
    return static_cast<std::size_t>(value);
}

/** @brief `ULP.run([entry:int]) -> nil`; the FSM program starts with a jump at @p entry. */
int Run(bvm* vm) {
    ulp::run(optional_integer(vm, 1));
    be_return_nil(vm);
}

/** @brief `ULP.wake_period(period_index:int, period_us:int) -> nil` */
int WakePeriod(bvm* vm) {
    ulp::set_wakeup_period(integer(vm, 1), integer(vm, 2));
    be_return_nil(vm);
}

/** @brief `ULP.set_mem(position:int, value:int) -> value:int` */
int SetMem(bvm* vm) {
    const auto at = position(vm, 1);
    const auto value = integer(vm, 2);
    ulp::set_memory(at, value);
    be_pushint(vm, value);
    be_return(vm);
}

/** @brief `ULP.get_mem(position:int) -> int` */
int GetMem(bvm* vm) {
    be_pushint(vm, ulp::memory(position(vm, 1)));
    be_return(vm);
}

/** @brief `ULP.gpio_init(pin:int, mode:int) -> rtc_pin:int`, -1 for a pin without RTC IO. */
int GpioInit(bvm* vm) {
    be_pushint(vm, ulp::gpio_init(integer(vm, 1), integer(vm, 2)));
    be_return(vm);
}

/** @brief `ULP.adc_config(channel:int, attenuation:int, width:int) -> error:int` */
int AdcConfig(bvm* vm) {
    bool supported = true;
    const auto error = ulp::adc_config(integer(vm, 1), integer(vm, 2), integer(vm, 3), supported);
    if (!supported)
        be_raise(vm, "ulp_adc_config_error", "ULP: this chip has no ADC the LP core can use");
    be_pushint(vm, error);
    be_return(vm);
}

/** @brief `ULP.sleep([wake time in seconds:int]) -> nil`; does not return. */
int Sleep(bvm* vm) {
    const auto seconds = static_cast<std::uint32_t>(optional_integer(vm, 1));
    if (restarter == nullptr)
        be_raise(vm, "runtime_error", "ULP: sleep unavailable");
    AddLog(newmota::LogLevel::info, "ULP: Enter sleep mode.");
    if (seconds != 0U)
        AddLog(newmota::LogLevel::info, "ULP: will wake up in %lu seconds.",
            static_cast<unsigned long>(seconds));
    restarter->deep_sleep_now(static_cast<std::uint64_t>(seconds) * 1000000U, true);
}

/** @brief `ULP.load(code:bytes) -> nil`, raising ulp_load_error for an invalid program. */
int Load(bvm* vm) {
    if (be_top(vm) < 1 || !be_isbytes(vm, 1))
        be_raise(vm, "type_error", "expected bytes");
    std::size_t size = 0U;
    const auto* image = static_cast<const std::uint8_t*>(be_tobytes(vm, 1, &size));
    const auto error = ulp::load(image, size);
    if (error != 0) {
        char message[40];
        (void)std::snprintf(
            message, sizeof message, "ULP: invalid code err=%li", static_cast<long>(error));
        be_raise(vm, "ulp_load_error", message);
    }
    be_return_nil(vm);
}

} // namespace

namespace newmota {

void install_ulp_module(RestartController& restart) {
    restarter = &restart;
}

} // namespace newmota

extern "C" {
be_native_module_attr_table(ULP){be_native_module_function("run", Run),
    be_native_module_function("load", Load), be_native_module_function("set_mem", SetMem),
    be_native_module_function("get_mem", GetMem), be_native_module_function("gpio_init", GpioInit),
    be_native_module_function("wake_period", WakePeriod), be_native_module_function("sleep", Sleep),
    be_native_module_function("adc_config", AdcConfig)};
extern const bntvmodule_t be_native_module_ULP = {
    "ULP", ULP_attrs, sizeof(ULP_attrs) / sizeof(ULP_attrs[0]), nullptr};
}
