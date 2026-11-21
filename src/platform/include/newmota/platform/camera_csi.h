/*
  camera_csi.h - MIPI-CSI camera pipeline and stream sessions for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi.h
 * @brief Ports Tasmota's ESP32-P4 CSI camera: CSI controller, ISP, JPEG and H.264 encoders.
 *
 * The sensor is configured by a Berry driver before start(). One processing task encodes
 * frames for the selected session: JPEG for the port-81 MJPEG endpoint (`camera.h`
 * take/give), H.264 for RTSP on 554 and WebSocket on 82, or H.264 plus audio over WebRTC
 * on UDP 5005. All calls except the audio hand-off belong to the application task.
 */
#pragma once
#include "newmota/platform/camera.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::platform::camera_csi {

/** @brief Tasmota's WcSession numbers; `still` needs USE_CSI_STILL. */
enum class Session : std::uint8_t { none = 0, mjpeg = 1, h264 = 2, webrtc = 3, still = 4 };

/** @brief The largest frame the P4's ISP processes. */
inline constexpr std::uint16_t kIspMaxWidth = 1920;
inline constexpr std::uint16_t kIspMaxHeight = 1080;

/**
 * @brief The widest RAW8 line of the still session.
 *
 * The bypassed ISP still checks its width against 1920 pixels before it counts the line in
 * 32-bit words, so a wider RAW8 line is declared as the RAW12 width with the same word count:
 * 720 words, 2880 pixels. RAW10 has no such stand-in and stops at 1920.
 */
inline constexpr std::uint16_t kStillMaxRaw8Width = kIspMaxWidth * 12U / 8U;

/**
 * @brief Shrinks a sensor window to the largest centred crop @p session can take.
 *
 * Each axis is cut to the limit on its own: this is a sensor crop, not a scaler, so
 * 3280x2464 becomes 1920x1080 rather than an untested 1440x1080. H.264 reads whole 16x16
 * macroblocks with the frame's own stride, so its sessions round both sides down to 16;
 * MJPEG keeps the sensors' 8x2 window alignment. The still session bypasses the ISP: only
 * the line width is bound, and RAW10's packing wants groups of four pixels.
 * @param format 0 RAW8, 1 RAW10, 2 RAW12; the still session takes RAW8 and RAW10.
 */
constexpr void fit_window(
    const Session session, const std::uint8_t format, std::uint16_t& width, std::uint16_t& height) {
    if (session == Session::still) {
        const std::uint16_t limit = format == 0U ? kStillMaxRaw8Width : kIspMaxWidth;
        width = static_cast<std::uint16_t>((width < limit ? width : limit) & ~0x3U);
        height = static_cast<std::uint16_t>(height & ~0x1U);
        return;
    }
    const bool h264 = session == Session::h264 || session == Session::webrtc;
    width = static_cast<std::uint16_t>(
        (width < kIspMaxWidth ? width : kIspMaxWidth) & (h264 ? ~0xFU : ~0x7U));
    height = static_cast<std::uint16_t>(
        (height < kIspMaxHeight ? height : kIspMaxHeight) & (h264 ? ~0xFU : ~0x1U));
}

/** @brief What the sensor driver reports for the active mode. */
struct Mode final {
    std::uint16_t width{}, height{};
    std::uint8_t format{1}; ///< 0 RAW8, 1 RAW10, 2 RAW12
    std::uint8_t lanes{2};
    std::uint16_t lane_mbps{};
    std::uint8_t fps{30};
    std::uint8_t bayer{}; ///< Colour filter order after flip/mirror: 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB
    std::array<char, 16> name{};
};

/** @brief Counters for WcStatus, kept like Tasmota's. */
struct Info final {
    Session session{Session::none};
    bool running{};
    std::uint32_t frames_captured{}, frames_processed{}, frames_unsent{};
    std::uint32_t jpeg_errors{}, jpeg_resets{}, bytes_sent{};
    std::uint32_t fps{}, last_frame_ms{}, encode_us{}, last_size{}, min_size{}, max_size{};
    std::uint32_t motion{};
    std::uint32_t convert_us{}; ///< Still session: software ISP time of the last still
    bool sps_pps{};
    std::uint8_t buffers{};
};

/** @brief Powers the MIPI PHY (LDO 3 at 2.5 V) before the sensor is configured. */
[[nodiscard]] camera::Result prepare();

/**
 * @brief Builds the pipeline for @p mode and starts the CSI controller and session.
 * @param quality Encoder quality 1..100: JPEG quality, and the H.264 bitrate and QP.
 * @param isp_json Contents of /isp.json, or empty for a bare ISP; read during the call.
 * The sensor starts streaming afterwards. A mode that fit_window() would change is refused.
 */
[[nodiscard]] camera::Result start(
    const Mode& mode, Session session, std::uint8_t quality, std::string_view isp_json);

/** @brief Stops the session, its sockets and the pipeline, and releases the PHY supply. */
void stop();

/**
 * @brief Stops capture for a sensor change that keeps the mode, such as a window shift.
 *
 * The buffers, encoders and viewers stay; frames stop until resume(). Stop the sensor's
 * output first.
 * @return false when the camera is not running.
 */
[[nodiscard]] bool pause();

/**
 * @brief Restarts capture after pause() for the same mode; start the sensor afterwards.
 *
 * H.264 viewers get an IDR next.
 * @return false when the controller does not start; stop() then cleans up.
 */
[[nodiscard]] bool resume();

/** @brief Runs the grey-world auto white balance; call at about 4 Hz while running. */
void poll_awb();

/** @brief Copies the counters. */
[[nodiscard]] Info info();

/** @brief Reports whether the session needs a network that this firmware does not have. */
[[nodiscard]] bool network_available();

/**
 * @brief Answers a WebRTC SDP offer from the port-81 endpoint or a Matter controller.
 * @param local_ip IPv4 address of the interface the offer arrived on, network order; 0 takes
 *        the default interface's, for an offer that came over Matter.
 * @return The answer length, or 0 when the session is not WebRTC or the offer is invalid.
 */
[[nodiscard]] std::size_t webrtc_answer(const char* offer, std::size_t offer_size,
    std::uint32_t local_ip, char* answer, std::size_t capacity);

/**
 * @brief Makes an SDP offer for a Matter controller, with setup:actpass.
 * @param local_ip As for webrtc_answer.
 * @return The offer length, or 0 outside a WebRTC session.
 */
[[nodiscard]] std::size_t webrtc_offer(std::uint32_t local_ip, char* offer, std::size_t capacity);

/**
 * @brief Takes the answer to the last offer.
 * @return false without a pending offer, or when the answer wants this side as the DTLS client.
 */
[[nodiscard]] bool webrtc_accept_answer(const char* answer, std::size_t answer_size);

/** @brief Reports whether the WebRTC peer completed DTLS and takes media. */
[[nodiscard]] bool webrtc_streaming();

/** @brief Payload type and RTP clock step of the WebRTC audio track. */
struct AudioCodec final {
    std::uint8_t payload_type;
    std::uint16_t samples_per_packet; ///< RTP timestamp step per 20 ms packet
};

/** @brief Returns the audio codec the SDP answer offers. */
[[nodiscard]] AudioCodec webrtc_audio_codec();

/**
 * @brief Sends one encoded 20 ms audio packet; safe from an audio worker task.
 * @return false while the peer is not streaming.
 */
bool webrtc_send_audio(const std::uint8_t* payload, std::size_t size);

/** @brief Returns the session's viewer page for the port-81 root. */
[[nodiscard]] const char* page();

} // namespace newmota::platform::camera_csi
