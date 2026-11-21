/*
  display_module.cpp - Berry display primitive bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_module.cpp
 * @brief Exposes simple display primitives without a renderer dependency.
 */

#include "services/berry/modules/display/display_module.h"

#include "newmota/platform/display.h"
#include "newmota_features.h"
#if USE_UNIVERSAL_TOUCH
#include "newmota/platform/display/touch.h"
#endif
#if USE_BERRY_PIXMAT
#include "services/berry/modules/pixmat/pixmat_module.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "berry.h"
}

#if USE_DISPLAY_DRAW
extern "C" const bclass be_class_draw;
#endif

namespace {

inline constexpr std::size_t kFillChunkPixels = 256U;
std::array<std::uint16_t, kFillChunkPixels> fill_pixels{};

/** @brief Requires a loaded display before a Berry operation uses its dimensions. */
const newmota::platform::display::Info& info(bvm* const vm) {
    const auto* const current = newmota::platform::display::info();
    if (current == nullptr)
        be_raise(vm, "runtime_error", "display descriptor is not loaded");
    return *current;
}

/** @brief Parses one bounded Berry integer argument. */
std::int32_t integer(
    bvm* const vm, const int index, const std::int32_t minimum, const std::int32_t maximum) {
    if (!be_isint(vm, index))
        be_raise(vm, "type_error", "display expects integer arguments");
    const auto value = be_toint(vm, index);
    if (value < minimum || value > maximum)
        be_raise(vm, "value_error", "display argument out of range");
    return static_cast<std::int32_t>(value);
}

/** @brief Converts a 24-bit RGB value to the portable panel RGB565 format. */
std::uint16_t rgb565(const std::uint32_t color) {
    const auto red = static_cast<std::uint16_t>((color >> 19U) & 0x1fU);
    const auto green = static_cast<std::uint16_t>((color >> 10U) & 0x3fU);
    const auto blue = static_cast<std::uint16_t>((color >> 3U) & 0x1fU);
    return static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
}

/** @brief Maps retained display failures to Berry exceptions. */
void check(bvm* const vm, const newmota::platform::display::Result result) {
    using newmota::platform::display::Result;
    if (result == Result::success)
        return;
    if (result == Result::unsupported)
        be_raise(vm, "value_error", "display operation is unsupported by this panel");
    if (result == Result::busy)
        be_raise(vm, "runtime_error", "display is busy");
    if (result == Result::no_memory)
        be_raise(vm, "memory_error", "display allocation failed");
    if (result == Result::timeout)
        be_raise(vm, "timeout_error", "display operation timed out");
    be_raise(vm, "runtime_error", "display operation failed");
}

/** @brief Constructs a lightweight handle to the process-wide display owner. */
int Init(bvm* const vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "display() takes no arguments");
    (void)info(vm);
    be_return_nil(vm);
}

/** @brief Returns the logical panel width. */
int Width(bvm* const vm) {
    be_pushint(vm, static_cast<bint>(info(vm).width));
    be_return(vm);
}

/** @brief Returns the logical panel height. */
int Height(bvm* const vm) {
    be_pushint(vm, static_cast<bint>(info(vm).height));
    be_return(vm);
}

/** @brief Writes one clipped RGB pixel to the retained panel. */
int Pixel(bvm* const vm) {
    if (be_top(vm) != 4)
        be_raise(vm, "type_error", "pixel(x, y, rgb)");
    const auto& panel = info(vm);
    const auto x = integer(vm, 2, -32768, 32767);
    const auto y = integer(vm, 3, -32768, 32767);
    const auto color = static_cast<std::uint32_t>(integer(vm, 4, 0, 0x00ffffff));
    if (x >= 0 && y >= 0 && x < panel.width && y < panel.height) {
        const auto pixel = rgb565(color);
        check(vm, newmota::platform::display::write(static_cast<std::uint16_t>(x),
                      static_cast<std::uint16_t>(y), 1U, 1U, &pixel, 1U));
    }
    be_return_nil(vm);
}

/** @brief Fills a clipped rectangle in bounded horizontal chunks. */
int FillRect(bvm* const vm) {
    if (be_top(vm) != 6)
        be_raise(vm, "type_error", "fill_rect(x, y, width, height, rgb)");
    const auto& panel = info(vm);
    const auto x = integer(vm, 2, -32768, 32767);
    const auto y = integer(vm, 3, -32768, 32767);
    const auto width = integer(vm, 4, 0, 65535);
    const auto height = integer(vm, 5, 0, 65535);
    const auto color = static_cast<std::uint32_t>(integer(vm, 6, 0, 0x00ffffff));
    if (width == 0 || height == 0)
        be_return_nil(vm);

    const auto left = std::max<std::int64_t>(0, x);
    const auto top = std::max<std::int64_t>(0, y);
    const auto right = std::min<std::int64_t>(panel.width, static_cast<std::int64_t>(x) + width);
    const auto bottom = std::min<std::int64_t>(panel.height, static_cast<std::int64_t>(y) + height);
    if (left >= right || top >= bottom)
        be_return_nil(vm);

    const auto pixel = rgb565(color);
    for (std::size_t index = 0; index < fill_pixels.size(); ++index)
        fill_pixels[index] = pixel;
    for (auto row = top; row < bottom; ++row) {
        for (auto column = left; column < right;) {
            const auto count = std::min(kFillChunkPixels, static_cast<std::size_t>(right - column));
            check(vm, newmota::platform::display::write(static_cast<std::uint16_t>(column),
                          static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(count), 1U,
                          fill_pixels.data(), count));
            column += static_cast<std::int64_t>(count);
        }
    }
    be_return_nil(vm);
}

#if USE_BERRY_PIXMAT
/** @brief Converts one pixmat pixel to panel RGB565; two-channel pixels already are RGB565. */
std::uint16_t source_pixel(const newmota::PixmatBuffer& source, const int x, const int y) {
    if (!source.bpp)
        return source.bit(x, y) ? 0xffffU : 0U;
    const auto* const p = source.pixel(x, y);
    if (source.bpp == 2)
        return static_cast<std::uint16_t>((p[0] << 8U) | p[1]);
    const std::uint32_t red = p[0];
    const std::uint32_t green = source.bpp == 1 ? p[0] : p[1];
    const std::uint32_t blue = source.bpp == 1 ? p[0] : p[2];
    return rgb565((red << 16U) | (green << 8U) | blue);
}

/** @brief Copies a clipped pixmat to the panel in tiles of at most one pixel chunk. */
int Blit(bvm* const vm) {
    if (be_top(vm) != 4)
        be_raise(vm, "type_error", "blit(pixmat, x, y)");
    const auto& panel = info(vm);
    const auto x = integer(vm, 3, -32768, 32767);
    const auto y = integer(vm, 4, -32768, 32767);
    const auto source = newmota::pixmat_buffer(vm, 2);

    const auto left = std::max<std::int64_t>(0, x);
    const auto top = std::max<std::int64_t>(0, y);
    const auto right =
        std::min<std::int64_t>(panel.width, static_cast<std::int64_t>(x) + source.width);
    const auto bottom =
        std::min<std::int64_t>(panel.height, static_cast<std::int64_t>(y) + source.height);
    if (left >= right || top >= bottom)
        be_return_nil(vm);

    // Several rows per tile keep the address-window traffic of direct panels low.
    const auto tile_width = std::min(kFillChunkPixels, static_cast<std::size_t>(right - left));
    const auto tile_height = kFillChunkPixels / tile_width;
    for (auto row = top; row < bottom; row += static_cast<std::int64_t>(tile_height)) {
        const auto rows = std::min(tile_height, static_cast<std::size_t>(bottom - row));
        for (auto column = left; column < right; column += static_cast<std::int64_t>(tile_width)) {
            const auto columns = std::min(tile_width, static_cast<std::size_t>(right - column));
            std::size_t count{};
            for (std::size_t r = 0; r < rows; ++r)
                for (std::size_t c = 0; c < columns; ++c)
                    fill_pixels[count++] = source_pixel(
                        source, static_cast<int>(column + c - x), static_cast<int>(row + r - y));
            check(vm, newmota::platform::display::write(static_cast<std::uint16_t>(column),
                          static_cast<std::uint16_t>(row), static_cast<std::uint16_t>(columns),
                          static_cast<std::uint16_t>(rows), fill_pixels.data(), count));
        }
    }
    be_return_nil(vm);
}
#endif

/** @brief Returns bytes mapped onto a monochrome panel's frame memory, or nil without one. */
int Framebuffer(bvm* const vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "framebuffer() takes no arguments");
    (void)info(vm);
    newmota::platform::display::Framebuffer frame;
    if (!newmota::platform::display::framebuffer(frame))
        be_return_nil(vm);
    (void)be_getbuiltin(vm, "bytes");
    be_pushcomptr(vm, frame.data);
    be_pushint(vm, static_cast<bint>(frame.size));
    be_call(vm, 2);
    be_pop(vm, 2);
    be_return(vm);
}

/** @brief Requests a full EPD refresh or a coalesced partial refresh. */
int Refresh(bvm* const vm) {
    if (be_top(vm) > 2)
        be_raise(vm, "type_error", "refresh([partial])");
    bool partial = false;
    if (be_top(vm) == 2) {
        if (!be_isbool(vm, 2))
            be_raise(vm, "type_error", "refresh partial expects bool");
        partial = be_tobool(vm, 2);
    }
    (void)info(vm);
    check(vm, newmota::platform::display::refresh(partial));
    be_return_nil(vm);
}

/** @brief Changes panel power using the descriptor's configured controls. */
int Power(bvm* const vm) {
    if (be_top(vm) != 2 || !be_isbool(vm, 2))
        be_raise(vm, "type_error", "power(on) expects bool");
    (void)info(vm);
    check(vm, newmota::platform::display::power(be_tobool(vm, 2)));
    be_return_nil(vm);
}

/** @brief Changes panel color inversion. */
int Invert(bvm* const vm) {
    if (be_top(vm) != 2 || !be_isbool(vm, 2))
        be_raise(vm, "type_error", "invert(enabled) expects bool");
    (void)info(vm);
    check(vm, newmota::platform::display::invert(be_tobool(vm, 2)));
    be_return_nil(vm);
}

/** @brief Selects one of the four logical display rotations. */
int Rotation(bvm* const vm) {
    if (be_top(vm) != 2)
        be_raise(vm, "type_error", "rotation(value)");
    (void)info(vm);
    check(
        vm, newmota::platform::display::rotation(static_cast<std::uint8_t>(integer(vm, 2, 0, 3))));
    be_return_nil(vm);
}

/** @brief Sets panel brightness on its retained backlight output. */
int Brightness(bvm* const vm) {
    if (be_top(vm) != 2)
        be_raise(vm, "type_error", "brightness(value)");
    (void)info(vm);
    check(vm,
        newmota::platform::display::brightness(static_cast<std::uint8_t>(integer(vm, 2, 0, 255))));
    be_return_nil(vm);
}

#if USE_UNIVERSAL_TOUCH
/** @brief Returns cached logical [x,y] while pressed, or nil without touching the bus. */
int Touch(bvm* const vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "touch() takes no arguments");
    (void)info(vm);
    const auto sample = newmota::platform::display::touch_state();
    if (!sample.available || !sample.healthy || !sample.pressed)
        be_return_nil(vm);
    be_newobject(vm, "list");
    for (auto coordinate : {sample.x, sample.y}) {
        be_pushint(vm, coordinate);
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    be_pop(vm, 1);
    be_return(vm);
}
#endif

const bnfuncinfo methods[] = {{".init", Init}, {"width", Width}, {"height", Height},
    {"pixel", Pixel}, {"fill_rect", FillRect}, {"framebuffer", Framebuffer}, {"refresh", Refresh},
    {"power", Power}, {"invert", Invert}, {"rotation", Rotation}, {"brightness", Brightness},
#if USE_BERRY_PIXMAT
    {"blit", Blit},
#endif
#if USE_UNIVERSAL_TOUCH
    {"touch", Touch},
#endif
    {nullptr, nullptr}};

} // namespace

namespace newmota {

bool install_display_module(bvm* const vm) {
    be_pushclass(vm, "display", methods);
    be_setglobal(vm, "display");
    be_pop(vm, 1);
#if USE_DISPLAY_DRAW
    be_pushntvclass(vm, &be_class_draw);
    be_setglobal(vm, "draw");
    be_pop(vm, 1);
#endif
    return true;
}

} // namespace newmota
