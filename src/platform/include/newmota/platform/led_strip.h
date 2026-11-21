/*
  led_strip.h - Platform LED strip transport contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file led_strip.h
 * @brief Platform LED strip transport contract.
 */

#pragma once
#include "newmota/core/hardware/pin.h"
#include <cstddef>
#include <cstdint>

namespace newmota::platform::led_strip {
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unsupported,
    busy,
    no_memory,
    failed
};
using Port = std::uint8_t;
inline constexpr std::size_t kMaximumPixels = 512;
// At 3.25 MHz (the fastest allowed adapter clock), 128 zero bytes hold
// the line LOW for 315 us. Both ends include reset, including the first show.
inline constexpr std::size_t kResetBytes = 128;

inline constexpr std::size_t wire_size(std::size_t pixels, bool rgbw = false) {
    return pixels * (rgbw ? 16 : 12) + 2 * kResetBytes;
}
enum class Order : std::uint8_t { rgb, grb, rbg, gbr, brg, bgr };

struct Pixel {
    std::uint8_t r{}, g{}, b{}, w{};
};

/** @brief Encodes pixels as MSB-first 1000/1100 symbols with LOW reset periods.
 * @param pixels Source pixels in logical order.
 * @param count Number of pixels to encode.
 * @param order Channel order used on the wire.
 * @param reverse Whether to emit the pixel sequence in reverse order.
 * @param output Destination wire buffer.
 * @param capacity Capacity of @p output in bytes.
 * @param rgbw Whether to encode the fourth white channel.
 * @param white_first Whether the white channel precedes RGB channels.
 * @return The encoding result.
 */
Result encode(const Pixel*, std::size_t, Order, bool reverse, std::uint8_t*, std::size_t,
    bool rgbw = false, bool white_first = false);
/** @brief Opens a native port for the application task.
 *
 * SDK and IRQ work must not access VM storage. A `failed` result leaves hardware
 * ownership uncertain; other failures leave it untouched or safely disabled.
 * The caller retains ownership of the GPIO pad.
 */
Result open(PinId, std::size_t pixels, Port&);
/** @brief Starts transmission from a buffer that remains immutable until ready. */
Result transmit(Port, const std::uint8_t*, std::size_t);
/** @brief Polls through FIFO, final shift bit, and trailing reset completion.
 *
 * A terminal failure does not establish safe cleanup; the adapter retains
 * buffers and hardware ownership until recovery is known to be safe.
 */
Result readiness(Port);
/** @brief Closes an idle native port; failure retains hardware and pad ownership. */
Result close(Port);
} // namespace newmota::platform::led_strip
