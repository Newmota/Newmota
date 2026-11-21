/*
  camera_csi_internal.h - Shared state of the ESP-IDF CSI camera files for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_internal.h
 * @brief Seams between the pipeline, ISP, H.264 and WebRTC parts of the CSI camera.
 */
#pragma once
#include "newmota/platform/camera_csi.h"

#include "driver/isp.h"
#include "driver/jpeg_types.h"
#include "hal/jpeg_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::platform::camera_csi::detail {

/** @brief Counters the processing task updates and Info reports. */
struct Stats final {
    std::uint32_t frames_captured, frames_processed, frames_unsent;
    std::uint32_t jpeg_errors, jpeg_resets, bytes_sent;
    std::uint32_t fps, last_frame_ms, encode_us, last_size, min_size, max_size;
    std::uint32_t convert_us;
};

extern Stats stats;

/** @brief Returns the active mode; valid while the pipeline runs. */
const Mode& mode();

/** @brief Applies the sensor's section of /isp.json; an empty text leaves a bare ISP. */
void isp_apply(
    isp_proc_handle_t isp, std::string_view json, const char* sensor, int width, int height);
/** @brief Corrects the colour matrix from the white-patch statistics. */
void isp_awb(isp_proc_handle_t isp);
/** @brief Removes the AWB controller; call before the ISP processor is deleted. */
void isp_release();

/** @brief A colour matrix in Q8, row by row: red, green and blue out of camera RGB. */
using Ccm = std::array<std::int16_t, 9>;
inline constexpr Ccm kIdentityCcm{256, 0, 0, 0, 256, 0, 0, 0, 256};
/** @brief Reads the stream's 6500 K colour matrix for stills; identity without all 9 numbers. */
Ccm isp_ccm(std::string_view json, const char* sensor);

/**
 * @brief Creates the JPEG engine and its frame ring for @p mode (camera_csi_jpeg.cpp).
 * @param slots Ring slots of half a pixel each: three for streams, one for stills.
 */
bool jpeg_open(const Mode& mode, jpeg_enc_input_format_t input, jpeg_down_sampling_type_t sampling,
    std::uint8_t quality, std::size_t slots);
/** @brief Deletes the engine; slots a reader still holds are freed by its give(). */
void jpeg_close();
/** @brief Reports whether a reader asked for a frame in the last two seconds. */
bool jpeg_wanted();
/** @brief Reports whether a reader waits for a frame right now. */
bool jpeg_waiting();
/** @brief Drops ready frames no reader holds, so the next reader waits for a new one. */
void jpeg_drop_ready();
/** @brief Encodes one captured frame into the ring. @return false when it was dropped. */
bool jpeg_encode(const std::uint8_t* frame, std::size_t frame_size);

/** @brief One encoded H.264 access unit in the encoder's output buffer. */
struct Encoded final {
    const std::uint8_t* data;
    std::size_t size;
    bool key;
};

/** @brief Opens the hardware encoder and captures SPS/PPS from a probe frame. */
bool h264_open(std::uint8_t quality);
void h264_close();
/** @brief Encodes one YUV420 frame. */
bool h264_encode(const std::uint8_t* yuv, Encoded& out);
/** @brief The SPS and PPS NAL units without start codes, if captured. */
bool h264_parameter_sets(const std::uint8_t*& sps, std::size_t& sps_size, const std::uint8_t*& pps,
    std::size_t& pps_size);
/** @brief Smoothed P-frame size, Tasmota's motion value. */
std::uint32_t h264_motion();
/** @brief Makes the next encoded frame an IDR, for a viewer that lost the picture; any task. */
void h264_request_idr();

/** @brief Starts RTSP on 554, the WebSocket on 82 and the RTP socket (camera_csi_rtsp.cpp). */
bool h264_net_start();
void h264_net_stop();
/** @brief Sends one access unit to the WebSocket client and, after PLAY, over RTP. */
void h264_net_send(const Encoded& frame);

/** @brief Starts the DTLS/STUN socket on UDP 5005 and its handshake task. */
bool webrtc_start();
void webrtc_stop();
/** @brief Sends one access unit over SRTP once DTLS is done. */
void webrtc_send_video(const Encoded& frame);

/**
 * @brief Turns one RAW8 or packed RAW10 still into RGB565 in place (camera_csi_still.cpp).
 * @param buffer Holds the raw frame and has room for two bytes per pixel.
 * @param ccm Applied to the white-balanced colour. Stream AWB scales the matrix's output rows
 *        instead, so a still's tint can differ slightly from the stream's.
 * @param abort Ends the conversion early when set, for stop().
 * @return false when the frame is too dark to correct or the conversion was aborted.
 */
bool still_convert(
    std::uint8_t* buffer, const Mode& mode, const Ccm& ccm, const std::atomic<bool>& abort);

/** @brief Splits an Annex B stream into NAL units without start codes. */
template <typename Sink> void for_each_nal(const std::uint8_t* data, std::size_t size, Sink sink) {
    std::size_t position{};
    while (position + 3 <= size) {
        std::size_t start{};
        if (data[position] == 0 && data[position + 1] == 0 && data[position + 2] == 1)
            start = position + 3;
        else if (position + 4 <= size && data[position] == 0 && data[position + 1] == 0 &&
                 data[position + 2] == 0 && data[position + 3] == 1)
            start = position + 4;
        else {
            ++position;
            continue;
        }
        std::size_t end = size;
        for (std::size_t i = start; i + 3 <= size; ++i)
            if (data[i] == 0 && data[i + 1] == 0 &&
                (data[i + 2] == 1 || (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1))) {
                end = i;
                break;
            }
        if (end > start)
            sink(data + start, end - start);
        position = end;
    }
}

} // namespace newmota::platform::camera_csi::detail
