/*
  led_strip_encoding.cpp - Allocation-free LED strip wire encoding

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip_encoding.cpp
 * @brief Allocation-free LED strip wire encoding.
 */

#include "newmota/platform/led_strip.h"
#include <cstring>

namespace newmota::platform::led_strip {
Result encode(const Pixel* pixels, std::size_t count, Order order, bool reverse,
    std::uint8_t* output, std::size_t capacity, bool rgbw, bool white_first) {
    if (!pixels || !output || !count || count > kMaximumPixels ||
        unsigned(order) > unsigned(Order::bgr) || capacity < wire_size(count, rgbw))
        return Result::invalid_argument;
    constexpr unsigned channels[6][3] = {
        {0, 1, 2}, {1, 0, 2}, {0, 2, 1}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    std::memset(output, 0, wire_size(count, rgbw));
    auto* cursor = output + kResetBytes;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = pixels[reverse ? count - 1 - i : i];
        const std::uint8_t rgb[] = {p.r, p.g, p.b, p.w};
        for (unsigned c = 0; c < (rgbw ? 4U : 3U); ++c) {
            const auto channel =
                rgbw && ((white_first && c == 0U) || (!white_first && c == 3U))
                    ? 3U
                    : channels[unsigned(order)][c - (rgbw && white_first ? 1U : 0U)];
            const auto value = rgb[channel];
            for (int bit = 7; bit > 0; bit -= 2)
                *cursor++ = static_cast<std::uint8_t>(((value & (1U << bit) ? 0xc : 0x8) << 4) |
                                                      (value & (1U << (bit - 1)) ? 0xc : 0x8));
        }
    }
    return Result::success;
}
} // namespace newmota::platform::led_strip
