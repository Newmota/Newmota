/*
  display.h - descriptor driven display boundary

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display.h
 * @brief Retained display owner; calls are serialized on the application task.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::display {
enum class Result : std::uint8_t {
    success,
    invalid_descriptor,
    unsupported,
    busy,
    no_memory,
    failed,
    timeout
};

struct Info {
    char name[32]{};
    std::uint16_t width{}, height{}, flush_lines{40};
    std::uint8_t bpp{}, flags{};
    bool buffered{};
    PinId pins[40]{};
    std::size_t pin_count{};
    // A configured shared I2C bus is borrowed; its pads are absent from pins.
    std::int8_t shared_i2c_bus{-1};
    // Optional controller lease is acquired by the application before open.
    std::int8_t touch_i2c_bus{-1};
    std::uint8_t touch_i2c_address{};
};

/** @brief Last successfully accepted controls and current initialization readiness.
 * Values are retained intent; they do not read back physical panel registers.
 */
struct State {
    bool ready{}, powered{}, inverted{};
    std::uint8_t brightness{}, rotation{};
};

/** @brief Frame memory of a monochrome panel, owned by the display until restart. */
struct Framebuffer {
    std::uint8_t* data{};
    std::size_t size{};
};

/** @brief Receives new logical dimensions synchronously on the application task. */
using GeometryCallback = void (*)(const Info&);

/** @brief Parses and validates the entire descriptor without changing hardware.
 * Descriptor text and diagnostic storage are borrowed only for this call.
 * @param error Optional bounded diagnostic destination.
 * @return success when syntax and target capabilities are supported.
 */
Result describe(
    const char* descriptor, std::size_t length, Info&, char* error, std::size_t error_capacity);
/** @brief Opens one display after the caller reserves Info::pins.
 * The owner and its hardware are retained until restart, including on hardware
 * failure. Descriptors never execute hardware operations during parsing.
 * Descriptor text is copied into owned configuration and may be released after return.
 */
Result open(
    const char* descriptor, std::size_t length, Info&, char* error, std::size_t error_capacity);
/** @brief Returns current logical dimensions, or nullptr before successful open. */
const Info* info();
/** @brief Copies control state, returning false before successful display open. */
bool state(State&);
/** @brief Sets the single UI geometry observer; nullptr detaches before UI teardown. */
void set_geometry_callback(GeometryCallback callback);
/** @brief Copies a complete in-bounds RGB565 rectangle before returning.
 * @param count Must equal width times height; colors use host byte order.
 * @return success once the source can safely be reused.
 */
Result write(std::uint16_t x, std::uint16_t y, std::uint16_t width, std::uint16_t height,
    const std::uint16_t* rgb565, std::size_t count);
/** @brief Shares a monochrome panel's frame memory at rotation 0.
 * Each byte is a column of eight rows, top row in bit 0, in pages of panel width; a set bit is
 * white. After this call every refresh() publishes the memory, since writes through it are not
 * seen. The layout stays physical if the panel is rotated later.
 * @return false when the panel keeps no such memory or is rotated.
 */
bool framebuffer(Framebuffer&);
/** @brief Advances EPD initialization, delays, BUSY and queued refresh on the app loop.
 * @return success while idle or progressing; a terminal fault remains retained.
 */
Result poll();
/** @brief Reports whether panel initialization has completed successfully.
 * Buffered writes are accepted during EPD initialization.
 */
bool ready();
/** @brief Publishes retained pixels; EPD requests are coalesced and run by poll().
 * An EPD panel lacking a partial sequence uses its full refresh sequence.
 */
Result refresh(bool partial = false);
/** @brief Applies descriptor display power command, dimmer and backlight state.
 * @return busy while EPD initialization or refresh owns its command stream;
 * unsupported when this direction has no effective command/dimmer/backlight path.
 */
Result power(bool on);
/** @brief Applies descriptor color inversion command or buffered EPD inversion. */
Result invert(bool enabled);
/** @brief Selects rotation 0..3 and updates logical dimensions. */
Result rotation(std::uint8_t value);
/** @brief Sets 0..255 brightness through descriptor dim command or backlight PWM.
 * @return busy while EPD initialization or refresh owns its command stream.
 */
Result brightness(std::uint8_t value);
} // namespace newmota::platform::display
