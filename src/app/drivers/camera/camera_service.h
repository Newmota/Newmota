/*
  camera_service.h - Camera pins, power sequence, settings and capture lifecycle for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_service.h
 * @brief The native half of camera start-up for the DVP (`USE_WEBCAM`) and MIPI-CSI
 * (`USE_CSI_WEBCAM`) backends.
 *
 * The Berry `cam` module drives the sequence: power first, then the registered sensor
 * driver configures the sensor over I2C, then capture starts here with the mode the sensor
 * reported.
 */
#pragma once
#include "core/hardware/hardware.h"
#include "newmota_features.h"
#include "newmota/platform/camera.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::camera {

inline constexpr std::uint32_t kDefaultXclkHz = 20000000U;
inline constexpr std::uint16_t kStreamPort = 81;

enum class Phase : std::uint8_t { idle, clocked, streaming, failed };

#if USE_CSI_WEBCAM
inline constexpr std::uint8_t kDefaultResolution = 0;   // the sensor file's first preset
inline constexpr std::uint8_t kMaximumResolution = 254; // the sensor file checks its presets
#else
inline constexpr std::uint8_t kDefaultResolution = 10; // VGA in Tasmota's framesize list
inline constexpr std::uint8_t kMaximumResolution = 15; // UXGA
#endif
inline constexpr std::uint8_t kWindowResolution = 255; // WcWindow's custom CSI mode

/** @brief One sensor setting: Tasmota's `Wc<command>` name and the sensor driver's key. */
struct Setting final {
    const char* command;
    const char* key;
    std::int16_t minimum, maximum, fallback;
};

inline constexpr std::array<Setting, 23> kSettings{{
    {"Mirror", "mirror", 0, 1, 0},
    {"Flip", "flip", 0, 1, 0},
    {"Saturation", "saturation", -2, 2, 0},
    {"Brightness", "brightness", -2, 2, 0},
    {"Contrast", "contrast", -2, 2, 0},
    {"SpecialEffect", "effect", 0, 6, 0},
    {"AWB", "awb", 0, 1, 1},
    {"WBMode", "wb_mode", 0, 4, 0},
    {"AWBGain", "awb_gain", 0, 1, 1},
    {"AEC", "aec", 0, 1, 1},
    {"AECValue", "aec_value", 0, 1200, 204},
    {"AELevel", "ae_level", -2, 2, 0},
    {"AECDSP", "aec2", 0, 1, 1},
    {"AGC", "agc", 0, 1, 1},
    {"AGCGain", "agc_gain", 0, 30, 5},
    {"GainCeiling", "gainceiling", 0, 6, 0},
    {"GammaCorrect", "raw_gma", 0, 1, 1},
    {"LensCorrect", "lenc", 0, 1, 1},
    {"WPC", "wpc", 0, 1, 1},
    {"DCW", "dcw", 0, 1, 1},
    {"BPC", "bpc", 0, 1, 0},
    {"Colorbar", "colorbar", 0, 1, 0},
#if USE_CSI_WEBCAM
    {"Quality", "quality", 1, 100, 50}, // the P4's JPEG and H.264 encoders, as in Tasmota
#else
    {"Quality", "quality", 4, 63, 12}, // the OV2640 scale: lower is better
#endif
}};
inline constexpr std::size_t kQualitySetting = kSettings.size() - 1U;
inline constexpr std::size_t kMirrorSetting = 0;
inline constexpr std::size_t kFlipSetting = 1;

/** @brief Stored camera choices in `/.newmota/webcam.a`/`.b`, applied on every start. */
struct Stored final {
    std::uint8_t resolution{kDefaultResolution};
    bool stream{};
    std::uint8_t session{1}; ///< CSI: WcSession 1 MJPEG, 2 H.264, 3 WebRTC, 4 still
    std::array<std::int16_t, kSettings.size()> values{};
};

/** @brief The mode a sensor driver reports after `start`; DVP uses the frame size. */
struct Capture final {
    std::uint16_t width{}, height{};
    std::uint8_t format{1}, lanes{2}; ///< CSI: 0 RAW8, 1 RAW10, 2 RAW12
    std::uint16_t lane_mbps{};
    std::uint8_t fps{30};
    std::uint8_t bayer{};                    ///< CSI: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB
    std::int16_t offset_x{}, offset_y{};     ///< CSI: applied shift from the centred crop
    std::uint16_t max_width{}, max_height{}; ///< CSI: the sensor's full array
    std::uint8_t binning{1};                 ///< CSI: 1 or 2
};

#if USE_CSI_WEBCAM
/**
 * @brief The part of the sensor the window shows, as Matter's ViewportStruct: full-sensor
 * pixels before binning, oriented as the shown picture (x right, y down), x2 and y2 exclusive.
 */
struct Viewport final {
    std::uint16_t x1{}, y1{}, x2{}, y2{};
};

/** @brief Digital pan and tilt, -100..100: where the window sits in the sensor, 0,0 centred. */
struct Position final {
    int pan{}, tilt{}; ///< Positive pans right and tilts up in the shown picture
};

/** @brief Reports whether the window is smaller than the sensor, so the viewport can move. */
[[nodiscard]] bool pannable(const Capture& mode);

/** @brief The viewport of @p mode's window, after mirror and flip. */
[[nodiscard]] Viewport viewport(const Capture& mode);

/**
 * @brief The window offsets in output pixels that show @p target.
 * @return false when @p target leaves the sensor or has another size than the window's:
 * the camera crops and cannot zoom.
 */
[[nodiscard]] bool viewport_offsets(
    const Capture& mode, const Viewport& target, std::int16_t& offset_x, std::int16_t& offset_y);

/** @brief The position of @p mode's viewport. */
[[nodiscard]] Position position(const Capture& mode);

/** @brief The viewport of @p mode's size at @p target. */
[[nodiscard]] Viewport viewport_at(const Capture& mode, const Position& target);
#endif

/** @brief Live state shown by Webcam, WcConfig and Berry `cam.info()`. */
struct Status final {
    Phase phase{Phase::idle};
    Capture mode{};
    std::uint8_t resolution{};
    std::array<char, 16> sensor{};
    std::array<char, 48> error{};
};

/** @brief Loads the stored choices and resolves DVP pins; incomplete wiring keeps DVP off. */
void initialize(const Hardware& hardware);

/** @brief Reports whether the camera can start: DVP pins complete, CSI always. */
[[nodiscard]] bool configured();

/**
 * @brief Returns the Template's SCCB pads for the I2C owner.
 * @return false when SIOC or SIOD is not assigned.
 */
[[nodiscard]] bool sccb_pins(const Hardware& hardware, PinId& scl, PinId& sda);

/** @brief Records which zero-based I2C bus carries SCCB, for status. */
void set_sccb_bus(std::uint8_t bus);

/** @brief Returns the one-based SCCB bus, or 0 when none is configured. */
[[nodiscard]] std::uint8_t sccb_bus();

/**
 * @brief Powers the sensor side: DVP PWDN/RESET and XCLK, CSI the MIPI PHY supply.
 * @return false with Status::error set.
 */
[[nodiscard]] bool power_up(std::uint32_t xclk_hz);

/**
 * @brief Starts capture for the mode the sensor driver reported.
 * @return false with Status::error set; the hardware is stopped as well.
 */
[[nodiscard]] bool start_capture(const Capture& mode);

/** @brief Stops capture, the stream servers and the sensor clock or PHY supply. */
void power_down();

/** @brief Marks a failed start with a short reason and stops the hardware. */
void fail(const char* reason);

/** @brief Records the active mode and sensor name of a successful start. */
void set_mode(std::uint8_t resolution, const Capture& mode, const char* sensor);

/** @brief Runs periodic work: CSI auto white balance and the WebRTC audio track. */
void poll(std::uint32_t now);

/** @brief Returns the live status. */
[[nodiscard]] const Status& status();

/** @brief Returns the stored choices loaded at start-up. */
[[nodiscard]] const Stored& stored();

/**
 * @brief Replaces the stored choices after range checks.
 * @return false when a value is out of range or the file could not be written.
 */
[[nodiscard]] bool store(const Stored& value);

/** @brief Returns Tasmota's defaults for every setting, keeping resolution, stream, session. */
[[nodiscard]] Stored defaults(const Stored& current);

} // namespace newmota::camera
