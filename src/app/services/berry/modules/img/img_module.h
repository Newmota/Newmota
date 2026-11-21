/*
  img_module.h - Berry img class for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file img_module.h
 * @brief Declares the global Berry `img` class and the native calls `cam.get_image` uses.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct bvm;

namespace newmota {
/** @brief Registers the global Berry `img` class. */
bool install_img_module(bvm* vm);

namespace img {
/** @brief Pixel formats with Tasmota's `img` constant values. */
enum class Format : std::uint8_t {
    rgb565 = 0,   ///< Big-endian RGB565.
    rgb565le = 1, ///< Little-endian RGB565, the ESP display framebuffer order.
    grayscale = 3,
    jpeg = 4,
    rgb888 = 5, ///< R, G, B byte order.
};

/** @brief The native image an `img` instance owns. */
struct Image;

/** @brief Outcome of storing or decoding into an image. */
enum class Result : std::uint8_t { ok, no_memory, invalid_jpeg, unsupported_jpeg, timeout };

/** @brief Returns the image of the `img` instance at @p index; raises when it is none. */
Image& instance(bvm* vm, int index);

/** @brief Reads a format constant at @p index; raises for an unknown value. */
Format format(bvm* vm, int index);

/** @brief Copies a JPEG into the image as it is, reading its size from the frame header. */
[[nodiscard]] Result store_jpeg(Image& image, const std::uint8_t* data, std::size_t size);

/**
 * @brief Decodes a baseline JPEG into the image as @p format, a raw format.
 *
 * The buffer is kept when the decoded frame fits. A failure while decoding leaves the image
 * at the new size and format with partial pixels; a header failure leaves it unchanged.
 */
[[nodiscard]] Result decode_jpeg(
    Image& image, const std::uint8_t* data, std::size_t size, Format format);

/** @brief Raises the Berry error for a failed result; returns for Result::ok. */
void raise(bvm* vm, Result result);
} // namespace img
} // namespace newmota
