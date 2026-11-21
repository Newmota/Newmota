/*
  pixmat_module.h - Berry pixel matrix module interface

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

/** @file pixmat_module.h
 *  @brief Declares installation of the Berry pixel matrix class.
 */

#include <cstddef>
#include <cstdint>

struct bvm;

namespace newmota {

/** @brief A matrix over its current Berry buffer.
 *
 *  bpp 0 stores one bit per pixel in the layout of monochrome panels: each byte is a column of
 *  eight rows with the top row in bit 0, and pages of `width` bytes follow each other.
 */
struct PixmatBuffer {
    std::uint8_t* data{};
    int width{}, height{}, bpp{};
    bool serpentine{};

    /** @brief Returns the number of bytes occupied by this matrix. */
    std::size_t size() const {
        const auto wide = static_cast<std::size_t>(width);
        return bpp ? wide * height * bpp : wide * (height / 8 + (height % 8 ? 1 : 0));
    }

    /** @brief Resolves a logical pixel of a byte format to physical storage. */
    std::uint8_t* pixel(int x, int y) const {
        if (serpentine && (y & 1))
            x = width - 1 - x;
        return data + (static_cast<std::size_t>(y) * width + x) * bpp;
    }

    /** @brief Reads a pixel of the 1-bit format. */
    bool bit(int x, int y) const {
        return data[static_cast<std::size_t>(y / 8) * width + x] & (1U << (y & 7));
    }

    /** @brief Writes a pixel of the 1-bit format. */
    void put_bit(int x, int y, bool on) const {
        auto& byte = data[static_cast<std::size_t>(y / 8) * width + x];
        const auto mask = static_cast<std::uint8_t>(1U << (y & 7));
        byte = static_cast<std::uint8_t>(on ? byte | mask : byte & ~mask);
    }
};

/** @brief Resolves a Berry \c pixmat argument, raising a Berry error if invalid.
 *
 *  The address stays valid until Berry code runs or allocates again.
 *
 *  @param vm Berry virtual machine holding the value.
 *  @param index Stack index of the pixmat instance.
 *  @param write True when the caller changes pixels, which read-only buffers refuse.
 *  @return The matrix layout and its buffer address.
 */
PixmatBuffer pixmat_buffer(bvm* vm, int index, bool write = false);

/** @brief Draws an MSB-first packed glyph with clipping.
 *
 *  @param target Matrix receiving the glyph.
 *  @param bits Glyph rows, @p stride bytes each.
 *  @param width Glyph width in pixels.
 *  @param height Glyph height in pixels.
 *  @param stride Bytes per glyph row.
 *  @param x Left edge in the matrix.
 *  @param y Top edge in the matrix.
 *  @param color Packed color of set bits.
 *  @param background Packed color of clear bits, or nullptr to leave those pixels unchanged.
 */
void pixmat_draw_glyph(const PixmatBuffer& target, const std::uint8_t* bits, int width, int height,
    int stride, std::int64_t x, std::int64_t y, std::uint32_t color,
    const std::uint32_t* background);

/** @brief Installs the global Berry \c pixmat class.
 *
 *  @param vm Berry virtual machine receiving the class.
 *  @return True when the class has been registered.
 */
bool install_pixmat_module(bvm* vm);
} // namespace newmota
