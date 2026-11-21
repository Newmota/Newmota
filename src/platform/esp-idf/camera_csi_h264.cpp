/*
  camera_csi_h264.cpp - ESP32-P4 hardware H.264 encoder of the CSI camera for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_h264.cpp
 * @brief Ported from Tasmota's xdrv_81_2_esp32_webcam_CSI_h264.ino: encoder set-up, the
 * SPS/PPS probe frame and Tasmota's motion value. The vendored esp_h264 selects the
 * register map of the P4 revision (hw_ver1 or hw_ver3); the input is the hardware's
 * O_UYY_E_VYY YUV420 layout on both.
 */
#include "camera_csi_internal.h"

#include "newmota/log.h"

extern "C" { // the port header has no C++ guard
#include "esp_h264_alloc.h"
}
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace newmota::platform::camera_csi::detail {
namespace {
struct Encoder final {
    esp_h264_enc_handle_t handle{};
    std::uint8_t* out{};
    std::uint32_t out_size{};
    std::uint32_t in_size{};
    std::uint8_t sps[512]{}, pps[256]{};
    std::size_t sps_size{}, pps_size{};
    std::uint32_t motion{};
};

Encoder enc{};
std::atomic<bool> idr_requested{false};

/** @brief Captures SPS (7) and PPS (8) from one encoded probe frame. */
void capture_parameter_sets(const std::uint8_t* data, const std::size_t size) {
    for_each_nal(data, size, [](const std::uint8_t* nal, const std::size_t length) {
        const auto type = nal[0] & 0x1FU;
        if (type == 7U && length <= sizeof(enc.sps)) {
            std::memcpy(enc.sps, nal, length);
            enc.sps_size = length;
        } else if (type == 8U && length <= sizeof(enc.pps)) {
            std::memcpy(enc.pps, nal, length);
            enc.pps_size = length;
        }
    });
}

} // namespace

bool h264_open(const std::uint8_t quality) {
    const auto& mode = detail::mode();
    // start() takes only 16x16-aligned frames: a cut width would break the row stride.
    const auto width = mode.width;
    const auto height = mode.height;
    // Quality 100 is 6 Mbit/s and QP 25, 10 is 600 kbit/s; subject to tuning.
    const std::uint8_t q = std::clamp<std::uint8_t>(quality, 5, 100);
    esp_h264_enc_cfg_hw_t cfg{};
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY; // the hardware's YUV420 layout
    cfg.gop = 30;
    cfg.fps = mode.fps ? mode.fps : 30;
    cfg.res.width = width;
    cfg.res.height = height;
    cfg.rc.bitrate = std::uint32_t(q) * 60000U;
    cfg.rc.qp_min = std::clamp<std::uint8_t>(static_cast<std::uint8_t>(50 - q / 4), 15, 25);
    cfg.rc.qp_max = 35;
    enc = {};
    std::uint32_t size = static_cast<std::uint32_t>(std::uint32_t(width) * height * 3U / 2U);
    enc.in_size = size;
    enc.out = static_cast<std::uint8_t*>(
        esp_h264_aligned_calloc(16, 1, size, &enc.out_size, ESP_H264_MEM_SPIRAM));
    if (!enc.out)
        return false;
    if (esp_h264_enc_hw_new(&cfg, &enc.handle) != ESP_H264_ERR_OK) {
        enc.handle = nullptr;
        h264_close();
        return false;
    }
    if (esp_h264_enc_open(enc.handle) != ESP_H264_ERR_OK) {
        h264_close();
        return false;
    }
    // The encoder does not repeat SPS/PPS in every frame; WebRTC's SDP needs them once.
    auto* probe = static_cast<std::uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (probe) {
        std::memset(probe, 128, size);
        esp_h264_enc_in_frame_t in{};
        in.raw_data.buffer = probe;
        in.raw_data.len = size;
        esp_h264_enc_out_frame_t out{};
        out.raw_data.buffer = enc.out;
        out.raw_data.len = enc.out_size;
        if (esp_h264_enc_process(enc.handle, &in, &out) == ESP_H264_ERR_OK && out.length > 0U)
            capture_parameter_sets(enc.out, out.length);
        heap_caps_free(probe);
    }
    if (enc.sps_size == 0U || enc.pps_size == 0U)
        AddLog(LogLevel::error, "H264: no SPS/PPS from the probe frame");
    AddLog(LogLevel::info, "H264: %ux%u, quality %u, %u kbit/s", unsigned(width), unsigned(height),
        unsigned(q), unsigned(cfg.rc.bitrate / 1000U));
    return true;
}

void h264_close() {
    if (enc.handle) {
        (void)esp_h264_enc_close(enc.handle);
        (void)esp_h264_enc_del(enc.handle);
    }
    if (enc.out)
        esp_h264_free(enc.out);
    enc = {};
}

bool h264_encode(const std::uint8_t* yuv, Encoded& out) {
    if (!enc.handle)
        return false;
    if (idr_requested.exchange(false)) {
        esp_h264_enc_param_hw_handle_t parameters{};
        if (esp_h264_enc_hw_get_param_hd(enc.handle, &parameters) == ESP_H264_ERR_OK)
            (void)esp_h264_enc_force_idr(
                reinterpret_cast<esp_h264_enc_param_handle_t>(&parameters->base));
    }
    esp_h264_enc_in_frame_t in{};
    in.raw_data.buffer = const_cast<std::uint8_t*>(yuv);
    in.raw_data.len = enc.in_size;
    esp_h264_enc_out_frame_t result{};
    result.raw_data.buffer = enc.out;
    result.raw_data.len = enc.out_size;
    if (esp_h264_enc_process(enc.handle, &in, &result) != ESP_H264_ERR_OK || result.length == 0U)
        return false;
    out.data = enc.out;
    out.size = result.length;
    out.key =
        result.frame_type == ESP_H264_FRAME_TYPE_IDR || result.frame_type == ESP_H264_FRAME_TYPE_I;
    if (!out.key) // about one unit per 128 changed bytes, smoothed
        enc.motion = (enc.motion * 4U + result.length / 128U) / 5U;
    return true;
}

bool h264_parameter_sets(const std::uint8_t*& sps, std::size_t& sps_size, const std::uint8_t*& pps,
    std::size_t& pps_size) {
    sps = enc.sps;
    sps_size = enc.sps_size;
    pps = enc.pps;
    pps_size = enc.pps_size;
    return enc.sps_size > 0U && enc.pps_size > 0U;
}

std::uint32_t h264_motion() {
    return enc.motion;
}

void h264_request_idr() {
    idr_requested.store(true);
}

} // namespace newmota::platform::camera_csi::detail
