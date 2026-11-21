/*
  lvgl_runtime.cpp - cooperative LVGL lifecycle for Berry

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file lvgl_runtime.cpp
 * @brief Owns lazy LVGL startup, font lookup and protected cooperative dispatch.
 */

#include "newmota_features.h"
#include "services/berry/modules/lvgl/lvgl_runtime.h"

#include "core/system/boot_guard.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#if USE_DISPLAY
#include "newmota/platform/display.h"
#if USE_UNIVERSAL_TOUCH
#include "newmota/platform/display/touch.h"
#endif
#endif

extern "C" {
#include "be_exec.h"
#include "be_mapping.h"
#include "berry.h"
#include "lv_berry.h"
#include "lvgl.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <limits>

#include "lvgl_widgets.h"

extern "C" const lv_font_t lv_font_montserrat_tasmota_10;
extern "C" const lv_font_t lv_font_montserrat_tasmota_14;
extern "C" const lv_font_t lv_font_montserrat_tasmota_20;
extern "C" const lv_font_t lv_font_montserrat_tasmota_28;
extern "C" const lv_font_t seg7_8;
extern "C" const lv_font_t seg7_10;
extern "C" const lv_font_t seg7_12;
extern "C" const lv_font_t seg7_14;
extern "C" const lv_font_t seg7_16;
extern "C" const lv_font_t seg7_18;
extern "C" const lv_font_t seg7_20;
extern "C" const lv_font_t seg7_24;
extern "C" const lv_font_t seg7_28;
extern "C" const lv_font_t seg7_36;
extern "C" const lv_font_t seg7_48;
extern "C" const lv_font_t lv_font_icons_12;
extern "C" const lv_font_t lv_font_icons_16;
extern "C" const lv_font_t lv_font_icons_18;
extern "C" const lv_font_t lv_font_icons_22;
extern "C" const lv_font_t lv_font_icons_24;

namespace newmota {
namespace {

static_assert(sizeof(void*) == 4 && sizeof(std::size_t) == 4,
    "The imported LVGL Berry ctypes tables require a 32-bit MCU ABI");
static_assert(sizeof(bint) == 4 && sizeof(breal) == 4,
    "The imported native call mapper requires 32-bit Berry values");

/** @brief Associates one Tasmota-compatible family and nominal size with a flash font. */
struct FontEntry {
    const char* family;
    bint size;
    const lv_font_t* font;
};

constexpr FontEntry fonts[] = {
    {"montserrat", 10, &lv_font_montserrat_tasmota_10},
    {"montserrat", 14, &lv_font_montserrat_tasmota_14},
    {"montserrat", 20, &lv_font_montserrat_tasmota_20},
    {"montserrat", 28, &lv_font_montserrat_tasmota_28},
    {"seg7", 8, &seg7_8},
    {"seg7", 10, &seg7_10},
    {"seg7", 12, &seg7_12},
    {"seg7", 14, &seg7_14},
    {"seg7", 16, &seg7_16},
    {"seg7", 18, &seg7_18},
    {"seg7", 20, &seg7_20},
    {"seg7", 24, &seg7_24},
    {"seg7", 28, &seg7_28},
    {"seg7", 36, &seg7_36},
    {"seg7", 48, &seg7_48},
    {"icons", 10, &lv_font_montserrat_tasmota_10},
    {"icons", 12, &lv_font_icons_12},
    {"icons", 14, &lv_font_montserrat_tasmota_14},
    {"icons", 16, &lv_font_icons_16},
    {"icons", 18, &lv_font_icons_18},
    {"icons", 20, &lv_font_montserrat_tasmota_20},
    {"icons", 22, &lv_font_icons_22},
    {"icons", 24, &lv_font_icons_24},
    {"icons", 28, &lv_font_montserrat_tasmota_28},
    {"unscii", 8, &lv_font_unscii_8},
    {"unscii", 16, &lv_font_unscii_16},
};

/** @brief Wraps the exact embedded font; unsupported families and sizes raise value_error. */
int Font(bvm* vm, const char* family, const bint size) {
    for (const auto& entry : fonts) {
        if (entry.size == size && std::strcmp(entry.family, family) == 0) {
            be_find_global_or_module_member(vm, "lv.lv_font");
            be_pushcomptr(vm, const_cast<lv_font_t*>(entry.font));
            be_call(vm, 1);
            be_pop(vm, 1);
            be_return(vm);
        }
    }
    be_raisef(vm, "value_error", "unknown font '%s-%i'", family, size);
    be_return_nil(vm);
}

/** @brief Implements Tasmota's generic family/size lookup and invalid-argument nil result. */
int EmbeddedFont(bvm* vm) {
    if (be_top(vm) >= 2 && be_isstring(vm, 1) && be_isint(vm, 2))
        return Font(vm, be_tostring(vm, 1), be_toint(vm, 2));
    be_return_nil(vm);
}

/** @brief Implements the Montserrat size shortcut. */
int MontserratFont(bvm* vm) {
    if (!be_isint(vm, 1) || be_toint(vm, 1) == 0)
        be_raise(vm, "value_error", "font size required");
    return Font(vm, "montserrat", be_toint(vm, 1));
}

/** @brief Implements the DSEG7 size shortcut. */
int Seg7Font(bvm* vm) {
    if (!be_isint(vm, 1) || be_toint(vm, 1) == 0)
        be_raise(vm, "value_error", "font size required");
    return Font(vm, "seg7", be_toint(vm, 1));
}

/** @brief Adds compatibility names to the writable lv module at the stack top. */
void InstallFonts(bvm* vm) {
    struct Binding {
        const char* name;
        bntvfunc function;
    };

    constexpr Binding bindings[] = {{"font_embedded", EmbeddedFont},
        {"font_montserrat", MontserratFont}, {"montserrat_font", MontserratFont},
        {"font_seg7", Seg7Font}, {"seg7_font", Seg7Font}};
    for (const auto& binding : bindings) {
        be_pushntvfunction(vm, binding.function);
        be_setmember(vm, -2, binding.name);
        be_pop(vm, 1);
    }
}

bvm* owner = nullptr;
bool started{};
lv_obj_t* splash = nullptr;
lv_timer_t* splash_timer = nullptr;
#if USE_DISPLAY
lv_display_t* panel_display = nullptr;
#if USE_UNIVERSAL_TOUCH
lv_indev_t* panel_pointer = nullptr;

/** @brief Supplies one cached cooperative sample, including release after transfer failure. */
void ReadPointer(lv_indev_t*, lv_indev_data_t* data) {
    const auto sample = platform::display::touch_state();
    data->state = sample.available && sample.healthy && sample.pressed ? LV_INDEV_STATE_PRESSED
                                                                       : LV_INDEV_STATE_RELEASED;
    data->point.x = sample.x;
    data->point.y = sample.y;
    data->continue_reading = false;
}
#endif
void* panel_buffer = nullptr;
bool geometry_pending{};

/** @brief Defers geometry events until LVGL runs inside the Berry execution deadline. */
void PanelGeometryChanged(const platform::display::Info&) {
    geometry_pending = true;
}

/** @brief Forgets a panel explicitly deleted through the LVGL Berry bindings. */
void PanelDeleted(lv_event_t*) {
    platform::display::set_geometry_callback(nullptr);
#if USE_UNIVERSAL_TOUCH
    if (panel_pointer)
        lv_indev_delete(panel_pointer);
#endif
    panel_display = nullptr;
    geometry_pending = false;
}

/** @brief Writes one LVGL RGB565 tile and returns its buffer as soon as copied. */
void FlushDisplay(
    lv_display_t* const display, const lv_area_t* const area, std::uint8_t* const pixels) {
    const auto* const info = platform::display::info();
    if (info == nullptr || area == nullptr || pixels == nullptr) {
        AddLog(LogLevel::error, "LVG: display flush has no panel or pixel buffer");
        lv_display_flush_ready(display);
        return;
    }
    const auto width = static_cast<std::int64_t>(area->x2) - area->x1 + 1;
    const auto height = static_cast<std::int64_t>(area->y2) - area->y1 + 1;
    const auto right = static_cast<std::int64_t>(area->x2);
    const auto bottom = static_cast<std::int64_t>(area->y2);
    const auto count = width > 0 && height > 0
                           ? static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
                           : 0U;
    auto result = platform::display::Result::invalid_descriptor;
    if (area->x1 >= 0 && area->y1 >= 0 && right < info->width && bottom < info->height &&
        count <= std::numeric_limits<std::size_t>::max()) {
        result = platform::display::write(static_cast<std::uint16_t>(area->x1),
            static_cast<std::uint16_t>(area->y1), static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height), reinterpret_cast<const std::uint16_t*>(pixels),
            static_cast<std::size_t>(count));
    }
    if (result == platform::display::Result::success && lv_display_flush_is_last(display))
        result = platform::display::refresh(true);
    if (result != platform::display::Result::success)
        AddLog(
            LogLevel::error, "LVG: display flush failed (%u)", static_cast<unsigned int>(result));
    // platform::display::write is synchronous and either transmits or copies
    // the tile into retained panel storage before returning.
    lv_display_flush_ready(display);
}

/** @brief Creates an LVGL partial-buffer display for the initialized panel. */
bool InitializePanelDisplay() {
    const auto* const info = platform::display::info();
    if (info == nullptr)
        return true;
    if (info->width == 0U || info->height == 0U)
        return false;
    // A rotated panel must still fit at least one complete row in this retained buffer.
    const auto lines = info->flush_lines == 0U ? 1U : std::min(info->flush_lines, info->height);
    const auto pixels = std::max(
        static_cast<std::size_t>(info->width) * lines, static_cast<std::size_t>(info->height));
    if (pixels > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t) ||
        pixels * sizeof(std::uint16_t) > std::numeric_limits<std::uint32_t>::max())
        return false;
    panel_buffer = std::malloc(pixels * sizeof(std::uint16_t));
    if (panel_buffer == nullptr)
        return false;
    panel_display = lv_display_create(info->width, info->height);
    if (panel_display == nullptr) {
        std::free(panel_buffer);
        panel_buffer = nullptr;
        return false;
    }
    lv_display_set_color_format(panel_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(panel_display, FlushDisplay);
    lv_display_set_buffers(panel_display, panel_buffer, nullptr,
        static_cast<std::uint32_t>(pixels * sizeof(std::uint16_t)), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(panel_display);
    lv_display_add_event_cb(panel_display, PanelDeleted, LV_EVENT_DELETE, nullptr);
#if USE_UNIVERSAL_TOUCH
    if (platform::display::touch_state().available) {
        panel_pointer = lv_indev_create();
        if (!panel_pointer)
            return false;
        lv_indev_set_type(panel_pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(panel_pointer, panel_display);
        lv_indev_set_read_cb(panel_pointer, ReadPointer);
        lv_indev_add_event_cb(
            panel_pointer, [](lv_event_t*) { panel_pointer = nullptr; }, LV_EVENT_DELETE, nullptr);
    }
#endif
    return true;
}
#endif

/** @brief Supplies LVGL's wrapping millisecond clock without an IRQ hook. */
std::uint32_t Tick() {
    return platform::monotonic_milliseconds();
}

/** @brief Routes LVGL diagnostics through the shared application logger. */
void Log(const lv_log_level_t level, const char* message) {
    AddLog(level == LV_LOG_LEVEL_ERROR ? LogLevel::error : LogLevel::info, "LVG: %s", message);
}

/** @brief Deletes the transient splash without disturbing the application screen. */
void RemoveSplash() {
    if (splash_timer != nullptr) {
        lv_timer_delete(splash_timer);
        splash_timer = nullptr;
    }
    if (splash != nullptr) {
        lv_obj_delete(splash);
        splash = nullptr;
    }
}

/** @brief Expires the native splash inside the normal protected LVGL timer poll. */
void ExpireSplash(lv_timer_t*) {
    RemoveSplash();
}

/** @brief Releases the core and panel buffer, including partially initialized starts. */
void StopCore(bvm* vm) {
#if USE_DISPLAY
    platform::display::set_geometry_callback(nullptr);
    geometry_pending = false;
#endif
    be_cb_deinit(vm);
    // No user callbacks may run during teardown outside the Berry deadline.
    splash = nullptr;
    splash_timer = nullptr;
    lv_deinit();
#if USE_DISPLAY
    std::free(panel_buffer);
    panel_buffer = nullptr;
    panel_display = nullptr;
#if USE_UNIVERSAL_TOUCH
    panel_pointer = nullptr;
#endif
#endif
    started = false;
}

/** @brief Lazily starts the core, display buffer, callback roots and custom widgets. */
int Start(bvm* vm) {
    if (started) {
        RemoveSplash();
        be_return_nil(vm);
    }
    lv_init();
    lv_log_register_print_cb(Log);
    lv_tick_set_cb(Tick);
#if USE_DISPLAY
    if (!InitializePanelDisplay()) {
        StopCore(vm);
        be_raise(vm, "memory_error", "LVGL display allocation or setup failed");
    }
#endif
    constexpr char source[] = "import cb";
    int result = be_loadbuffer(vm, "lvgl_start", source, sizeof(source) - 1U);
    if (result == BE_OK)
        result = be_pcall(vm, 0);
    if (result == BE_OK) {
        be_pop(vm, 1);
        result = be_loadbuffer(
            vm, "lvgl_widgets", newmota_lvgl_widgets, sizeof(newmota_lvgl_widgets) - 1U);
        if (result == BE_OK)
            result = be_pcall(vm, 0);
    }
    if (result != BE_OK) {
        StopCore(vm);
        be_throw(vm, result);
    }
    be_pop(vm, 1);
    started = true;
#if USE_DISPLAY
    if (panel_display != nullptr)
        platform::display::set_geometry_callback(PanelGeometryChanged);
#endif
    be_return_nil(vm);
}

/** @brief Installs Newmota extensions on the binding's writable lv module. */
int InitializeBindings(bvm* vm) {
    be_load_lvgl_classes(vm);
    be_pushnil(vm);
    be_setglobal(vm, "_lvgl");
    be_pop(vm, 1);
    constexpr char source[] = "import lv return lv";
    int result = be_loadbuffer(vm, "lvgl_init", source, sizeof(source) - 1U);
    if (result == BE_OK)
        result = be_pcall(vm, 0);
    if (result != BE_OK)
        be_throw(vm, result);
    be_pushntvfunction(vm, Start);
    be_setmember(vm, -2, "start");
    be_pop(vm, 1);
    InstallFonts(vm);
    be_pop(vm, 1);
#if USE_DISPLAY
    if (platform::display::info() != nullptr) {
        constexpr char query[] = "return newmota.cmd('SetOption135')['SetOption135'] == 'OFF'";
        result = be_loadbuffer(vm, "lvgl_autostart", query, sizeof(query) - 1U);
        if (result == BE_OK)
            result = be_pcall(vm, 0);
        if (result != BE_OK)
            be_throw(vm, result);
        // A boot loop skips the autostart; an explicit lv.start() still opens the panel.
        const bool automatic = be_tobool(vm, -1) && !boot_guard().reduced();
        be_pop(vm, 1);
        if (automatic) {
            be_pushntvfunction(vm, Start);
            result = be_pcall(vm, 0);
            if (result != BE_OK)
                be_throw(vm, result);
            be_pop(vm, 1);
            if (panel_display != nullptr) {
                splash = lv_obj_create(lv_layer_top());
                lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
                lv_obj_set_style_bg_color(splash, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(splash, 0, 0);
                lv_obj_t* label = lv_label_create(splash);
                lv_label_set_text(label, "Newmota");
                lv_obj_set_style_text_color(label, lv_color_white(), 0);
                lv_obj_center(label);
                // Deleting the display or its top layer must also forget the overlay.
                lv_obj_add_event_cb(
                    splash, [](lv_event_t*) { splash = nullptr; }, LV_EVENT_DELETE, nullptr);
                splash_timer = lv_timer_create(ExpireSplash, 5000, nullptr);
                if (splash_timer == nullptr)
                    RemoveSplash();
            }
        }
    }
#endif
    be_return_nil(vm);
}

} // namespace

bool install_lvgl_runtime(bvm* vm) {
    if (owner != nullptr)
        return owner == vm;
    be_set_ctype_func_handler(vm, be_call_ctype_func);
    be_pushntvfunction(vm, InitializeBindings);
    const int result = be_pcall(vm, 0);
    if (result != BE_OK) {
        const char* message = be_isstring(vm, -1) ? be_tostring(vm, -1) : "binding setup failed";
        AddLog(LogLevel::error, "LVG: %s", message);
        if (started)
            StopCore(vm);
    }
    be_pop(vm, be_top(vm));
    if (result != BE_OK)
        return false;
    owner = vm;
    return true;
}

int poll_lvgl_runtime(bvm* vm) {
    if (owner == vm && started) {
#if USE_DISPLAY
        if (geometry_pending && panel_display != nullptr) {
            geometry_pending = false;
            const auto* const info = platform::display::info();
            if (info != nullptr)
                lv_display_set_resolution(panel_display, info->width, info->height);
        }
#endif
        (void)lv_timer_handler();
        // Keep deleting objects rooted until all native event callbacks have returned.
        be_getglobal(vm, "_lvgl");
        be_pushnil(vm);
        be_setmember(vm, -2, "deleted");
        be_pop(vm, 2);
    }
    be_return_nil(vm);
}

void remove_lvgl_runtime(bvm* vm) {
    if (owner != vm)
        return;
    if (started)
        StopCore(vm);
    owner = nullptr;
}

} // namespace newmota
