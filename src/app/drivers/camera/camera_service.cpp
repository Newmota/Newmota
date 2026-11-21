/*
  camera_service.cpp - Camera pins, power sequence, settings and capture lifecycle for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_service.cpp
 * @brief DVP pins come from the Template's GPIO_WEBCAM_* functions, as in Tasmota; a CSI
 * sensor needs only its I2C bus.
 */
#include "drivers/camera/camera_service.h"

#include "core/storage/driver_settings_file.h"
#include "newmota/log.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#if USE_WEBCAM
#include "gpio_ids.h"
#endif
#if USE_CSI_WEBCAM
#include "core/storage/filesystem.h"
#include "drivers/audio/audio_webrtc.h"
#include "newmota/platform/camera_csi.h"

#include <memory>
#include <new>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace newmota::camera {
namespace {
constexpr std::size_t kPayloadSize = 3U + kSettings.size() * 2U;
static_assert(kPayloadSize <= kDriverSettingsMaximumPayload);

bool valid_payload(const std::uint8_t* payload);
constexpr DriverSettingsSpec kSpec{"/.newmota/webcam.bin", "/.newmota/webcam.tmp",
    "/.newmota/webcam.prev", {'N', 'M', 'W', 'C'}, 2, kPayloadSize, valid_payload};

Status state{};
Stored saved{};
std::uint8_t sccb{};

bool in_range(const Stored& value) {
    if (value.resolution > kMaximumResolution || value.session < 1U ||
        value.session > (USE_CSI_STILL ? 4U : 3U))
        return false;
    for (std::size_t i = 0; i < kSettings.size(); ++i)
        if (value.values[i] < kSettings[i].minimum || value.values[i] > kSettings[i].maximum)
            return false;
    return true;
}

std::array<std::uint8_t, kPayloadSize> encode(const Stored& value) {
    std::array<std::uint8_t, kPayloadSize> payload{};
    payload[0] = value.resolution;
    payload[1] = value.stream ? 1U : 0U;
    payload[2] = value.session;
    for (std::size_t i = 0; i < kSettings.size(); ++i) {
        const auto bits = static_cast<std::uint16_t>(value.values[i]);
        payload[3U + i * 2U] = static_cast<std::uint8_t>(bits & 0xffU);
        payload[4U + i * 2U] = static_cast<std::uint8_t>(bits >> 8U);
    }
    return payload;
}

Stored decode(const std::uint8_t* payload) {
    Stored value{};
    value.resolution = payload[0];
    value.stream = payload[1] != 0U;
    value.session = payload[2];
    for (std::size_t i = 0; i < kSettings.size(); ++i)
        value.values[i] = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(payload[3U + i * 2U] | (payload[4U + i * 2U] << 8U)));
    return value;
}

bool valid_payload(const std::uint8_t* payload) {
    return payload[1] <= 1U && in_range(decode(payload));
}

const char* result_text(const platform::camera::Result result) {
    switch (result) {
    case platform::camera::Result::success:
        return "ok";
    case platform::camera::Result::unavailable:
        return "not available";
    case platform::camera::Result::invalid_argument:
        return "invalid pins, clock or mode";
    case platform::camera::Result::busy:
        return "busy";
    case platform::camera::Result::no_memory:
        return "no PSRAM for frame buffers";
    case platform::camera::Result::failed:
        return "controller failed";
    }
    return "failed";
}

#if USE_WEBCAM
constexpr std::size_t kMinimumBufferBytes = 32U * 1024U;
constexpr std::size_t kBufferAlignment = 4096U;
constexpr std::uint32_t kPowerSettleMs = 10;

platform::camera::Pins pins{};
PinId pwdn{}, reset{};
bool complete{};

/** @brief Finds the one active pad assigned to a webcam function instance. */
PinId assigned(
    const Hardware& hardware, const std::uint16_t base, const std::uint8_t instance = 1U) {
    const auto function = generated::gpio::encode(base, instance);
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function && binding->active())
            return binding->pin;
    }
    return PinId{};
}

/** @brief Sizes a JPEG frame buffer for a sensor that compresses on the chip. */
std::size_t buffer_bytes(const std::uint16_t width, const std::uint16_t height) {
    const auto pixels = static_cast<std::size_t>(width) * height;
    const auto bytes = std::max(kMinimumBufferBytes, pixels / 4U);
    return (bytes + kBufferAlignment - 1U) / kBufferAlignment * kBufferAlignment;
}
#endif

#if USE_CSI_WEBCAM
constexpr std::size_t kMaxIspJson = 8192;
constexpr std::uint32_t kAwbPeriodMs = 250; // Tasmota's 4 Hz

std::uint32_t last_awb{};

/** @brief Reads /isp.json into @p buffer; empty when it is missing or too large. */
std::size_t read_isp_json(std::unique_ptr<char[]>& buffer) {
    std::uint32_t size{};
    filesystem::EntryType type{};
    if (filesystem::stat("/isp.json", type, size) != filesystem::ReadResult::succeeded ||
        size == 0U || size > kMaxIspJson)
        return 0;
    buffer.reset(new (std::nothrow) char[size]);
    std::size_t read{};
    if (!buffer || filesystem::read_file("/isp.json", buffer.get(), size, read) !=
                       filesystem::ReadResult::succeeded)
        return 0;
    return read;
}
#endif
} // namespace

void initialize(const Hardware& hardware) {
    auto payload = encode(defaults(Stored{}));
    (void)load_driver_settings(kSpec, payload.data());
    saved = decode(payload.data());
#if USE_WEBCAM
    pins.xclk = assigned(hardware, generated::gpio::kGpioWebcamXclkBase);
    pins.pclk = assigned(hardware, generated::gpio::kGpioWebcamPclkBase);
    pins.vsync = assigned(hardware, generated::gpio::kGpioWebcamVsyncBase);
    pins.href = assigned(hardware, generated::gpio::kGpioWebcamHrefBase);
    complete = pins.xclk.valid() && pins.pclk.valid() && pins.vsync.valid() && pins.href.valid();
    for (std::size_t i = 0; i < pins.data.size(); ++i) {
        pins.data[i] = assigned(
            hardware, generated::gpio::kGpioWebcamDataBase, static_cast<std::uint8_t>(i + 1U));
        complete = complete && pins.data[i].valid();
    }
    pwdn = assigned(hardware, generated::gpio::kGpioWebcamPwdnBase);
    reset = assigned(hardware, generated::gpio::kGpioWebcamResetBase);
    if (pwdn.valid())
        (void)platform::gpio_configure_output(pwdn, true); // powered down until WcInit
    if (reset.valid())
        (void)platform::gpio_configure_output(reset, true);
    if (complete && sccb != 0U)
        AddLog(LogLevel::info, "CAM: DVP pins configured, SCCB on I2C bus %u", unsigned(sccb));
    else if (complete)
        AddLog(LogLevel::info, "CAM: DVP pins configured, SCCB on a configured I2C bus");
    else if (pins.xclk.valid() || pins.pclk.valid() || pins.vsync.valid() || pins.href.valid())
        AddLog(LogLevel::error, "CAM: incomplete DVP pins, need XCLK, PCLK, VSYNC, HREF, DATA1..8");
#else
    (void)hardware;
#endif
}

bool configured() {
#if USE_WEBCAM
    return complete;
#else
    return true;
#endif
}

#if USE_WEBCAM
bool sccb_pins(const Hardware& hardware, PinId& scl, PinId& sda) {
    scl = assigned(hardware, generated::gpio::kGpioWebcamSiocBase);
    sda = assigned(hardware, generated::gpio::kGpioWebcamSiodBase);
    return scl.valid() && sda.valid();
}
#endif

void set_sccb_bus(const std::uint8_t bus) {
    sccb = static_cast<std::uint8_t>(bus + 1U);
}

std::uint8_t sccb_bus() {
    return sccb;
}

bool power_up(const std::uint32_t xclk_hz) {
    state.error.fill('\0');
#if USE_WEBCAM
    if (!complete) {
        fail("DVP pins not configured");
        return false;
    }
    if (platform::camera::clock_running())
        platform::camera::stop();
    if (pwdn.valid()) {
        (void)platform::gpio_write(pwdn, false);
        platform::sleep_for_milliseconds(kPowerSettleMs);
    }
    const auto result = platform::camera::start_clock(pins, xclk_hz);
    if (result != platform::camera::Result::success) {
        fail(result_text(result));
        return false;
    }
    if (reset.valid()) {
        (void)platform::gpio_write(reset, false);
        platform::sleep_for_milliseconds(kPowerSettleMs);
        (void)platform::gpio_write(reset, true);
    }
    platform::sleep_for_milliseconds(kPowerSettleMs);
#else
    (void)xclk_hz;
    // The sensor's own supply and clock are on the module; the P4's MIPI PHY needs LDO 3.
    const auto result = platform::camera_csi::prepare();
    if (result != platform::camera::Result::success) {
        fail("MIPI PHY supply failed");
        return false;
    }
#endif
    state.phase = Phase::clocked;
    return true;
}

bool start_capture(const Capture& mode) {
#if USE_WEBCAM
    const auto result = platform::camera::start(buffer_bytes(mode.width, mode.height));
#else
    platform::camera_csi::Mode csi{};
    csi.width = mode.width;
    csi.height = mode.height;
    csi.format = mode.format;
    csi.lanes = mode.lanes;
    csi.lane_mbps = mode.lane_mbps;
    csi.fps = mode.fps;
    csi.bayer = mode.bayer;
    std::strncpy(csi.name.data(), state.sensor.data(), csi.name.size() - 1U);
    std::unique_ptr<char[]> isp;
    const auto isp_size = read_isp_json(isp);
    const auto result =
        platform::camera_csi::start(csi, static_cast<platform::camera_csi::Session>(saved.session),
            static_cast<std::uint8_t>(saved.values[kQualitySetting]), {isp.get(), isp_size});
#endif
    if (result != platform::camera::Result::success) {
        fail(result_text(result));
        return false;
    }
    state.phase = Phase::streaming;
    return true;
}

void power_down() {
#if USE_CSI_WEBCAM
    stop_audio(); // the audio worker sends on the WebRTC socket
#endif
    (void)platform::camera::serve(false, kStreamPort);
#if USE_WEBCAM
    platform::camera::stop();
    if (pwdn.valid())
        (void)platform::gpio_write(pwdn, true);
#else
    platform::camera_csi::stop();
#endif
    state.phase = Phase::idle;
    state.mode = {};
}

void fail(const char* reason) {
    power_down();
    state.phase = Phase::failed;
    state.error.fill('\0');
    std::strncpy(state.error.data(), reason ? reason : "failed", state.error.size() - 1U);
    AddLog(LogLevel::error, "CAM: %s", state.error.data());
}

void set_mode(const std::uint8_t resolution, const Capture& mode, const char* sensor) {
    state.resolution = resolution;
    state.mode = mode;
    state.sensor.fill('\0');
    if (sensor)
        std::strncpy(state.sensor.data(), sensor, state.sensor.size() - 1U);
}

void poll(const std::uint32_t now) {
#if USE_CSI_WEBCAM
    if (state.phase != Phase::streaming)
        return;
    if (now - last_awb >= kAwbPeriodMs) {
        last_awb = now;
        platform::camera_csi::poll_awb();
    }
    poll_audio();
#else
    (void)now;
#endif
}

const Status& status() {
    return state;
}

const Stored& stored() {
    return saved;
}

#if USE_CSI_WEBCAM
namespace {
/** @brief One axis of the window in full-sensor pixels. */
struct Axis final {
    int sensor, size, centre; ///< Array length, window length, centred start
    bool reversed;            ///< Shown picture and sensor rows or columns run opposite ways
};

Axis axis(const Capture& mode, const bool horizontal) {
    const int binning = std::max<int>(mode.binning, 1);
    const int size = (horizontal ? mode.width : mode.height) * binning;
    const int sensor = std::max<int>(horizontal ? mode.max_width : mode.max_height, size);
    const bool reversed = saved.values[horizontal ? kMirrorSetting : kFlipSetting] != 0;
    return {sensor, size, (sensor - size) / 2, reversed};
}

/** @brief The shown start of an axis whose sensor start is @p start, or the reverse. */
int turn(const Axis& a, const int start) {
    return a.reversed ? a.sensor - a.size - start : start;
}

std::uint16_t shown_start(const Capture& mode, const bool horizontal) {
    const auto a = axis(mode, horizontal);
    const int offset = horizontal ? mode.offset_x : mode.offset_y;
    const int start = a.centre + offset * std::max<int>(mode.binning, 1);
    return static_cast<std::uint16_t>(std::clamp(turn(a, start), 0, a.sensor - a.size));
}

/** @brief Output-pixel offset of a shown start, rounded to the nearest. */
std::int16_t offset_of(const Capture& mode, const bool horizontal, const int shown) {
    const auto a = axis(mode, horizontal);
    const int binning = std::max<int>(mode.binning, 1);
    const int shift = turn(a, shown) - a.centre;
    return static_cast<std::int16_t>(
        (shift + (shift >= 0 ? binning / 2 : -(binning / 2))) / binning);
}

/** @brief A start's place in its range, -100..100, rounded to the nearest. */
int percent(const Axis& a, const int start) {
    const int range = (a.sensor - a.size) / 2;
    if (range <= 0)
        return 0;
    const int scaled = (start - a.centre) * 100;
    return std::clamp((scaled + (scaled >= 0 ? range / 2 : -(range / 2))) / range, -100, 100);
}

int start_at(const Axis& a, const int position) {
    const int scaled = std::clamp(position, -100, 100) * ((a.sensor - a.size) / 2);
    return a.centre + (scaled + (scaled >= 0 ? 50 : -50)) / 100;
}
} // namespace

bool pannable(const Capture& mode) {
    const auto x = axis(mode, true), y = axis(mode, false);
    return x.sensor > x.size || y.sensor > y.size;
}

Viewport viewport(const Capture& mode) {
    const auto x1 = shown_start(mode, true), y1 = shown_start(mode, false);
    return {x1, y1, static_cast<std::uint16_t>(x1 + axis(mode, true).size),
        static_cast<std::uint16_t>(y1 + axis(mode, false).size)};
}

bool viewport_offsets(
    const Capture& mode, const Viewport& target, std::int16_t& offset_x, std::int16_t& offset_y) {
    const auto x = axis(mode, true), y = axis(mode, false);
    if (target.x2 <= target.x1 || target.y2 <= target.y1 || target.x2 - target.x1 != x.size ||
        target.y2 - target.y1 != y.size || target.x2 > x.sensor || target.y2 > y.sensor)
        return false;
    offset_x = offset_of(mode, true, target.x1);
    offset_y = offset_of(mode, false, target.y1);
    return true;
}

Position position(const Capture& mode) {
    const auto at = viewport(mode);
    return {percent(axis(mode, true), at.x1), -percent(axis(mode, false), at.y1)};
}

Viewport viewport_at(const Capture& mode, const Position& target) {
    const auto x = axis(mode, true), y = axis(mode, false);
    const auto x1 = static_cast<std::uint16_t>(start_at(x, target.pan));
    const auto y1 = static_cast<std::uint16_t>(start_at(y, -target.tilt));
    return {
        x1, y1, static_cast<std::uint16_t>(x1 + x.size), static_cast<std::uint16_t>(y1 + y.size)};
}
#endif

bool store(const Stored& value) {
    if (!in_range(value) || !save_driver_settings(kSpec, encode(value).data()))
        return false;
    saved = value;
    return true;
}

Stored defaults(const Stored& current) {
    Stored value{};
    value.resolution = current.resolution;
    value.stream = current.stream;
    value.session = current.session;
    for (std::size_t i = 0; i < kSettings.size(); ++i)
        value.values[i] = kSettings[i].fallback;
    return value;
}

} // namespace newmota::camera
