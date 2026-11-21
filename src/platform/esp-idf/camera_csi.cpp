/*
  camera_csi.cpp - ESP-IDF MIPI-CSI camera pipeline for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi.cpp
 * @brief CSI controller, ISP, frame buffers and the processing task.
 *
 * Ported from Tasmota's xdrv_81_0_esp32_webcam_CSI_core.ino. The CSI controller writes ISP
 * output into two or three PSRAM frame buffers; the buffer the processing task reads is
 * never handed to the controller. Per frame the task encodes H.264 for the H.264 and WebRTC
 * sessions, and JPEG when a reader waits and the silicon can (camera_csi_silicon.h).
 *
 * The still session (USE_CSI_STILL) bypasses the ISP. Sensor and controller run all the
 * time, and the controller writes into its own spare buffer except for the one frame the
 * task asks for while a reader waits; that frame becomes RGB565 in place and then JPEG.
 */
#include "newmota/platform/camera_csi.h"
#include "camera_csi_internal.h"
#include "camera_csi_silicon.h"

#include "newmota_features.h"
#include "newmota/log.h"

#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>

namespace newmota::platform::camera_csi {
namespace detail {
Stats stats{};
} // namespace detail

namespace {
constexpr int kMipiPhyLdo = 3;
constexpr int kMipiPhyMillivolts = 2500;
constexpr std::uint32_t kIspClockHz = 120U * 1000U * 1000U;
constexpr std::size_t kMaxFrames = 3;
constexpr std::uint32_t kTaskStack = 16000;
constexpr UBaseType_t kTaskPriority = 5;
// Room for the CSI controller's backup frame beyond its size: alignment and heap header.
// IDF 5.5 keeps the controller claimed until reboot when its constructor runs out of memory.
constexpr std::size_t kBackupSlack = 256;
constexpr std::size_t kStreamHeadroom = 256U * 1024U;
#if USE_CSI_STILL
constexpr std::size_t kStillJpegMinimum = 64U * 1024U;
constexpr std::size_t kStillHeadroom = 512U * 1024U;
constexpr std::uint32_t kStillSettleMs = 1000; // the sensor's exposure after a start
#endif

struct Pipeline final {
    Mode mode{};
    Session session{Session::none};
    std::uint8_t quality{50};
    bool jpeg{};
    esp_ldo_channel_handle_t ldo{};
    esp_cam_ctlr_handle_t csi{};
    isp_proc_handle_t isp{};
    std::array<std::uint8_t*, kMaxFrames> frames{};
    std::size_t frame_count{}, frame_size{};
    std::size_t frame_capacity{}; ///< frame_size, or the RGB565 room of a still buffer
    esp_cam_ctlr_trans_t trans{};
    TaskHandle_t task{};
    std::uint32_t started_ms{};
#if USE_CSI_STILL
    detail::Ccm ccm{detail::kIdentityCcm}; ///< the still session's colour matrix
#endif
};

/** @brief Who owns the still buffer: the task until it arms it, then the controller. */
enum class Still : std::uint8_t { idle, armed, filling, ready };

Pipeline pipe{};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> running{}, stopping{};
// Frame buffer indices shared with the ISR; -1 means none.
volatile int write_index{}, ready_index{-1}, locked_index{-1};
volatile Still still{Still::idle};

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

/** @brief Picks the next buffer for the controller: never the one being encoded. */
bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    portENTER_CRITICAL_ISR(&lock);
    if (pipe.session == Session::still) {
        // No buffer sends the frame to the controller's spare one.
        if (still == Still::armed) {
            still = Still::filling;
            trans->buffer = pipe.frames[0];
            trans->buflen = pipe.frame_capacity;
        }
        portEXIT_CRITICAL_ISR(&lock);
        return false;
    }
    int next = write_index;
    for (std::size_t step = 1; step <= pipe.frame_count; ++step) {
        const int candidate = static_cast<int>((write_index + step) % pipe.frame_count);
        if (candidate != locked_index) {
            next = candidate;
            break;
        }
    }
    write_index = next;
    trans->buffer = pipe.frames[static_cast<std::size_t>(next)];
    trans->buflen = pipe.frame_size;
    portEXIT_CRITICAL_ISR(&lock);
    return false;
}

/** @brief Publishes the finished buffer and wakes the processing task. */
bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t*, void*) {
    if (!running.load())
        return false;
    portENTER_CRITICAL_ISR(&lock);
    ready_index = write_index;
    if (still == Still::filling) // only frames in our buffer finish here
        still = Still::ready;
    portEXIT_CRITICAL_ISR(&lock);
    ++detail::stats.frames_captured;
    BaseType_t woken = pdFALSE;
    if (pipe.task)
        vTaskNotifyGiveFromISR(pipe.task, &woken);
    return woken == pdTRUE;
}

/** @brief Encodes H.264 and sends it to the session's peers. */
bool encode_h264(const std::uint8_t* frame) {
    detail::Encoded out{};
    const auto started = esp_timer_get_time();
    if (!detail::h264_encode(frame, out) || out.size == 0U)
        return false;
    detail::stats.encode_us = static_cast<std::uint32_t>(esp_timer_get_time() - started);
    detail::stats.last_size = static_cast<std::uint32_t>(out.size);
    detail::stats.bytes_sent += static_cast<std::uint32_t>(out.size);
    if (pipe.session == Session::h264)
        detail::h264_net_send(out);
    else
        detail::webrtc_send_video(out);
    return true;
}

#if USE_CSI_STILL
/** @brief Sets the still buffer's owner. */
void set_still(const Still state) {
    portENTER_CRITICAL(&lock);
    still = state;
    portEXIT_CRITICAL(&lock);
}

/** @brief Captures, converts and encodes one still for each reader that waits. */
void still_loop() {
    const auto& mode = pipe.mode;
    const TickType_t frame_wait = pdMS_TO_TICKS(3000U / std::max<std::uint8_t>(mode.fps, 1) + 500U);
    while (!stopping.load()) {
        if (!detail::jpeg_waiting()) {
            detail::jpeg_drop_ready(); // the next reader gets a new still
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!running.load() || now_ms() - pipe.started_ms < kStillSettleMs) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const auto started = now_ms();
        (void)ulTaskNotifyTake(pdTRUE, 0);
        set_still(Still::armed);
        bool captured{};
        for (TickType_t waited = 0; !captured && waited < frame_wait && !stopping.load();) {
            const TickType_t slice = pdMS_TO_TICKS(100);
            (void)ulTaskNotifyTake(pdTRUE, slice);
            waited += slice;
            captured = still == Still::ready;
        }
        if (!captured) {
            set_still(Still::idle);
            ++detail::stats.frames_unsent;
            continue;
        }
        std::uint8_t* frame = pipe.frames[0];
        (void)esp_cache_msync(frame, pipe.frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        const auto converting = esp_timer_get_time();
        const bool converted = detail::still_convert(frame, mode, pipe.ccm, stopping);
        detail::stats.convert_us = static_cast<std::uint32_t>(esp_timer_get_time() - converting);
        const auto rgb_size = static_cast<std::size_t>(mode.width) * mode.height * 2U;
        (void)esp_cache_msync(frame, pipe.frame_capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (converted && detail::jpeg_encode(frame, rgb_size))
            ++detail::stats.frames_processed;
        else
            ++detail::stats.frames_unsent;
        set_still(Still::idle);
        detail::stats.last_frame_ms = now_ms() - started;
        AddLog(LogLevel::info, "CAM: still %ux%u, %u bytes, convert %u ms, encode %u ms",
            unsigned(mode.width), unsigned(mode.height), unsigned(detail::stats.last_size),
            unsigned(detail::stats.convert_us / 1000U), unsigned(detail::stats.encode_us / 1000U));
        vTaskDelay(pdMS_TO_TICKS(20)); // the reader takes the still before the next look
    }
}
#endif

/** @brief Encodes the newest captured frame until stop() asks it to end. */
void processing_task(void*) {
#if USE_CSI_STILL
    if (pipe.session == Session::still) {
        still_loop();
        pipe.task = nullptr;
        vTaskDelete(nullptr);
    }
#endif
    std::uint32_t second_start = now_ms(), frames_in_second{};
    while (!stopping.load()) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0U || !running.load())
            continue;
        int index{};
        portENTER_CRITICAL(&lock);
        index = ready_index;
        locked_index = index;
        portEXIT_CRITICAL(&lock);
        if (index < 0)
            continue;
        const auto started = now_ms();
        std::uint8_t* frame = pipe.frames[static_cast<std::size_t>(index)];
        (void)esp_cache_msync(frame, pipe.frame_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        bool sent{};
        if (pipe.session != Session::mjpeg)
            sent = encode_h264(frame);
        if (pipe.jpeg && detail::jpeg_wanted()) {
            const bool encoded = detail::jpeg_encode(frame, pipe.frame_size);
            sent = sent || (encoded && pipe.session == Session::mjpeg);
        }
        portENTER_CRITICAL(&lock);
        locked_index = -1;
        portEXIT_CRITICAL(&lock);
        if (sent) {
            ++detail::stats.frames_processed;
            ++frames_in_second;
        } else {
            ++detail::stats.frames_unsent;
        }
        detail::stats.last_frame_ms = now_ms() - started;
        if (now_ms() - second_start >= 1000U) {
            detail::stats.fps = frames_in_second;
            frames_in_second = 0;
            second_start = now_ms();
        }
    }
    pipe.task = nullptr;
    vTaskDelete(nullptr);
}

camera::Result fail(const char* what, const esp_err_t error, const camera::Result result) {
    AddLog(LogLevel::error, "CAM: %s failed (0x%x)", what, static_cast<unsigned>(error));
    stop();
    return result;
}

/** @brief Registers the frame callbacks and enables the controller. */
esp_err_t enable_csi() {
    esp_cam_ctlr_evt_cbs_t callbacks{};
    callbacks.on_get_new_trans = on_get_new_trans;
    callbacks.on_trans_finished = on_trans_finished;
    pipe.trans.buffer = pipe.frames[0];
    pipe.trans.buflen = pipe.frame_capacity;
    auto error = esp_cam_ctlr_register_event_callbacks(pipe.csi, &callbacks, &pipe.trans);
    if (error == ESP_OK)
        error = esp_cam_ctlr_enable(pipe.csi);
    return error;
}

#if USE_CSI_STILL
/**
 * @brief Builds the still pipeline: raw CSI into one buffer through a bypassed ISP, and a
 * one-slot RGB565 JPEG ring.
 */
camera::Result build_still(const Mode& mode, const std::string_view isp_json) {
    const auto pixels = static_cast<std::size_t>(mode.width) * mode.height;
    pipe.frame_size = pixels * (mode.format == 0U ? 8U : 10U) / 8U;
    // RAW expands to RGB565 backwards in the same buffer; whole cache lines for the sync.
    pipe.frame_capacity = (pixels * 2U + 63U) & ~std::size_t{63};
    // IDF 5.5 keeps the controller claimed until reboot when its constructor runs out of
    // memory, so check before: our buffer, the controller's raw backup and the JPEG slot.
    const auto needed = pipe.frame_capacity + pipe.frame_size +
                        std::max(pixels / 2U, kStillJpegMinimum) + kStillHeadroom;
    const auto free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const auto largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    AddLog(LogLevel::info, "CAM: still needs %u bytes of PSRAM, %u free, largest %u",
        unsigned(needed), unsigned(free), unsigned(largest));
    if (free < needed || largest < pipe.frame_capacity)
        return fail("still memory check", ESP_ERR_NO_MEM, camera::Result::no_memory);
    pipe.frames[0] = static_cast<std::uint8_t*>(
        heap_caps_aligned_calloc(64, 1, pipe.frame_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!pipe.frames[0])
        return fail("still buffer allocation", ESP_ERR_NO_MEM, camera::Result::no_memory);
    (void)esp_cache_msync(pipe.frames[0], pipe.frame_capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    pipe.frame_count = 1;
    still = Still::idle;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < pipe.frame_size + kBackupSlack)
        return fail("still memory check", ESP_ERR_NO_MEM, camera::Result::no_memory);

    esp_cam_ctlr_csi_config_t csi{};
    csi.ctlr_id = 0;
    csi.h_res = mode.width;
    csi.v_res = mode.height;
    csi.data_lane_num = mode.lanes;
    csi.lane_bit_rate_mbps = mode.lane_mbps;
    csi.input_data_color_type = detail::silicon::csi_input(mode.format);
    csi.output_data_color_type = detail::silicon::csi_input(mode.format);
    csi.queue_items = 1;
    auto error = esp_cam_new_csi_ctlr(&csi, &pipe.csi);
    if (error != ESP_OK)
        return fail("CSI controller", error, camera::Result::failed);
    std::size_t length{};
    error = esp_cam_ctlr_get_frame_buffer_len(pipe.csi, &length);
    if (error == ESP_OK && length != pipe.frame_size)
        error = ESP_ERR_INVALID_SIZE; // a layout the converter does not know
    if (error == ESP_OK)
        error = enable_csi();
    if (error != ESP_OK)
        return fail("CSI enable", error, camera::Result::failed);

    // The controller needs an ISP processor even when bypassed; it is created, never enabled.
    esp_isp_processor_cfg_t isp{};
    isp.clk_hz = kIspClockHz;
    isp.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp.input_data_color_type = detail::silicon::isp_input(mode.format);
    isp.output_data_color_type = isp.input_data_color_type;
    isp.h_res = mode.width;
    isp.v_res = mode.height;
    isp.flags.bypass_isp = true;
    if (mode.format == 0U && mode.width > kIspMaxWidth) {
        // The same 32-bit words per line, declared as RAW12 (see kStillMaxRaw8Width).
        isp.input_data_color_type = isp.output_data_color_type = ISP_COLOR_RAW12;
        isp.h_res = (mode.width + 3U) / 4U * 8U / 3U;
    }
    error = esp_isp_new_processor(&isp, &pipe.isp);
    if (error != ESP_OK)
        return fail("ISP", error, camera::Result::failed);
    if (!detail::jpeg_open(
            mode, JPEG_ENCODE_IN_FORMAT_RGB565, JPEG_DOWN_SAMPLING_YUV422, pipe.quality, 1))
        return fail("JPEG encoder", ESP_ERR_NO_MEM, camera::Result::no_memory);
    pipe.jpeg = true;
    pipe.ccm = detail::isp_ccm(isp_json, mode.name.data());
    return camera::Result::success;
}
#endif

camera::Result build(const Mode& mode, const Session session, const std::string_view isp_json) {
#if USE_CSI_STILL
    if (session == Session::still)
        return build_still(mode, isp_json);
#endif
    const bool yuv420 = detail::silicon::yuv420(session);
    pipe.frame_size = detail::silicon::frame_bytes(mode, yuv420);
    pipe.frame_capacity = pipe.frame_size;
    pipe.frame_count = 0;
    const auto allocate_frame = [] {
        auto*& frame = pipe.frames[pipe.frame_count];
        frame = static_cast<std::uint8_t*>(
            heap_caps_aligned_calloc(64, 1, pipe.frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!frame)
            return false;
        (void)esp_cache_msync(frame, pipe.frame_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ++pipe.frame_count;
        return true;
    };
    if (!allocate_frame() || !allocate_frame())
        return fail("frame buffer allocation", ESP_ERR_NO_MEM, camera::Result::no_memory);
    // The controller's backup frame needs a block of its own: hold one while the third buffer,
    // which only keeps the encoded frame apart, is allocated, and only when the encoders'
    // buffers, created after the controller, still fit beside it.
    auto* backup = heap_caps_malloc(pipe.frame_size + kBackupSlack, MALLOC_CAP_SPIRAM);
    if (!backup) {
        AddLog(LogLevel::error, "CAM: no %u bytes left for the CSI backup frame, largest %u",
            unsigned(pipe.frame_size),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        return fail("CSI memory check", ESP_ERR_NO_MEM, camera::Result::no_memory);
    }
    const auto pixels = static_cast<std::size_t>(mode.width) * mode.height;
    const std::size_t encoders = (detail::silicon::jpeg(session) ? 3U * pixels / 2U : 0U) +
                                 (session != Session::mjpeg ? 2U * pixels * 3U / 2U : 0U);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= pipe.frame_size + encoders + kStreamHeadroom)
        (void)allocate_frame();
    heap_caps_free(backup);
    write_index = 0;
    ready_index = -1;
    locked_index = -1;

    esp_cam_ctlr_csi_config_t csi{};
    csi.ctlr_id = 0;
    csi.h_res = mode.width;
    csi.v_res = mode.height;
    csi.data_lane_num = mode.lanes;
    csi.lane_bit_rate_mbps = mode.lane_mbps;
    csi.input_data_color_type = detail::silicon::csi_input(mode.format);
    csi.output_data_color_type = detail::silicon::csi_output(yuv420);
    csi.queue_items = 1;
    auto error = esp_cam_new_csi_ctlr(&csi, &pipe.csi);
    if (error != ESP_OK)
        return fail("CSI controller", error, camera::Result::failed);
    error = enable_csi();
    if (error != ESP_OK)
        return fail("CSI enable", error, camera::Result::failed);

    esp_isp_processor_cfg_t isp{};
    isp.clk_hz = kIspClockHz;
    isp.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp.input_data_color_type = detail::silicon::isp_input(mode.format);
    isp.output_data_color_type = detail::silicon::isp_output(yuv420);
    isp.h_res = mode.width;
    isp.v_res = mode.height;
    isp.bayer_order = static_cast<color_raw_element_order_t>(mode.bayer & 3U);
    error = esp_isp_new_processor(&isp, &pipe.isp);
    if (error == ESP_OK)
        error = esp_isp_enable(pipe.isp);
    if (error != ESP_OK)
        return fail("ISP", error, camera::Result::failed);
    detail::isp_apply(pipe.isp, isp_json, mode.name.data(), mode.width, mode.height);

    pipe.jpeg = detail::silicon::jpeg(session);
    if (pipe.jpeg && !detail::jpeg_open(mode, detail::silicon::jpeg_input(yuv420),
                         detail::silicon::jpeg_sampling(yuv420), pipe.quality, 3))
        return fail("JPEG encoder", ESP_ERR_NO_MEM, camera::Result::no_memory);
    if (session != Session::mjpeg && !detail::h264_open(pipe.quality))
        return fail("H.264 encoder", ESP_FAIL, camera::Result::failed);
    return camera::Result::success;
}
} // namespace

const Mode& detail::mode() {
    return pipe.mode;
}

bool network_available() {
    // Sockets need the lwIP stack, which only a networked build starts.
    return USE_WIFI != 0;
}

camera::Result prepare() {
    if (pipe.ldo)
        return camera::Result::success;
    esp_ldo_channel_config_t ldo{};
    ldo.chan_id = kMipiPhyLdo;
    ldo.voltage_mv = kMipiPhyMillivolts;
    const auto error = esp_ldo_acquire_channel(&ldo, &pipe.ldo);
    if (error != ESP_OK) {
        pipe.ldo = nullptr;
        AddLog(LogLevel::error, "CAM: MIPI PHY LDO failed (0x%x)", static_cast<unsigned>(error));
        return camera::Result::failed;
    }
    return camera::Result::success;
}

camera::Result start(const Mode& mode, const Session session, const std::uint8_t quality,
    const std::string_view isp_json) {
    if (pipe.csi)
        return camera::Result::busy;
    if (session == Session::none || mode.width < 16U || mode.height < 16U || mode.lanes == 0U ||
        mode.lane_mbps == 0U || mode.format > 2U)
        return camera::Result::invalid_argument;
#if !USE_CSI_STILL
    if (session == Session::still)
        return camera::Result::unavailable;
#endif
    if (session == Session::still && mode.format > 1U)
        return camera::Result::invalid_argument; // the converter reads RAW8 and RAW10
    auto width = mode.width, height = mode.height;
    fit_window(session, mode.format, width, height);
    if (width != mode.width || height != mode.height) {
        AddLog(LogLevel::error, "CAM: %ux%u does not fit the ISP and encoder, %ux%u would",
            unsigned(mode.width), unsigned(mode.height), unsigned(width), unsigned(height));
        return camera::Result::invalid_argument;
    }
    if (session == Session::mjpeg && !detail::silicon::kJpeg) {
        AddLog(LogLevel::error, "CAM: P4 before rev 3 has no CSI JPEG on IDF 6; use IDF 5.5");
        return camera::Result::unavailable;
    }
    if (session != Session::mjpeg && session != Session::still && !network_available()) {
        AddLog(LogLevel::error, "CAM: H.264 and WebRTC sessions need a network");
        return camera::Result::unavailable;
    }
    if (prepare() != camera::Result::success)
        return camera::Result::failed;
    pipe.mode = mode;
    pipe.session = session;
    pipe.quality = std::clamp<std::uint8_t>(quality, 1, 100);
    detail::stats = {};
    const auto built = build(mode, session, isp_json);
    if (built != camera::Result::success)
        return built;
    if (session == Session::h264 && !detail::h264_net_start())
        return fail("RTSP/WebSocket servers", ESP_FAIL, camera::Result::failed);
    if (session == Session::webrtc && !detail::webrtc_start())
        return fail("WebRTC", ESP_FAIL, camera::Result::failed);
    stopping.store(false);
    if (xTaskCreatePinnedToCore(processing_task, "camera", kTaskStack, nullptr, kTaskPriority,
            &pipe.task, 1) != pdPASS) {
        pipe.task = nullptr;
        return fail("processing task", ESP_ERR_NO_MEM, camera::Result::no_memory);
    }
    running.store(true);
    pipe.started_ms = now_ms();
    const auto error = esp_cam_ctlr_start(pipe.csi);
    if (error != ESP_OK)
        return fail("CSI start", error, camera::Result::failed);
    AddLog(LogLevel::info, "CAM: CSI %ux%u %s, %u frame buffers%s", unsigned(mode.width),
        unsigned(mode.height),
        session == Session::still          ? (mode.format == 0U ? "RAW8 still" : "RAW10 still")
        : detail::silicon::yuv420(session) ? "YUV420"
                                           : "YUV422",
        unsigned(pipe.frame_count), pipe.jpeg ? ", JPEG" : "");
    return camera::Result::success;
}

bool pause() {
    if (!pipe.csi || !running.load())
        return false;
    running.store(false);
    // Let the frame in flight end, so the controller stops between frames.
    vTaskDelay(pdMS_TO_TICKS(1000U / std::max<std::uint8_t>(pipe.mode.fps, 1) + 10U));
    (void)esp_cam_ctlr_stop(pipe.csi);
    return true;
}

bool resume() {
    if (!pipe.csi || running.load())
        return false;
    portENTER_CRITICAL(&lock);
    ready_index = -1;
    if (still == Still::filling) // the stop cut that frame; the task asks again
        still = Still::idle;
    portEXIT_CRITICAL(&lock);
    running.store(true);
    pipe.started_ms = now_ms();
    if (esp_cam_ctlr_start(pipe.csi) != ESP_OK) {
        running.store(false);
        return false;
    }
    if (pipe.session == Session::h264 || pipe.session == Session::webrtc)
        detail::h264_request_idr();
    return true;
}

void stop() {
    running.store(false);
    stopping.store(true);
    if (pipe.task) {
        xTaskNotifyGive(pipe.task);
        for (int i = 0; i < 50 && pipe.task; ++i)
            vTaskDelay(pdMS_TO_TICKS(25));
        if (pipe.task) {
            AddLog(LogLevel::error, "CAM: processing task did not exit");
            vTaskDelete(pipe.task);
            pipe.task = nullptr;
        }
    }
    detail::webrtc_stop();
    detail::h264_net_stop();
    detail::isp_release();
    detail::h264_close();
    detail::jpeg_close();
    pipe.jpeg = false;
    if (pipe.csi) {
        (void)esp_cam_ctlr_stop(pipe.csi);
        (void)esp_cam_ctlr_disable(pipe.csi);
        (void)esp_cam_ctlr_del(pipe.csi);
        pipe.csi = nullptr;
    }
    if (pipe.isp) {
        if (pipe.session != Session::still) // a bypassed processor is never enabled
            (void)esp_isp_disable(pipe.isp);
        (void)esp_isp_del_processor(pipe.isp);
        pipe.isp = nullptr;
    }
    for (auto& frame : pipe.frames) {
        heap_caps_free(frame);
        frame = nullptr;
    }
    pipe.frame_count = 0;
    if (pipe.ldo) {
        (void)esp_ldo_release_channel(pipe.ldo);
        pipe.ldo = nullptr;
    }
    pipe.session = Session::none;
}

void poll_awb() {
    if (running.load() && pipe.isp && pipe.session != Session::still)
        detail::isp_awb(pipe.isp);
}

Info info() {
    Info result{};
    result.session = pipe.session;
    result.running = running.load();
    const auto& s = detail::stats;
    result.frames_captured = s.frames_captured;
    result.frames_processed = s.frames_processed;
    result.frames_unsent = s.frames_unsent;
    result.jpeg_errors = s.jpeg_errors;
    result.jpeg_resets = s.jpeg_resets;
    result.bytes_sent = s.bytes_sent;
    result.fps = s.fps;
    result.last_frame_ms = s.last_frame_ms;
    result.encode_us = s.encode_us;
    result.last_size = s.last_size;
    result.min_size = s.min_size;
    result.max_size = s.max_size;
    result.convert_us = s.convert_us;
    result.motion = pipe.session == Session::mjpeg ? 0U : detail::h264_motion();
    const std::uint8_t* sps{};
    const std::uint8_t* pps{};
    std::size_t sps_size{}, pps_size{};
    result.sps_pps = detail::h264_parameter_sets(sps, sps_size, pps, pps_size);
    result.buffers = static_cast<std::uint8_t>(pipe.frame_count);
    return result;
}

} // namespace newmota::platform::camera_csi
