/*
  berry_module_table.c - plain Berry module registry for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "berry.h"
#include "newmota_features.h"

be_extern_native_module(string);
be_extern_native_module(json);
be_extern_native_module(math);
be_extern_native_module(time);
be_extern_native_module(os);
be_extern_native_module(global);
be_extern_native_module(sys);
be_extern_native_module(debug);
be_extern_native_module(gc);
be_extern_native_module(solidify);
be_extern_native_module(introspect);
be_extern_native_module(strict);
be_extern_native_module(undefined);
be_extern_native_module(preproc);
be_extern_native_module(newmota);
be_extern_native_module(path);
#if defined(USE_BERRY_PERSIST) && USE_BERRY_PERSIST
be_extern_native_module(persist);
#endif
#if USE_BERRY_GPIO
be_extern_native_module(gpio);
#endif
#if USE_BERRY_ULP
be_extern_native_module(ULP);
#endif
#if defined(USE_LVGL) && USE_LVGL
be_extern_native_module(lv);
be_extern_native_module(lv_extra);
#endif
#if (defined(USE_LVGL) && USE_LVGL) || USE_BLE
be_extern_native_module(cb);
#endif
#if USE_BLE
be_extern_native_module(BLE);
#endif
#if USE_MQTT
be_extern_native_module(mqtt);
#endif
#if USE_WEB
be_extern_native_module(widget);
#endif
#if USE_BERRY_CRYPTO
be_extern_native_module(crypto);
#endif
#if USE_PWM || USE_WS2812
be_extern_native_module(light);
#endif
#if USE_BERRY_AUDIO
be_extern_native_module(audio);
#endif
#if USE_MATTER
be_extern_native_module(matter);
#endif
#if USE_CAMERA
be_extern_native_module(cam);
#endif

BERRY_LOCAL const bntvmodule_t* const be_module_table[] = {
#if BE_USE_STRING_MODULE
    &be_native_module(string),
#endif
#if BE_USE_JSON_MODULE
    &be_native_module(json),
#endif
#if BE_USE_MATH_MODULE
    &be_native_module(math),
#endif
#if BE_USE_TIME_MODULE
    &be_native_module(time),
#endif
#if BE_USE_OS_MODULE
    &be_native_module(os),
#endif
#if BE_USE_GLOBAL_MODULE
    &be_native_module(global),
#endif
#if BE_USE_SYS_MODULE
    &be_native_module(sys),
#endif
#if BE_USE_DEBUG_MODULE
    &be_native_module(debug),
#endif
#if BE_USE_GC_MODULE
    &be_native_module(gc),
#endif
#if BE_USE_SOLIDIFY_MODULE
    &be_native_module(solidify),
#endif
#if BE_USE_INTROSPECT_MODULE
    &be_native_module(introspect),
#endif
#if BE_USE_STRICT_MODULE
    &be_native_module(strict),
#endif
    &be_native_module(undefined),
#if BE_USE_PREPROCESSOR
    &be_native_module(preproc),
#endif
    &be_native_module(newmota),
    &be_native_module(path),
#if defined(USE_BERRY_PERSIST) && USE_BERRY_PERSIST
    &be_native_module(persist),
#endif
#if USE_BERRY_GPIO
    &be_native_module(gpio),
#endif
#if USE_BERRY_ULP
    &be_native_module(ULP),
#endif
#if defined(USE_LVGL) && USE_LVGL
    &be_native_module(lv),
    &be_native_module(lv_extra),
#endif
#if (defined(USE_LVGL) && USE_LVGL) || USE_BLE
    &be_native_module(cb),
#endif
#if USE_BLE
    &be_native_module(BLE),
#endif
#if USE_MQTT
    &be_native_module(mqtt),
#endif
#if USE_WEB
    &be_native_module(widget),
#endif
#if USE_BERRY_CRYPTO
    &be_native_module(crypto),
#endif
#if USE_PWM || USE_WS2812
    &be_native_module(light),
#endif
#if USE_BERRY_AUDIO
    &be_native_module(audio),
#endif
#if USE_CAMERA
    &be_native_module(cam),
#endif
#if USE_MATTER
    &be_native_module(matter),
#endif
    NULL,
};

BERRY_LOCAL bclass_array be_class_table = {NULL};

/** @brief Leaves the initial plain Berry runtime without Newmota modules. */
BERRY_API void be_load_custom_libs(bvm* vm) {
    (void)vm;
}
