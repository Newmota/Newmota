/*
  camera_csi_still.cpp - Software ISP of the CSI camera's still session for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_still.cpp
 * @brief Ported from the still session of Tasmota's xdrv_81_0_esp32_webcam_CSI_isp.ino.
 *
 * The P4's ISP takes at most 1920x1080, so full-sensor stills bypass it and are corrected
 * here. Black and white points come from the 1st and 99th percentile of green, colour from
 * grey-world gains, both from a sample of every eighth 2x2 cell. Each pixel then gets its
 * missing colours by bilinear interpolation, the white balance, the stream's colour matrix
 * from /isp.json and a gamma-like curve.
 *
 * The RGB565 output takes two bytes per pixel and is written backwards in the same buffer.
 * Rows are unpacked into a four-row cache before the output can reach them: when a row pair
 * starts, nothing below the next pair's output has been written, and that is above the raw
 * rows the pair still loads.
 */
#include "camera_csi_internal.h"

#include "newmota/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::platform::camera_csi::detail {
namespace {
constexpr std::uint32_t kSampleStep = 8;
constexpr std::uint32_t kWhiteTarget = 200;        // the 99th percentile, before the tone curve
constexpr std::uint32_t kMaxExposureQ8 = 8U * 256; // at most 8x brighter
constexpr std::uint32_t kMinGainQ8 = 128, kMaxGainQ8 = 1024;
constexpr std::uint32_t kWhite = 255U * 4U; // linear values carry two fraction bits
constexpr std::size_t kCacheRows = 4;

/** @brief Reads the upper 8 bits of one pixel; RAW10 packs four pixels into five bytes. */
inline std::uint8_t sample(const std::uint8_t* raw, const bool raw10, const std::size_t pixel) {
    // The fifth byte of a RAW10 group holds the four low bit pairs, which are dropped.
    return raw10 ? raw[(pixel >> 2U) * 5U + (pixel & 3U)] : raw[pixel];
}

struct Rgb final {
    std::uint8_t r, g, b;
};

/** @brief The colour of the 2x2 cell whose top-left pixels are @p top and @p bottom. */
inline Rgb cell(const std::uint8_t* raw, const bool raw10, const std::size_t top,
    const std::size_t bottom, const std::uint8_t order) {
    const std::uint8_t p00 = sample(raw, raw10, top), p01 = sample(raw, raw10, top + 1U);
    const std::uint8_t p10 = sample(raw, raw10, bottom), p11 = sample(raw, raw10, bottom + 1U);
    switch (order) {
    case 0: // BGGR
        return {p11, static_cast<std::uint8_t>((p01 + p10 + 1U) >> 1U), p00};
    case 1: // GBRG
        return {p10, static_cast<std::uint8_t>((p00 + p11 + 1U) >> 1U), p01};
    case 2: // GRBG
        return {p01, static_cast<std::uint8_t>((p00 + p11 + 1U) >> 1U), p10};
    default: // RGGB
        return {p00, static_cast<std::uint8_t>((p01 + p10 + 1U) >> 1U), p11};
    }
}

/** @brief Black level, gains and the colour matrix, then the curve y = 2x - x²/255. */
struct Tone final {
    std::array<std::array<std::uint16_t, 256>, 3> linear; ///< per channel, 0..kWhite
    std::array<std::uint8_t, kWhite + 1U> curve;
    Ccm ccm;

    void build(const std::uint8_t black, const std::uint32_t exposure_q8,
        const std::array<std::uint32_t, 3>& gains_q8, const Ccm& matrix) {
        for (std::size_t c = 0; c < linear.size(); ++c) {
            for (std::uint32_t value = 0; value < 256U; ++value) {
                const std::uint32_t corrected = value > black ? value - black : 0U;
                // Clipped before the matrix, so a saturated pixel stays white.
                linear[c][value] = static_cast<std::uint16_t>(std::min<std::uint32_t>(
                    (corrected * gains_q8[c] * exposure_q8 * 4U + 32768U) >> 16U, kWhite));
            }
        }
        for (std::uint32_t x = 0; x <= kWhite; ++x) {
            const std::uint32_t y = x + ((kWhite - x) * x + kWhite / 2U) / kWhite;
            curve[x] = static_cast<std::uint8_t>(std::min<std::uint32_t>((y + 2U) >> 2U, 255U));
        }
        ccm = matrix;
    }

    std::uint16_t pixel(const std::uint8_t r, const std::uint8_t g, const std::uint8_t b) const {
        const std::int32_t lr = linear[0][r], lg = linear[1][g], lb = linear[2][b];
        const auto mix = [&](const std::size_t row) {
            const std::int32_t value =
                (ccm[row] * lr + ccm[row + 1U] * lg + ccm[row + 2U] * lb + 128) >> 8;
            return std::uint32_t{curve[static_cast<std::size_t>(
                std::clamp<std::int32_t>(value, 0, static_cast<std::int32_t>(kWhite)))]};
        };
        return static_cast<std::uint16_t>(
            ((mix(0) & 0xF8U) << 8U) | ((mix(3) & 0xFCU) << 3U) | (mix(6) >> 3U));
    }
};

/**
 * @brief Four unpacked rows, slot = row mod 4, each with one mirrored pixel on either side.
 *
 * Borders mirror two pixels away, so the neighbour beyond an edge has the same colour as the
 * one inside: column -1 reads column 1 and row -1 reads row 1.
 */
class RowCache final {
  public:
    RowCache(const std::uint8_t* raw, const bool raw10, const std::uint32_t width,
        const std::uint32_t height, std::uint8_t* storage)
        : raw_(raw), raw10_(raw10), width_(width), height_(height), storage_(storage) {}

    void load(const std::uint32_t row) {
        std::uint8_t* out = slot(row);
        const std::size_t pixel = static_cast<std::size_t>(row) * width_;
        if (raw10_) {
            const std::uint8_t* in = raw_ + pixel / 4U * 5U;
            for (std::uint32_t x = 0; x < width_; x += 4U, in += 5)
                std::memcpy(out + 1U + x, in, 4U);
        } else {
            std::memcpy(out + 1U, raw_ + pixel, width_);
        }
        out[0] = out[2];
        out[width_ + 1U] = out[width_ - 1U];
    }

    /** @brief The row with column x at index x + 1, mirrored at the top and bottom. */
    const std::uint8_t* at(const std::int32_t row) const {
        const auto last = static_cast<std::int32_t>(height_) - 1;
        const std::int32_t inside = row < 0 ? -row : (row > last ? 2 * last - row : row);
        return slot(static_cast<std::uint32_t>(inside));
    }

  private:
    std::uint8_t* slot(const std::uint32_t row) const {
        return storage_ + static_cast<std::size_t>(row % kCacheRows) * (width_ + 2U);
    }

    const std::uint8_t* raw_;
    bool raw10_;
    std::uint32_t width_, height_;
    std::uint8_t* storage_;
};

/**
 * @brief Bilinear colours of the pixel at index @p i of row @p mid.
 * @tparam RedRow The row holds red sites, so its green sites have red left and right.
 * @tparam Site The pixel is a red site, or with RedRow false a blue one; else green.
 */
template <bool RedRow, bool Site>
inline std::uint16_t demosaic(const std::uint8_t* up, const std::uint8_t* mid,
    const std::uint8_t* down, const std::size_t i, const Tone& tone) {
    const std::uint8_t centre = mid[i];
    if constexpr (Site) {
        const auto cross =
            static_cast<std::uint8_t>((up[i] + down[i] + mid[i - 1U] + mid[i + 1U] + 2U) >> 2U);
        const auto diagonal = static_cast<std::uint8_t>(
            (up[i - 1U] + up[i + 1U] + down[i - 1U] + down[i + 1U] + 2U) >> 2U);
        return RedRow ? tone.pixel(centre, cross, diagonal) : tone.pixel(diagonal, cross, centre);
    } else {
        const auto across = static_cast<std::uint8_t>((mid[i - 1U] + mid[i + 1U] + 1U) >> 1U);
        const auto vertical = static_cast<std::uint8_t>((up[i] + down[i] + 1U) >> 1U);
        return RedRow ? tone.pixel(across, centre, vertical) : tone.pixel(vertical, centre, across);
    }
}

/** @brief Converts row pair @p y, right to left. @tparam RedTop, RedLeft Red's cell corner. */
template <bool RedTop, bool RedLeft>
void convert_pair(const RowCache& rows, const std::int32_t y, const std::uint32_t width,
    std::uint16_t* out, const Tone& tone) {
    const std::uint8_t* above = rows.at(y - 1);
    const std::uint8_t* top = rows.at(y);
    const std::uint8_t* bottom = rows.at(y + 1);
    const std::uint8_t* below = rows.at(y + 2);
    std::uint16_t* out_top = out + static_cast<std::size_t>(y) * width;
    std::uint16_t* out_bottom = out_top + width;
    for (std::int32_t x = static_cast<std::int32_t>(width) - 2; x >= 0; x -= 2) {
        const auto left = static_cast<std::size_t>(x) + 1U, right = left + 1U;
        // Red and blue sit where row and column both match red's corner, or both do not.
        out_bottom[x + 1] = demosaic<!RedTop, RedTop == RedLeft>(top, bottom, below, right, tone);
        out_bottom[x] = demosaic<!RedTop, RedTop != RedLeft>(top, bottom, below, left, tone);
        out_top[x + 1] = demosaic<RedTop, RedTop != RedLeft>(above, top, bottom, right, tone);
        out_top[x] = demosaic<RedTop, RedTop == RedLeft>(above, top, bottom, left, tone);
    }
}

using PairConverter = void (*)(
    const RowCache&, std::int32_t, std::uint32_t, std::uint16_t*, const Tone&);

/** @brief The converter for colour filter order 0 BGGR, 1 GBRG, 2 GRBG, 3 RGGB. */
PairConverter pair_converter(const std::uint8_t order) {
    switch (order) {
    case 0:
        return convert_pair<false, false>;
    case 1:
        return convert_pair<false, true>;
    case 2:
        return convert_pair<true, false>;
    default:
        return convert_pair<true, true>;
    }
}
} // namespace

bool still_convert(
    std::uint8_t* buffer, const Mode& mode, const Ccm& ccm, const std::atomic<bool>& abort) {
    const std::uint32_t width = mode.width, height = mode.height;
    const bool raw10 = mode.format == 1U;
    const std::uint8_t order = mode.bayer & 3U;

    std::uint64_t sum_r{}, sum_g{}, sum_b{};
    std::array<std::uint32_t, 256> green{};
    std::uint32_t samples{};
    for (std::uint32_t y = 0; y + 1U < height; y += kSampleStep) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        for (std::uint32_t x = 0; x + 1U < width; x += kSampleStep) {
            const auto c = cell(buffer, raw10, row + x, row + width + x, order);
            sum_r += c.r;
            sum_g += c.g;
            sum_b += c.b;
            ++green[c.g];
            ++samples;
        }
    }
    if (samples == 0U || sum_r == 0U || sum_g == 0U || sum_b == 0U)
        return false;

    const std::uint32_t low_rank = (samples + 99U) / 100U;
    const std::uint32_t high_rank = (samples * 99U + 99U) / 100U;
    std::uint32_t count{};
    std::uint8_t black{}, white{255};
    bool black_found{};
    for (std::uint32_t value = 0; value < green.size(); ++value) {
        count += green[value];
        if (!black_found && count >= low_rank) {
            black = static_cast<std::uint8_t>(value);
            black_found = true;
        }
        if (count >= high_rank) {
            white = static_cast<std::uint8_t>(value);
            break;
        }
    }
    std::uint32_t exposure = 256;
    if (white > black + 4U) {
        // The 99th percentile at linear 200, which the curve lifts to about 243.
        const std::uint32_t range = white - black;
        exposure = std::clamp<std::uint32_t>(
            (kWhiteTarget * 256U + range / 2U) / range, 256U, kMaxExposureQ8);
    }
    const auto above_black = [black](const std::uint64_t sum, const std::uint32_t n) {
        const auto average = static_cast<std::uint32_t>(sum / n);
        return average > black ? average - black : 1U;
    };
    const std::uint32_t r = above_black(sum_r, samples), g = above_black(sum_g, samples),
                        b = above_black(sum_b, samples);
    const std::uint32_t gain_r =
        std::clamp<std::uint32_t>((g * 256U + r / 2U) / r, kMinGainQ8, kMaxGainQ8);
    const std::uint32_t gain_b =
        std::clamp<std::uint32_t>((g * 256U + b / 2U) / b, kMinGainQ8, kMaxGainQ8);
    AddLog(LogLevel::debug, "CAM: still green %u, range %u..%u, exposure %u/256, gains %u/%u",
        unsigned(sum_g / samples), unsigned(black), unsigned(white), unsigned(exposure),
        unsigned(gain_r), unsigned(gain_b));

    const auto tone = std::unique_ptr<Tone>(new (std::nothrow) Tone);
    const auto storage = std::unique_ptr<std::uint8_t[]>(
        new (std::nothrow) std::uint8_t[kCacheRows * (static_cast<std::size_t>(width) + 2U)]);
    if (!tone || !storage)
        return false;
    tone->build(black, exposure, {gain_r, 256U, gain_b}, ccm);

    RowCache rows{buffer, raw10, width, height, storage.get()};
    const auto convert = pair_converter(order);
    auto* out = reinterpret_cast<std::uint16_t*>(buffer);
    rows.load(height - 1U);
    for (std::int32_t y = static_cast<std::int32_t>(height) - 2; y >= 0; y -= 2) {
        // Rows y + 1 and y + 2 are cached from the pair below.
        rows.load(static_cast<std::uint32_t>(y));
        if (y > 0)
            rows.load(static_cast<std::uint32_t>(y - 1));
        convert(rows, y, width, out, *tone);
        if ((y & 31) == 0) {
            if (abort.load())
                return false;
            vTaskDelay(1); // let the core's other tasks run during the second-long pass
        }
    }
    return true;
}

} // namespace newmota::platform::camera_csi::detail
