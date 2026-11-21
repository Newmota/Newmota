/*
  camera_csi_silicon.h - ESP32-P4 revision differences of the CSI camera for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_silicon.h
 * @brief One place for what the pixel formats depend on: P4 silicon and IDF version.
 *
 * IDF treats revisions before 3.0 (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3`) as a different
 * chip. Rev 3.x runs one YUV420 pipeline: CSI and ISP emit what both the JPEG and the H.264
 * encoder read, so a JPEG snapshot also works while H.264 or WebRTC streams. Before rev 3
 * the JPEG encoder needs YUV422, as in Tasmota; H.264 needs YUV420, so JPEG exists only in
 * the MJPEG session. IDF 6 names the YUV422 byte orders: the ISP emits UYVY, the JPEG
 * encoder reads YVYU, and only rev 3 can convert in the CSI controller. Old silicon on
 * IDF 6 therefore has no JPEG.
 */
#pragma once
#include "newmota/platform/camera_csi.h"

#include "driver/jpeg_types.h"
#include "esp_cam_ctlr_types.h"
#include "esp_idf_version.h"
#include "hal/cam_ctlr_types.h"
#include "hal/isp_types.h"
#include "hal/jpeg_types.h"
#include "sdkconfig.h"

#include <cstddef>
#include <cstdint>

namespace newmota::platform::camera_csi::detail::silicon {

#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
inline constexpr bool kRevision3 = false;
#else
inline constexpr bool kRevision3 = true;
#endif
inline constexpr bool kIdf6 = ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0);

/** @brief Whether this chip and SDK can encode JPEG from the CSI pipeline at all. */
inline constexpr bool kJpeg = kRevision3 || !kIdf6;

/** @brief Whether @p session's frames are YUV420 (else YUV422). */
constexpr bool yuv420(const Session session) {
    return kRevision3 || session != Session::mjpeg;
}

/** @brief Whether JPEG frames exist in @p session, for MJPEG and snapshots. */
constexpr bool jpeg(const Session session) {
    return kJpeg && (session == Session::mjpeg || kRevision3);
}

constexpr std::size_t frame_bytes(const Mode& mode, const bool yuv420) {
    const auto pixels = static_cast<std::size_t>(mode.width) * mode.height;
    return yuv420 ? pixels * 3U / 2U : pixels * 2U;
}

constexpr cam_ctlr_color_t csi_input(const std::uint8_t format) {
    return format == 0 ? CAM_CTLR_COLOR_RAW8
                       : (format == 2 ? CAM_CTLR_COLOR_RAW12 : CAM_CTLR_COLOR_RAW10);
}

constexpr isp_color_t isp_input(const std::uint8_t format) {
    return format == 0 ? ISP_COLOR_RAW8 : (format == 2 ? ISP_COLOR_RAW12 : ISP_COLOR_RAW10);
}

/** @brief The CSI output equals the ISP output, so the controller converts nothing. */
constexpr cam_ctlr_color_t csi_output(const bool yuv420) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    return yuv420 ? CAM_CTLR_COLOR_YUV420 : CAM_CTLR_COLOR_YUV422_UYVY;
#else
    return yuv420 ? CAM_CTLR_COLOR_YUV420 : CAM_CTLR_COLOR_YUV422;
#endif
}

constexpr isp_color_t isp_output(const bool yuv420) {
    return yuv420 ? ISP_COLOR_YUV420 : ISP_COLOR_YUV422;
}

/** @brief YUV420 JPEG input exists on rev 3 only; IDF does not declare it before. */
constexpr jpeg_enc_input_format_t jpeg_input(const bool yuv420) {
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    (void)yuv420;
    return JPEG_ENCODE_IN_FORMAT_YUV422;
#else
    return yuv420 ? JPEG_ENCODE_IN_FORMAT_YUV420 : JPEG_ENCODE_IN_FORMAT_YUV422;
#endif
}

constexpr jpeg_down_sampling_type_t jpeg_sampling(const bool yuv420) {
    return yuv420 ? JPEG_DOWN_SAMPLING_YUV420 : JPEG_DOWN_SAMPLING_YUV422;
}

} // namespace newmota::platform::camera_csi::detail::silicon
