/*
  camera.h - DVP camera capture and stream endpoint for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera.h
 * @brief Owns the DVP controller, its XCLK and a ring of JPEG frame buffers in PSRAM.
 *
 * Sensor configuration is not part of this boundary: the application starts the clock,
 * lets the sensor driver configure the sensor over I2C, then starts capture. Capture and
 * the stream endpoint run on native tasks; every other call belongs to the application task.
 */
#pragma once
#include "newmota/core/hardware/pin.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::platform::camera {

inline constexpr std::size_t kDataPins = 8;

enum class Result : std::uint8_t {
    success,
    unavailable,
    invalid_argument,
    busy,
    no_memory,
    failed,
};

/** @brief Physical DVP pads; all are required except the controller-generated XCLK. */
struct Pins final {
    PinId xclk{}, pclk{}, vsync{}, href{};
    std::array<PinId, kDataPins> data{};
};

/** @brief One completed JPEG frame, valid until give(). */
struct Frame final {
    const std::uint8_t* data{};
    std::size_t size{};
    std::uint32_t sequence{};
    std::uint32_t timestamp_ms{};
    std::uint8_t slot{0xffU};
};

/** @brief Counters for status reports; frames counts completed JPEG frames. */
struct Statistics final {
    std::uint32_t frames{}, dropped{}, invalid{};
    std::size_t buffer_size{};
    bool capturing{};
};

/**
 * @brief Routes the DVP pads and starts XCLK so the sensor answers on I2C.
 * @param pins Pads resolved from the GPIO configuration.
 * @param xclk_hz Sensor input clock; the controller source must divide it exactly.
 */
[[nodiscard]] Result start_clock(const Pins& pins, std::uint32_t xclk_hz);

/**
 * @brief Allocates frame buffers and starts JPEG capture after the sensor streams.
 * @param buffer_size Bytes per frame buffer; a larger JPEG frame is dropped.
 */
[[nodiscard]] Result start(std::size_t buffer_size);

/**
 * @brief Stops capture, the controller and XCLK.
 *
 * Buffers still held by a reader are released by its give().
 */
void stop();

/** @brief Reports whether XCLK runs, with or without capture. */
[[nodiscard]] bool clock_running();

/**
 * @brief Waits for a frame newer than @p after_sequence and holds it for the caller.
 * @return false on timeout or when capture stops.
 */
[[nodiscard]] bool take(Frame& frame, std::uint32_t after_sequence, std::uint32_t timeout_ms);

/** @brief Releases a frame returned by take(). */
void give(Frame& frame);

/** @brief Copies the current counters. */
[[nodiscard]] Statistics statistics();

/**
 * @brief Starts or stops the plain HTTP snapshot and MJPEG endpoint.
 * @param port TCP port, Tasmota's webcam stream uses 81.
 */
[[nodiscard]] bool serve(bool enable, std::uint16_t port);

/** @brief Reports whether the stream endpoint listens. */
[[nodiscard]] bool serving();

/** @brief Counts stream endpoint starts since boot; a change means its viewers were dropped. */
[[nodiscard]] std::uint32_t serves();

} // namespace newmota::platform::camera
