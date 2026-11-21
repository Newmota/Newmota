/*
  cam_module.cpp - Berry cam module and Wc commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file cam_module.cpp
 * @brief Sensor drivers are Berry objects; this module runs their start-up and settings.
 *
 * A sensor file defines a class with `start(resolution)`, `stream(on)` and `set(key, value)`
 * and calls `cam.register(instance)`. After a successful `start` the instance's `width`,
 * `height` and `name` members describe the frames; an optional `xclk` member sets the
 * sensor clock. Frames never pass through the VM.
 */
#include "services/berry/modules/cam/cam_module.h"

#include "drivers/camera/camera_service.h"
#include "newmota/log.h"
#include "newmota/platform/camera.h"
#if USE_CSI_WEBCAM
#include "newmota/platform/camera_csi.h"
#endif
#include "newmota/platform/i2c.h"
#include "newmota/platform/platform.h"
#include "services/berry/berry_service.h"
#if USE_BERRY_IMAGE
#include "services/berry/modules/img/img_module.h"
#endif
#include "support/number.h"
#include "support/text.h"
extern "C" {
#include "berry.h"
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

namespace newmota {
namespace {
namespace i2c = platform::i2c;
constexpr const char* kSensorGlobal = "_camera_sensor";
constexpr std::uint32_t kImageTimeoutMs = 2000;
constexpr std::size_t kDeadlineCheckInterval = 32;

std::uint32_t started_ms{};

/** @brief Validates a bounded Berry integer argument. */
int integer(bvm* vm, const int index, const int low, const int high) {
    if (!be_isint(vm, index))
        be_raise(vm, "type_error", "cam expects integer arguments");
    const auto value = be_toint(vm, index);
    if (value < low || value > high)
        be_raise(vm, "value_error", "cam argument out of range");
    return static_cast<int>(value);
}

/** @brief Resolves a Wire instance to its zero-based bus. */
std::uint8_t wire_bus(bvm* vm, const int index) {
    if (!be_isinstance(vm, index) || !be_getmember(vm, index, "bus") || !be_isint(vm, -1))
        be_raise(vm, "type_error", "cam expects a Wire instance");
    const auto bus = be_toint(vm, -1);
    be_pop(vm, 1);
    if (bus < 1 || bus > static_cast<int>(i2c::kBusCount))
        be_raise(vm, "value_error", "cam Wire bus out of range");
    return static_cast<std::uint8_t>(bus - 1);
}

/** @brief Reads the optional register width argument: 1 or 2 address bytes. */
int register_width(bvm* vm, const int index) {
    return be_top(vm) >= index ? integer(vm, index, 1, 2) : 1;
}

/** @brief Writes a register address in big-endian order. */
std::size_t encode_register(std::uint8_t* out, const int reg, const int width) {
    if (width == 2) {
        out[0] = static_cast<std::uint8_t>(reg >> 8);
        out[1] = static_cast<std::uint8_t>(reg);
        return 2;
    }
    out[0] = static_cast<std::uint8_t>(reg);
    return 1;
}

/** @brief Logs the exception a failed protected call left on the stack. */
void log_sensor_error(bvm* vm, const char* method) {
    const int top = be_top(vm);
    const char* exception = top >= 2 && be_isstring(vm, -2) ? be_tostring(vm, -2) : "exception";
    const char* message = top >= 1 && be_isstring(vm, -1) ? be_tostring(vm, -1) : "failed";
    AddLog(LogLevel::error, "CAM: sensor %s failed [%s] %s", method, exception, message);
}

/**
 * @brief Calls `sensor.<method>(...)` with arguments pushed by @p push.
 * @param truthy Receives the truthiness of the return value.
 * @return false when the method is missing or raised; the stack is restored either way.
 */
template <typename Push>
bool call_sensor(
    bvm* vm, const int sensor, const char* method, const int argc, Push push, bool& truthy) {
    const int top = be_top(vm);
    truthy = false;
    if (!be_getmember(vm, sensor, method) || !be_isfunction(vm, -1)) {
        be_pop(vm, be_top(vm) - top);
        return false;
    }
    const int function = be_top(vm);
    be_pushvalue(vm, sensor);
    push();
    if (be_pcall(vm, argc + 1) != BE_OK) {
        log_sensor_error(vm, method);
        be_pop(vm, be_top(vm) - top);
        return false;
    }
    truthy = be_tobool(vm, function);
    be_pop(vm, be_top(vm) - top);
    return true;
}

/** @brief Reads an integer member of the sensor, or 0. */
int sensor_integer(bvm* vm, const int sensor, const char* name) {
    int value{};
    if (be_getmember(vm, sensor, name) && be_isint(vm, -1))
        value = static_cast<int>(be_toint(vm, -1));
    be_pop(vm, 1);
    return value;
}

/** @brief Applies one stored setting through the sensor; unsupported keys are ignored. */
void apply_setting(bvm* vm, const int sensor, const std::size_t index, const int value) {
    bool accepted{};
    if (call_sensor(
            vm, sensor, "set", 2,
            [&] {
                be_pushstring(vm, camera::kSettings[index].key);
                be_pushint(vm, value);
            },
            accepted) &&
        !accepted)
        AddLog(LogLevel::debug, "CAM: sensor ignores %s", camera::kSettings[index].key);
}

/** @brief Pushes the registered sensor; returns its absolute index or 0. */
int push_sensor(bvm* vm) {
    if (!be_getglobal(vm, kSensorGlobal) || be_isnil(vm, -1)) {
        be_pop(vm, 1);
        return 0;
    }
    return be_absindex(vm, -1);
}

/** @brief WcWindow's custom CSI mode, handed to the sensor's optional `window` method. */
struct Window final {
    int x, y, width, height, binning, fps, format;
};

/** @brief Starts the stream server: stored WcStream, or a CSI session that needs its page. */
void serve_if_wanted() {
    const auto& stored = camera::stored();
    bool wanted = stored.stream;
#if USE_CSI_WEBCAM
    wanted = wanted || stored.session != 1U; // the H.264 player and the WebRTC offer
#endif
    if (wanted && !platform::camera::serve(true, camera::kStreamPort))
        AddLog(
            LogLevel::error, "CAM: stream server on port %u failed", unsigned(camera::kStreamPort));
}

/** @brief Passes every stored setting to the sensor's `set`. */
void apply_settings(bvm* vm, const int sensor) {
    const auto& stored = camera::stored();
    for (std::size_t i = 0; i < camera::kSettings.size(); ++i)
        apply_setting(vm, sensor, i, stored.values[i]);
}

/** @brief Reads the mode the sensor's members describe; returns a failure reason or nullptr. */
const char* read_mode(bvm* vm, const int sensor, camera::Capture& mode) {
    const int width = sensor_integer(vm, sensor, "width");
    const int height = sensor_integer(vm, sensor, "height");
    if (width <= 0 || height <= 0 || width > 0xffff || height > 0xffff)
        return "sensor reported no frame size";
    mode.width = static_cast<std::uint16_t>(width);
    mode.height = static_cast<std::uint16_t>(height);
#if USE_CSI_WEBCAM
    // Tasmota's sensor files call the lane rate mipi_clock.
    int lane_mbps = sensor_integer(vm, sensor, "lane_mbps");
    if (lane_mbps <= 0)
        lane_mbps = sensor_integer(vm, sensor, "mipi_clock");
    const int lanes = sensor_integer(vm, sensor, "lanes");
    const int fps = sensor_integer(vm, sensor, "fps");
    mode.format = static_cast<std::uint8_t>(std::clamp(sensor_integer(vm, sensor, "format"), 0, 2));
    mode.lanes = static_cast<std::uint8_t>(lanes > 0 ? std::min(lanes, 4) : 2);
    mode.lane_mbps = static_cast<std::uint16_t>(std::clamp(lane_mbps, 0, 0xffff));
    mode.fps = static_cast<std::uint8_t>(fps > 0 ? std::min(fps, 120) : 30);
    mode.bayer = static_cast<std::uint8_t>(std::clamp(sensor_integer(vm, sensor, "bayer"), 0, 3));
    mode.offset_x = static_cast<std::int16_t>(
        std::clamp(sensor_integer(vm, sensor, "offset_x"), -0x8000, 0x7fff));
    mode.offset_y = static_cast<std::int16_t>(
        std::clamp(sensor_integer(vm, sensor, "offset_y"), -0x8000, 0x7fff));
    mode.max_width = static_cast<std::uint16_t>(
        std::clamp(sensor_integer(vm, sensor, "max_width"), int(mode.width), 0xffff));
    mode.max_height = static_cast<std::uint16_t>(
        std::clamp(sensor_integer(vm, sensor, "max_height"), int(mode.height), 0xffff));
    mode.binning =
        static_cast<std::uint8_t>(std::clamp(sensor_integer(vm, sensor, "bin_mode"), 1, 2));
    if (mode.lane_mbps == 0U)
        return "sensor reported no lane rate";
#endif
    return nullptr;
}

/** @brief Calls the sensor's `window`; false when it is missing, raised or refused. */
bool call_window(bvm* vm, const int sensor, const Window& window) {
    bool ok{};
    return call_sensor(
               vm, sensor, "window", 7,
               [&] {
                   for (const int value : {window.x, window.y, window.width, window.height,
                            window.binning, window.fps, window.format})
                       be_pushint(vm, value);
               },
               ok) &&
           ok;
}

#if USE_CSI_WEBCAM
/**
 * @brief Asks the sensor again for a centred window when the ISP or the session's encoder
 * cannot take its mode, as Tasmota does before anything is allocated.
 * @return A failure reason, or nullptr when @p mode fits now.
 */
const char* fit_sensor(bvm* vm, const int sensor, camera::Capture& mode) {
    const auto session = static_cast<platform::camera_csi::Session>(camera::stored().session);
    if (session == platform::camera_csi::Session::still && mode.format > 1U)
        return "still session needs a RAW8 or RAW10 sensor mode";
    auto width = mode.width, height = mode.height;
    platform::camera_csi::fit_window(session, mode.format, width, height);
    if (width == mode.width && height == mode.height)
        return nullptr;
    if (width < 16U || height < 16U)
        return "sensor window too small for the session";
    AddLog(LogLevel::info, "CAM: sensor gave %ux%u, asking for a centred %ux%u",
        unsigned(mode.width), unsigned(mode.height), unsigned(width), unsigned(height));
    if (!call_window(vm, sensor, {0, 0, width, height, mode.binning, mode.fps, mode.format}))
        return "sensor cannot crop to the ISP limit";
    if (const char* reason = read_mode(vm, sensor, mode))
        return reason;
    if (mode.width != width || mode.height != height)
        return "sensor window does not fit the ISP";
    return nullptr;
}
#endif

/**
 * @brief Runs the full start: power, sensor init, capture, stream, stored settings.
 * @param window A CSI custom window instead of the preset @p resolution.
 * @return false with the camera status holding the reason.
 */
bool start_camera(bvm* vm, const std::uint8_t resolution, const Window* window = nullptr) {
    const int top = be_top(vm);
    const auto restore = [vm, top] { be_pop(vm, be_top(vm) - top); };
    camera::power_down();
    const int sensor = push_sensor(vm);
    if (!sensor) {
        camera::fail("no sensor driver registered");
        return false;
    }
    std::uint32_t xclk = camera::kDefaultXclkHz;
    if (const int value = sensor_integer(vm, sensor, "xclk"); value > 0)
        xclk = static_cast<std::uint32_t>(value);
    if (!camera::power_up(xclk)) {
        restore();
        return false;
    }
#if USE_CSI_WEBCAM
    // A CSI sensor writes flip and mirror into its register tables and reports the Bayer
    // order they give, which the ISP needs from the start. The session lets it pick RAW8
    // and a longer exposure for stills.
    apply_settings(vm, sensor);
    bool known{};
    (void)call_sensor(
        vm, sensor, "set", 2,
        [&] {
            be_pushstring(vm, "session");
            be_pushint(vm, camera::stored().session);
        },
        known);
#endif
    bool ok{};
    const bool called =
        window ? (ok = call_window(vm, sensor, *window))
               : call_sensor(vm, sensor, "start", 1, [&] { be_pushint(vm, resolution); }, ok);
    if (!called || !ok) {
        restore();
        camera::fail(window ? "sensor window failed" : "sensor init failed");
        return false;
    }
    camera::Capture mode{};
    const char* reason = read_mode(vm, sensor, mode);
#if USE_CSI_WEBCAM
    if (!reason)
        reason = fit_sensor(vm, sensor, mode);
#endif
    if (reason) {
        restore();
        camera::fail(reason);
        return false;
    }
    char name[16]{};
    if (be_getmember(vm, sensor, "name") && be_isstring(vm, -1))
        std::strncpy(name, be_tostring(vm, -1), sizeof(name) - 1U);
    be_pop(vm, 1);
    // Before capture: the CSI pipeline looks up the sensor's /isp.json section by name.
    camera::set_mode(window ? camera::kWindowResolution : resolution, mode, name);
    if (!camera::start_capture(mode)) {
        restore();
        return false;
    }
    if (!call_sensor(vm, sensor, "stream", 1, [&] { be_pushbool(vm, true); }, ok) || !ok) {
        restore();
        camera::fail("sensor stream start failed");
        return false;
    }
#if !USE_CSI_WEBCAM
    apply_settings(vm, sensor);
#endif
    restore();
    started_ms = platform::monotonic_milliseconds();
    serve_if_wanted();
    AddLog(LogLevel::info, "CAM: %s %ux%u streaming", name[0] ? name : "sensor",
        unsigned(mode.width), unsigned(mode.height));
    return true;
}

/** @brief Asks the sensor to stop before the hardware goes down. */
void stop_camera(bvm* vm) {
    const int top = be_top(vm);
    if (camera::status().phase == camera::Phase::streaming) {
        if (const int sensor = push_sensor(vm)) {
            bool ok{};
            (void)call_sensor(vm, sensor, "stream", 1, [&] { be_pushbool(vm, false); }, ok);
        }
    }
    be_pop(vm, be_top(vm) - top);
    camera::power_down();
}

const char* phase_name(const camera::Phase phase) {
    switch (phase) {
    case camera::Phase::idle:
        return "idle";
    case camera::Phase::clocked:
        return "starting";
    case camera::Phase::streaming:
        return "streaming";
    case camera::Phase::failed:
        return "failed";
    }
    return "idle";
}

void map_insert_int(bvm* vm, const char* key, const int value) {
    be_pushstring(vm, key);
    be_pushint(vm, value);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

void map_insert_string(bvm* vm, const char* key, const char* value) {
    be_pushstring(vm, key);
    be_pushstring(vm, value);
    be_data_insert(vm, -3);
    be_pop(vm, 2);
}

// Berry API ---------------------------------------------------------------

/** @brief `cam.register(sensor)`: keeps the driver and starts the camera when wired. */
int Register(bvm* vm) {
    if (be_top(vm) < 1 || !be_isinstance(vm, 1))
        be_raise(vm, "type_error", "cam.register expects a sensor instance");
    be_pushvalue(vm, 1);
    be_setglobal(vm, kSensorGlobal);
    be_pop(vm, 1);
    if (!camera::configured()) {
        AddLog(LogLevel::info, "CAM: sensor registered, DVP pins not configured");
        be_pushbool(vm, false);
        be_return(vm);
    }
    be_pushbool(vm, start_camera(vm, camera::stored().resolution));
    be_return(vm);
}

/** @brief `cam.setup([mode])` restarts with the stored or given resolution, as Tasmota's
 * `cam.setup(mode)`; the mode is not stored. */
int Setup(bvm* vm) {
    const auto resolution = be_top(vm) >= 1 ? integer(vm, 1, 0, camera::kMaximumResolution)
                                            : camera::stored().resolution;
    be_pushbool(vm, start_camera(vm, static_cast<std::uint8_t>(resolution)));
    be_return(vm);
}

/** @brief `cam.stop()` stops the stream server, capture and XCLK. */
int Stop(bvm* vm) {
    stop_camera(vm);
    be_return_nil(vm);
}

/** @brief `cam.info()` returns the state and counters as a map; `mode` is the resolution. */
int Info(bvm* vm) {
    const auto& status = camera::status();
    const auto stats = platform::camera::statistics();
    be_newobject(vm, "map");
    map_insert_string(vm, "state", phase_name(status.phase));
    map_insert_string(vm, "sensor", status.sensor.data());
    map_insert_int(vm, "width", status.mode.width);
    map_insert_int(vm, "height", status.mode.height);
#if USE_CSI_WEBCAM
    const auto csi = platform::camera_csi::info();
    map_insert_int(vm, "session", static_cast<int>(camera::stored().session));
    map_insert_int(vm, "fps", static_cast<int>(csi.fps));
    map_insert_int(vm, "motion", static_cast<int>(csi.motion));
    map_insert_int(vm, "sensor_width", status.mode.max_width);
    map_insert_int(vm, "sensor_height", status.mode.max_height);
#endif
    map_insert_int(vm, "mode", status.resolution);
    map_insert_int(vm, "frames", static_cast<int>(stats.frames));
    map_insert_int(vm, "dropped", static_cast<int>(stats.dropped));
    map_insert_int(vm, "invalid", static_cast<int>(stats.invalid));
    map_insert_int(vm, "buffer", static_cast<int>(stats.buffer_size));
    map_insert_int(vm, "sccb_bus", camera::sccb_bus());
    map_insert_int(vm, "stream_port", platform::camera::serving() ? camera::kStreamPort : 0);
    if (status.error[0])
        map_insert_string(vm, "error", status.error.data());
    be_pop(vm, 1);
    be_return(vm);
}

/**
 * @brief `cam.get_image()` copies the newest JPEG frame into bytes, or returns nil.
 *
 * Raises value_error for a frame above BE_BYTES_MAX_SIZE instead of returning a cut JPEG.
 * With USE_BERRY_IMAGE, `cam.get_image(img [, format])` stores the JPEG in the `img`, or
 * decodes it there, and returns true, or false without a frame; the frame is held only while
 * it is copied or decoded.
 */
int GetImage(bvm* vm) {
    if (be_top(vm) >= 1 && !be_isnil(vm, 1)) {
#if USE_BERRY_IMAGE
        auto& image = img::instance(vm, 1);
        const auto format =
            be_top(vm) >= 2 && !be_isnil(vm, 2) ? img::format(vm, 2) : img::Format::jpeg;
        platform::camera::Frame frame{};
        if (!platform::camera::take(frame, 0U, kImageTimeoutMs)) {
            be_pushbool(vm, false);
            be_return(vm);
        }
        const auto result = img::decode_jpeg(image, frame.data, frame.size, format);
        platform::camera::give(frame);
        img::raise(vm, result);
        be_pushbool(vm, true);
        be_return(vm);
#else
        be_raise(vm, "type_error", "cam.get_image(img) needs USE_BERRY_IMAGE");
#endif
    }
    platform::camera::Frame frame{};
    if (!platform::camera::take(frame, 0U, kImageTimeoutMs))
        be_return_nil(vm);
    // be_pushbytes() would silently cut a larger frame to the bytes limit.
    if (frame.size > BE_BYTES_MAX_SIZE) {
        platform::camera::give(frame);
        be_raise(vm, "value_error", "camera frame exceeds the Berry bytes limit");
    }
    be_pushbytes(vm, frame.data, frame.size);
    platform::camera::give(frame);
    be_return(vm);
}

#if USE_CSI_WEBCAM
bool move_window(bvm* vm, std::int16_t offset_x, std::int16_t offset_y);

/** @brief Pushes a viewport as the list [x1, y1, x2, y2]. */
void push_viewport(bvm* vm, const camera::Viewport& at) {
    be_newobject(vm, "list");
    for (const int value : {int(at.x1), int(at.y1), int(at.x2), int(at.y2)}) {
        be_pushint(vm, value);
        be_data_push(vm, -2);
        be_pop(vm, 1);
    }
    be_pop(vm, 1);
}

/**
 * @brief `cam.viewport()` returns the part of the sensor the window shows as
 * [x1, y1, x2, y2] in full-sensor pixels, or nil while the camera does not stream.
 */
int Viewport(bvm* vm) {
    const auto& status = camera::status();
    if (status.phase != camera::Phase::streaming)
        be_return_nil(vm);
    push_viewport(vm, camera::viewport(status.mode));
    be_return(vm);
}

/**
 * @brief `cam.set_viewport(x1, y1, x2, y2)` moves the window there and returns the applied
 * viewport, or nil when the camera does not stream or the rectangle leaves the sensor or has
 * another size than the window: the camera crops and cannot zoom.
 */
int SetViewport(bvm* vm) {
    camera::Viewport target{};
    std::uint16_t* const fields[] = {&target.x1, &target.y1, &target.x2, &target.y2};
    for (int i = 0; i < 4; ++i)
        *fields[i] = static_cast<std::uint16_t>(integer(vm, i + 1, 0, 0xffff));
    const auto& status = camera::status();
    std::int16_t x{}, y{};
    if (status.phase != camera::Phase::streaming ||
        !camera::viewport_offsets(status.mode, target, x, y) || !move_window(vm, x, y))
        be_return_nil(vm);
    push_viewport(vm, camera::viewport(camera::status().mode));
    be_return(vm);
}

#if USE_MATTER_CAMERA
/** @brief `cam.webrtc_answer(sdp)` answers an offer from Matter; nil outside a WebRTC session. */
int WebRtcAnswer(bvm* vm) {
    constexpr std::size_t kMaxAnswer = 4096;
    const char* const offer = be_isstring(vm, 1) ? be_tostring(vm, 1) : nullptr;
    if (offer == nullptr)
        be_return_nil(vm);
    auto answer = std::unique_ptr<char[]>(new (std::nothrow) char[kMaxAnswer]);
    const std::size_t size = answer ? platform::camera_csi::webrtc_answer(
                                          offer, std::strlen(offer), 0U, answer.get(), kMaxAnswer)
                                    : 0U;
    if (size == 0U)
        be_return_nil(vm);
    be_pushnstring(vm, answer.get(), size);
    be_return(vm);
}

/** @brief `cam.webrtc_offer()` makes an offer for Matter's SolicitOffer, or nil. */
int WebRtcOffer(bvm* vm) {
    constexpr std::size_t kMaxOffer = 4096;
    auto offer = std::unique_ptr<char[]>(new (std::nothrow) char[kMaxOffer]);
    const std::size_t size =
        offer ? platform::camera_csi::webrtc_offer(0U, offer.get(), kMaxOffer) : 0U;
    if (size == 0U)
        be_return_nil(vm);
    be_pushnstring(vm, offer.get(), size);
    be_return(vm);
}

/** @brief `cam.webrtc_accept(sdp)` takes the answer to that offer; false if it cannot. */
int WebRtcAccept(bvm* vm) {
    const char* const answer = be_isstring(vm, 1) ? be_tostring(vm, 1) : nullptr;
    be_pushbool(vm, answer != nullptr &&
                        platform::camera_csi::webrtc_accept_answer(answer, std::strlen(answer)));
    be_return(vm);
}
#endif
#endif

/**
 * @brief `cam.write_regs(wire, addr, table [, reg_bytes])` writes register/value pairs.
 *
 * Each entry is the register address (1 or 2 bytes, big endian) followed by one value byte.
 * In 2-byte tables, register 0xFFFF waits for value milliseconds.
 */
int WriteRegs(bvm* vm) {
    const auto bus = wire_bus(vm, 1);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    std::size_t size{};
    const auto* table = static_cast<const std::uint8_t*>(be_tobytes(vm, 3, &size));
    if (!table)
        be_raise(vm, "type_error", "cam.write_regs expects bytes");
    const int width = register_width(vm, 4);
    const auto entry = static_cast<std::size_t>(width) + 1U;
    if (size % entry != 0U)
        be_raise(vm, "value_error", "cam.write_regs table size");
    bool ok = true;
    for (std::size_t position = 0; ok && position < size; position += entry) {
        if ((position / entry) % kDeadlineCheckInterval == 0U)
            check_berry_execution_deadline(vm);
        if (width == 2 && table[position] == 0xffU && table[position + 1U] == 0xffU) {
            platform::sleep_for_milliseconds(table[position + 2U]);
            continue;
        }
        ok = i2c::transfer(bus, address, table + position, entry, nullptr, 0) ==
             i2c::Result::success;
    }
    be_pushbool(vm, ok);
    be_return(vm);
}

/** @brief `cam.write_reg(wire, addr, reg, value [, reg_bytes])` writes one register. */
int WriteReg(bvm* vm) {
    const auto bus = wire_bus(vm, 1);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const int width = register_width(vm, 5);
    const int reg = integer(vm, 3, 0, width == 2 ? 0xffff : 0xff);
    std::uint8_t data[3]{};
    const auto length = encode_register(data, reg, width);
    data[length] = static_cast<std::uint8_t>(integer(vm, 4, 0, 255));
    be_pushbool(
        vm, i2c::transfer(bus, address, data, length + 1U, nullptr, 0) == i2c::Result::success);
    be_return(vm);
}

/** @brief `cam.read_reg(wire, addr, reg [, reg_bytes])` reads one register, or nil. */
int ReadReg(bvm* vm) {
    const auto bus = wire_bus(vm, 1);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const int width = register_width(vm, 4);
    const int reg = integer(vm, 3, 0, width == 2 ? 0xffff : 0xff);
    std::uint8_t data[2]{};
    const auto length = encode_register(data, reg, width);
    std::uint8_t value{};
    // SCCB has no repeated START: write the address, stop, then read.
    if (i2c::transfer(bus, address, data, length, nullptr, 0) != i2c::Result::success ||
        i2c::transfer(bus, address, nullptr, 0, &value, 1) != i2c::Result::success)
        be_return_nil(vm);
    be_pushint(vm, value);
    be_return(vm);
}

// Commands ----------------------------------------------------------------

bool parse_value(std::string_view payload, const camera::Setting& setting, std::int16_t& value) {
    payload = support::trim_ascii(payload);
    if (setting.minimum == 0 && setting.maximum == 1) {
        if (support::ascii_iequals(payload, "on")) {
            value = 1;
            return true;
        }
        if (support::ascii_iequals(payload, "off")) {
            value = 0;
            return true;
        }
    }
    std::int32_t parsed{};
    if (!support::parse_signed_decimal(payload, setting.minimum, setting.maximum, parsed))
        return false;
    value = static_cast<std::int16_t>(parsed);
    return true;
}

bool write_setting(CommandOutput& output, const camera::Setting& setting, const int value) {
    if (setting.minimum == 0 && setting.maximum == 1)
        return output.format("{\"Wc%s\":\"%s\"}", setting.command, value ? "ON" : "OFF");
    return output.format("{\"Wc%s\":%d}", setting.command, value);
}

bool write_overview(CommandOutput& output) {
    const auto& stored = camera::stored();
    if (!output.format("{\"Webcam\":{\"Stream\":%d,\"Resolution\":%u", stored.stream ? 1 : 0,
            unsigned(stored.resolution)))
        return false;
    for (std::size_t i = 0; i < camera::kSettings.size(); ++i)
        if (!output.format(",\"%s\":%d", camera::kSettings[i].command, stored.values[i]))
            return false;
    const auto& status = camera::status();
    if (!output.format(",\"State\":\"%s\",\"Sensor\":", phase_name(status.phase)) ||
        !output.json_string(status.sensor.data()) ||
        !output.format(",\"Width\":%u,\"Height\":%u", unsigned(status.mode.width),
            unsigned(status.mode.height)))
        return false;
#if USE_CSI_WEBCAM
    if (!output.format(",\"Session\":%u", unsigned(stored.session)))
        return false;
#endif
    if (status.error[0] &&
        (!output.write(",\"Error\":") || !output.json_string(status.error.data())))
        return false;
    return output.write("}}");
}

bool write_result(CommandOutput& output, const char* command, const bool ok) {
    if (ok)
        return output.format("{\"%s\":\"Done\"}", command);
    const auto& error = camera::status().error;
    return output.format("{\"%s\":", command) &&
           output.json_string(error[0] ? error.data() : "Failed") && output.write("}");
}

/** @brief Applies a changed setting to a streaming sensor. */
void apply_live(bvm* vm, const std::size_t index, const int value) {
    if (camera::status().phase != camera::Phase::streaming)
        return;
    const int top = be_top(vm);
    if (const int sensor = push_sensor(vm))
        apply_setting(vm, sensor, index, value);
    be_pop(vm, be_top(vm) - top);
}

#if USE_CSI_WEBCAM
const char* session_name(const unsigned session) {
    static constexpr const char* kNames[] = {"None", "MJPEG", "RTSP", "WebRTC", "Still"};
    return session < 5U ? kNames[session] : "Unknown";
}

/**
 * @brief Shifts the streaming window to new offsets.
 *
 * When the sensor keeps the mode, only the sensor is reprogrammed between pause() and
 * resume(): viewers stay connected and see a short gap. A changed mode restarts the camera.
 */
bool move_window(bvm* vm, const std::int16_t offset_x, const std::int16_t offset_y) {
    const auto& status = camera::status();
    if (status.phase != camera::Phase::streaming)
        return false;
    const auto old = status.mode;
    const Window window{
        offset_x, offset_y, old.width, old.height, old.binning, old.fps, old.format};
    const int top = be_top(vm);
    const auto restore = [vm, top] { be_pop(vm, be_top(vm) - top); };
    const int sensor = push_sensor(vm);
    bool ok{};
    if (!sensor ||
        !call_sensor(
            vm, sensor, "stream", 1, [&] { be_pushbool(vm, false); }, ok) ||
        !platform::camera_csi::pause()) {
        restore();
        return start_camera(vm, camera::kWindowResolution, &window);
    }
    camera::Capture mode{};
    const bool same = call_window(vm, sensor, window) && !read_mode(vm, sensor, mode) &&
                      mode.width == old.width && mode.height == old.height &&
                      mode.format == old.format && mode.lanes == old.lanes &&
                      mode.lane_mbps == old.lane_mbps && mode.fps == old.fps &&
                      mode.bayer == old.bayer;
    if (same && platform::camera_csi::resume() &&
        call_sensor(vm, sensor, "stream", 1, [&] { be_pushbool(vm, true); }, ok) && ok) {
        const auto name = status.sensor; // set_mode() clears the status it would copy from
        camera::set_mode(status.resolution, mode, name.data());
        restore();
        return true;
    }
    restore();
    return start_camera(vm, camera::kWindowResolution, &window);
}

/** @brief Moves the window to @p target and writes the applied position. */
CommandStatus write_position(
    bvm* vm, CommandOutput& output, const char* command, const camera::Position* target) {
    if (target) {
        const auto& mode = camera::status().mode;
        std::int16_t x{}, y{};
        if (!camera::viewport_offsets(mode, camera::viewport_at(mode, *target), x, y) ||
            !move_window(vm, x, y))
            return write_result(output, command, false) ? CommandStatus::handled
                                                        : CommandStatus::failed;
    }
    const auto& mode = camera::status().mode;
    const auto applied = camera::position(mode);
    bool written{};
    if (std::strcmp(command, "WcPan") == 0)
        written = output.format("{\"WcPan\":%d}", applied.pan);
    else if (std::strcmp(command, "WcTilt") == 0)
        written = output.format("{\"WcTilt\":%d}", applied.tilt);
    else
        written =
            output.format("{\"WcPosition\":{\"Pan\":%d,\"Tilt\":%d,\"OffsetX\":%d,\"OffsetY\":%d}}",
                applied.pan, applied.tilt, int(mode.offset_x), int(mode.offset_y));
    return written ? CommandStatus::handled : CommandStatus::failed;
}

/** @brief Tasmota's CSI-only commands: WcSession, WcStatus, WcConfig and WcWindow. */
CommandStatus handle_csi(bvm* vm, const std::string_view command, const CommandRequest& request,
    const std::string_view payload, CommandOutput& output) {
    const auto done = [&](const bool written) {
        return written ? CommandStatus::handled : CommandStatus::failed;
    };
    if (support::ascii_iequals(command, "Session")) {
        if (!request.is_query()) {
            std::uint32_t value{};
            if (!support::parse_unsigned_decimal(payload, USE_CSI_STILL ? 4U : 3U, value))
                return CommandStatus::invalid;
            if (value == 0U) {
                stop_camera(vm);
            } else {
                auto stored = camera::stored();
                stored.session = static_cast<std::uint8_t>(value);
                if (!camera::store(stored))
                    return CommandStatus::failed;
                (void)start_camera(vm, stored.resolution); // a session change starts it
            }
        }
        const unsigned type =
            camera::status().phase == camera::Phase::streaming ? camera::stored().session : 0U;
        return done(output.format("{\"WcSession\":{\"Type\":%u,\"Name\":\"%s\",\"Stored\":%u}}",
            type, session_name(type), unsigned(camera::stored().session)));
    }
    if (support::ascii_iequals(command, "Status")) {
        const auto info = platform::camera_csi::info();
        const auto& status = camera::status();
        return done(
            output.format("{\"WcStatus\":{\"State\":\"%s\",\"Session\":\"%s\","
                          "\"Resolution\":\"%ux%u\",\"FramesCaptured\":%u,"
                          "\"FramesProcessed\":%u,\"FramesUnsent\":%u,",
                phase_name(status.phase), session_name(unsigned(info.session)),
                unsigned(status.mode.width), unsigned(status.mode.height),
                unsigned(info.frames_captured), unsigned(info.frames_processed),
                unsigned(info.frames_unsent)) &&
            output.format("\"JpegErrors\":%u,\"JpegResets\":%u,\"BytesSent\":%u,\"FPS\":%u,"
                          "\"LastFrameTimeMs\":%u,\"EncodeUs\":%u,\"ConvertUs\":%u,"
                          "\"Buffers\":%u,",
                unsigned(info.jpeg_errors), unsigned(info.jpeg_resets), unsigned(info.bytes_sent),
                unsigned(info.fps), unsigned(info.last_frame_ms), unsigned(info.encode_us),
                unsigned(info.convert_us), unsigned(info.buffers)) &&
            output.format("\"Size\":{\"Last\":%u,\"Min\":%u,\"Max\":%u},\"Motion\":%u,"
                          "\"SpsPps\":%s,\"WebRTC\":%s}}",
                unsigned(info.last_size), unsigned(info.min_size), unsigned(info.max_size),
                unsigned(info.motion), info.sps_pps ? "true" : "false",
                platform::camera_csi::webrtc_streaming() ? "true" : "false"));
    }
    if (support::ascii_iequals(command, "Config")) {
        const auto& status = camera::status();
        return done(output.write("{\"WcConfig\":{\"Sensor\":") &&
                    output.json_string(status.sensor.data()) &&
                    output.format(",\"Width\":%u,\"Height\":%u,\"Format\":%u,\"Lanes\":%u,"
                                  "\"MipiClock\":%u,\"FPS\":%u,\"ResIndex\":%u}}",
                        unsigned(status.mode.width), unsigned(status.mode.height),
                        unsigned(status.mode.format), unsigned(status.mode.lanes),
                        unsigned(status.mode.lane_mbps), unsigned(status.mode.fps),
                        unsigned(status.resolution)));
    }
    if (support::ascii_iequals(command, "Window")) {
        int values[7]{};
        std::size_t count{};
        for (std::size_t start = 0; start <= payload.size() && count < 7U;) {
            const auto comma = payload.find(',', start);
            const auto field = support::trim_ascii(payload.substr(
                start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
            // x and y shift the window from the centred crop, so they may be negative.
            std::int32_t parsed{};
            if (!support::parse_signed_decimal(field, count < 2U ? -8192 : 0, 8192, parsed))
                break;
            values[count++] = static_cast<int>(parsed);
            if (comma == std::string_view::npos)
                break;
            start = comma + 1U;
        }
        if (count != 7U)
            return done(output.write(
                "{\"WcWindow\":{\"Error\":\"Invalid. Use: x,y,w,h,bin,fps,format\"}}"));
        const Window window{values[0], values[1], values[2], values[3], values[4],
            values[5] ? values[5] : 30, values[6]};
        if (window.width < 16 || window.height < 16 || window.binning < 1 || window.binning > 2 ||
            window.fps > 120 || window.format > 2)
            return done(output.write("{\"WcWindow\":{\"Error\":\"Invalid geometry, binning, "
                                     "FPS or format\"}}"));
        if (!start_camera(vm, camera::kWindowResolution, &window))
            return done(write_result(output, "WcWindow", false));
        const auto& mode = camera::status().mode;
        return done(output.format("{\"WcWindow\":{\"Status\":\"Applied\",\"Width\":%u,"
                                  "\"Height\":%u,\"OffsetX\":%d,\"OffsetY\":%d,\"Binning\":%d,"
                                  "\"FPS\":%u,\"Format\":%u}}",
            unsigned(mode.width), unsigned(mode.height), int(mode.offset_x), int(mode.offset_y),
            window.binning, unsigned(mode.fps), unsigned(mode.format)));
    }
    // Digital pan and tilt move the sensor window; they are not stored.
    const bool pan = support::ascii_iequals(command, "Pan");
    const bool tilt = support::ascii_iequals(command, "Tilt");
    if (pan || tilt || support::ascii_iequals(command, "Position")) {
        const char* name = pan ? "WcPan" : (tilt ? "WcTilt" : "WcPosition");
        if (request.is_query())
            return write_position(vm, output, name, nullptr);
        auto target = camera::position(camera::status().mode);
        std::int32_t first{}, second{};
        const auto comma = payload.find(',');
        if (pan || tilt) {
            if (!support::parse_signed_decimal(payload, -100, 100, first))
                return CommandStatus::invalid;
            (pan ? target.pan : target.tilt) = static_cast<int>(first);
        } else if (comma == std::string_view::npos ||
                   !support::parse_signed_decimal(
                       support::trim_ascii(payload.substr(0, comma)), -100, 100, first) ||
                   !support::parse_signed_decimal(
                       support::trim_ascii(payload.substr(comma + 1U)), -100, 100, second)) {
            return CommandStatus::invalid;
        } else {
            target = {static_cast<int>(first), static_cast<int>(second)};
        }
        return write_position(vm, output, name, &target);
    }
    return CommandStatus::unknown;
}
#endif

CommandStatus handle(bvm* vm, const CommandRequest& request, CommandOutput& output) {
    const auto name = request.name;
    const auto done = [&](const bool written) {
        return written ? CommandStatus::handled : CommandStatus::failed;
    };
    if (support::ascii_iequals(name, "Webcam"))
        return done(write_overview(output));
    if (name.size() <= 2U || !support::ascii_iequals(name.substr(0, 2), "Wc"))
        return CommandStatus::unknown;
    const auto command = name.substr(2);
    const auto payload = support::trim_ascii(request.payload);
    auto stored = camera::stored();

    if (support::ascii_iequals(command, "Init"))
        return done(write_result(output, "WcInit", start_camera(vm, stored.resolution)));
    if (support::ascii_iequals(command, "Stop")) {
        stop_camera(vm);
        return done(output.write("{\"WcStop\":\"Done\"}"));
    }
#if USE_CSI_WEBCAM
    if (const auto csi = handle_csi(vm, command, request, payload, output);
        csi != CommandStatus::unknown)
        return csi;
#endif
    // WcRes is the CSI driver's name in Tasmota.
    if (support::ascii_iequals(command, "Resolution") || support::ascii_iequals(command, "Res")) {
        if (!request.is_query()) {
            std::uint32_t value{};
            if (!support::parse_unsigned_decimal(payload, camera::kMaximumResolution, value))
                return CommandStatus::invalid;
            stored.resolution = static_cast<std::uint8_t>(value);
            if (!camera::store(stored))
                return CommandStatus::failed;
            if (camera::status().phase == camera::Phase::streaming)
                (void)start_camera(vm, stored.resolution);
        }
        return done(output.format("{\"WcResolution\":%u}", unsigned(camera::stored().resolution)));
    }
    if (support::ascii_iequals(command, "Stream")) {
        if (!request.is_query()) {
            bool enable{};
            if (support::ascii_iequals(payload, "on") || payload == "1")
                enable = true;
            else if (!support::ascii_iequals(payload, "off") && payload != "0")
                return CommandStatus::invalid;
            stored.stream = enable;
            if (!camera::store(stored))
                return CommandStatus::failed;
            if (!platform::camera::serve(enable, camera::kStreamPort))
                AddLog(LogLevel::error, "CAM: stream server on port %u failed",
                    unsigned(camera::kStreamPort));
        }
        return done(
            output.format("{\"WcStream\":\"%s\"}", platform::camera::serving() ? "ON" : "OFF"));
    }
    if (support::ascii_iequals(command, "SetDefaults")) {
        const auto values = camera::defaults(stored);
        if (!camera::store(values))
            return CommandStatus::failed;
        for (std::size_t i = 0; i < camera::kSettings.size(); ++i)
            apply_live(vm, i, values.values[i]);
        return done(output.write("{\"WcSetDefaults\":\"Done\"}"));
    }
    if (support::ascii_iequals(command, "Stats")) {
        const auto stats = platform::camera::statistics();
        const auto elapsed = platform::monotonic_milliseconds() - started_ms;
        const auto fps = stats.capturing && elapsed > 0U
                             ? static_cast<unsigned>(stats.frames * 1000ULL / elapsed)
                             : 0U;
        return done(output.format("{\"WcStats\":{\"FPS\":%u,\"CamFail\":%u,\"JpegFail\":%u,"
                                  "\"ClientFail\":0,\"Frames\":%u,\"Buffer\":%u}}",
            fps, unsigned(stats.dropped), unsigned(stats.invalid), unsigned(stats.frames),
            unsigned(stats.buffer_size)));
    }
    for (std::size_t i = 0; i < camera::kSettings.size(); ++i) {
        const auto& setting = camera::kSettings[i];
        if (!support::ascii_iequals(command, setting.command))
            continue;
        if (!request.is_query()) {
            std::int16_t value{};
            if (!parse_value(payload, setting, value))
                return CommandStatus::invalid;
            stored.values[i] = value;
            if (!camera::store(stored))
                return CommandStatus::failed;
#if USE_CSI_WEBCAM
            // The encoders take their quality when the pipeline is built, and the ISP the
            // Bayer order that flip and mirror change.
            if ((i == camera::kQualitySetting || i == camera::kMirrorSetting ||
                    i == camera::kFlipSetting) &&
                camera::status().phase == camera::Phase::streaming)
                (void)start_camera(vm, camera::status().resolution);
            else
#endif
                apply_live(vm, i, value);
        }
        return done(write_setting(output, setting, camera::stored().values[i]));
    }
    return CommandStatus::unknown;
}
} // namespace

int dispatch_camera_command(bvm* vm) {
    auto* command = static_cast<CameraCommand*>(be_tocomptr(vm, 1));
    command->status = handle(vm, *command->request, *command->output);
    be_return_nil(vm);
}

bool camera_command_name(const std::string_view name) {
    return support::ascii_iequals(name, "Webcam") ||
           (name.size() > 2U && support::ascii_iequals(name.substr(0, 2), "Wc"));
}

void remove_cam_module() {
    camera::power_down();
}
} // namespace newmota

extern "C" {
be_native_module_attr_table(cam){
    be_native_module_function("register", newmota::Register),
    be_native_module_function("setup", newmota::Setup),
    be_native_module_function("stop", newmota::Stop),
    be_native_module_function("info", newmota::Info),
    be_native_module_function("get_image", newmota::GetImage),
#if USE_CSI_WEBCAM
    be_native_module_function("viewport", newmota::Viewport),
    be_native_module_function("set_viewport", newmota::SetViewport),
#if USE_MATTER_CAMERA
    be_native_module_function("webrtc_answer", newmota::WebRtcAnswer),
    be_native_module_function("webrtc_offer", newmota::WebRtcOffer),
    be_native_module_function("webrtc_accept", newmota::WebRtcAccept),
#endif
#endif
    be_native_module_function("write_regs", newmota::WriteRegs),
    be_native_module_function("write_reg", newmota::WriteReg),
    be_native_module_function("read_reg", newmota::ReadReg),
};
extern const bntvmodule_t be_native_module_cam = {
    "cam", cam_attrs, sizeof(cam_attrs) / sizeof(cam_attrs[0]), nullptr};
}
