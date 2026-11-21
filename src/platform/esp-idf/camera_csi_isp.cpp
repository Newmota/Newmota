/*
  camera_csi_isp.cpp - ESP32-P4 ISP tuning from /isp.json for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_isp.cpp
 * @brief Ported from Tasmota's xdrv_81_0_esp32_webcam_CSI_isp.ino.
 *
 * The file holds one object per sensor name, in the layout of Espressif's IPA JSON:
 * `acc.ccm.table`, `aen.gamma.table`, `aen.sharpen`, `aen.contrast`, `ext`, `adn.bf` and
 * `awb`. Each block picks the entry nearest to normal light (6500 K, luma 50, gain 1).
 * A missing block leaves that ISP stage off; nothing here stops the camera.
 */
#include "camera_csi_internal.h"

#include "newmota_features.h"
#include "newmota/log.h"
#include "support/json.h"

#include "driver/isp_awb.h"
#include "driver/isp_bf.h"
#include "driver/isp_ccm.h"
#include "driver/isp_color.h"
#include "driver/isp_gamma.h"
#include "driver/isp_sharpen.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::platform::camera_csi::detail {
namespace {
using support::JsonKind;
using support::JsonReader;
using support::JsonToken;

constexpr std::size_t kMaxTokens = 768;

struct AwbState final {
    isp_awb_ctlr_t handle{};
    float base_ccm[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float gain_r{1.0F}, gain_b{1.0F};
    float min_r_step{0.034F}, min_b_step{0.034F};
    std::uint32_t min_counted{2000};
};

AwbState awb{};
std::uint32_t gamma_y[16]{};

/** @brief Wraps the reader with the lookups the ISP blocks need. */
class Json final {
  public:
    explicit Json(const JsonReader& reader) : r_(reader) {}

    bool found(const std::size_t index) const { return index < r_.token_count(); }

    bool is(const std::size_t index, const JsonKind kind) const {
        return found(index) && r_.token(index).kind == kind;
    }

    std::size_t member(const std::size_t object, const char* name) const {
        return is(object, JsonKind::object) ? r_.member(object, name) : r_.token_count();
    }

    std::size_t path(
        std::size_t object, const char* a, const char* b = nullptr, const char* c = nullptr) const {
        object = member(object, a);
        if (b)
            object = member(object, b);
        if (c)
            object = member(object, c);
        return object;
    }

    float number(const std::size_t index, const float fallback) const {
        if (!is(index, JsonKind::number))
            return fallback;
        const auto text = r_.text(index);
        char buffer[32]{};
        const auto size = text.size() < sizeof(buffer) - 1U ? text.size() : sizeof(buffer) - 1U;
        std::memcpy(buffer, text.data(), size);
        return std::strtof(buffer, nullptr);
    }

    float number(const std::size_t object, const char* name, const float fallback) const {
        return number(member(object, name), fallback);
    }

    int integer(const std::size_t object, const char* name, const int fallback) const {
        return static_cast<int>(std::lround(number(member(object, name), float(fallback))));
    }

    /** @brief Calls @p sink(index, value) for the numbers of an array, at most @p limit. */
    template <typename Sink>
    void numbers(const std::size_t array, const std::size_t limit, Sink sink) const {
        if (!is(array, JsonKind::array))
            return;
        std::size_t i{};
        for (auto item = r_.first(array); found(item) && i < limit; item = r_.next(array, item))
            sink(i++, number(item, 0.0F));
    }

    /** @brief Finds the array entry whose @p key is nearest to @p target. */
    std::size_t nearest(const std::size_t array, const char* key, const int target) const {
        std::size_t best = r_.token_count();
        int best_distance = INT_MAX;
        if (!is(array, JsonKind::array))
            return best;
        for (auto item = r_.first(array); found(item); item = r_.next(array, item)) {
            const auto value = member(item, key);
            if (!is(value, JsonKind::number))
                continue;
            const int distance = std::abs(int(std::lround(number(value, 0.0F))) - target);
            if (distance < best_distance) {
                best_distance = distance;
                best = item;
            }
        }
        return best;
    }

  private:
    const JsonReader& r_;
};

/**
 * @brief Parses @p json and calls @p use(j, section) with the section named @p sensor.
 * @param user What stays untuned without that section, for the log.
 */
template <typename Use>
void with_section(const std::string_view json, const char* sensor, const char* user, Use use) {
    if (json.empty()) {
        AddLog(LogLevel::info, "CAM: no /isp.json, %s untuned", user);
        return;
    }
    const auto tokens = std::unique_ptr<JsonToken[]>(new (std::nothrow) JsonToken[kMaxTokens]);
    if (!tokens)
        return;
    JsonReader reader{json, tokens.get(), kMaxTokens};
    std::size_t root{};
    if (!reader.parse(root) || reader.token(root).kind != JsonKind::object) {
        AddLog(LogLevel::error, "CAM: /isp.json is not a valid object");
        return;
    }
    const Json j{reader};
    const auto section = j.member(root, sensor);
    if (!j.is(section, JsonKind::object)) {
        AddLog(LogLevel::info, "CAM: /isp.json has no section for %s, %s untuned", sensor, user);
        return;
    }
    use(j, section);
}

/** @brief Reads the colour matrix of the table entry nearest to 6500 K; it needs 9 numbers. */
bool read_ccm(const Json& j, const std::size_t sensor, float (&matrix)[3][3]) {
    const auto best = j.nearest(j.path(sensor, "acc", "ccm", "table"), "color_temp", 6500);
    float values[3][3]{};
    std::size_t count{};
    j.numbers(j.member(best, "matrix"), 9, [&](const std::size_t i, const float value) {
        values[i / 3][i % 3] = value;
        count = i + 1U;
    });
    if (count != 9U)
        return false;
    std::memcpy(matrix, values, sizeof(values));
    return true;
}

void apply_ccm(isp_proc_handle_t isp, const Json& j, const std::size_t sensor) {
    esp_isp_ccm_config_t cfg{};
    if (!read_ccm(j, sensor, cfg.matrix))
        return;
    std::memcpy(awb.base_ccm, cfg.matrix, sizeof(awb.base_ccm));
    if (esp_isp_ccm_configure(isp, &cfg) == ESP_OK)
        (void)esp_isp_ccm_enable(isp);
}

/** @brief Interpolates the 16-point curve for the ISP's gamma helper. */
uint32_t gamma_curve(const uint32_t x) {
    const uint32_t segment = (x * 15U) / 255U;
    if (segment >= 15U)
        return gamma_y[15];
    const uint32_t x0 = segment * 255U / 15U, x1 = (segment + 1U) * 255U / 15U;
    const uint32_t y0 = gamma_y[segment], y1 = gamma_y[segment + 1U];
    return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y0;
}

void apply_gamma(isp_proc_handle_t isp, const Json& j, const std::size_t sensor) {
    const auto best = j.nearest(j.path(sensor, "aen", "gamma", "table"), "luma", 50);
    const auto points = j.member(best, "y");
    if (!j.is(points, JsonKind::array))
        return;
    j.numbers(points, 16, [](const std::size_t i, const float value) {
        gamma_y[i] = static_cast<std::uint32_t>(value);
    });
    isp_gamma_curve_points_t curve{};
    if (esp_isp_gamma_fill_curve_points(gamma_curve, &curve) != ESP_OK)
        return;
    for (const auto component : {COLOR_COMPONENT_R, COLOR_COMPONENT_G, COLOR_COMPONENT_B})
        (void)esp_isp_gamma_configure(isp, component, &curve);
    (void)esp_isp_gamma_enable(isp);
}

void apply_sharpen(isp_proc_handle_t isp, const Json& j, const std::size_t sensor) {
    const auto best = j.nearest(j.path(sensor, "aen", "sharpen"), "gain", 1);
    const auto param = j.member(best, "param");
    if (!j.is(param, JsonKind::object))
        return;
    esp_isp_sharpen_config_t cfg{};
    cfg.h_thresh = static_cast<std::uint8_t>(j.integer(param, "h_thresh", 255));
    cfg.l_thresh = static_cast<std::uint8_t>(j.integer(param, "l_thresh", 128));
    const float h = j.number(param, "h_coeff", 1.0F), m = j.number(param, "m_coeff", 1.0F);
    cfg.h_freq_coeff.integer = static_cast<std::uint8_t>(h);
    cfg.h_freq_coeff.decimal = static_cast<std::uint8_t>((h - std::floor(h)) * 256.0F);
    cfg.m_freq_coeff.integer = static_cast<std::uint8_t>(m);
    cfg.m_freq_coeff.decimal = static_cast<std::uint8_t>((m - std::floor(m)) * 256.0F);
    j.numbers(j.member(param, "matrix"), 9, [&](const std::size_t i, const float value) {
        cfg.sharpen_template[i / 3][i % 3] = static_cast<std::uint8_t>(value);
    });
    cfg.padding_mode = ISP_SHARPEN_EDGE_PADDING_MODE_SRND_DATA;
    if (esp_isp_sharpen_configure(isp, &cfg) == ESP_OK)
        (void)esp_isp_sharpen_enable(isp);
}

void apply_color(isp_proc_handle_t isp, const Json& j, const std::size_t sensor) {
    esp_isp_color_config_t cfg{};
    cfg.color_contrast.integer = 1;
    const auto best = j.nearest(j.path(sensor, "aen", "contrast"), "gain", 1);
    if (j.found(best)) {
        const int value = j.integer(best, "value", 128);
        cfg.color_contrast.integer = static_cast<std::uint8_t>(value >> 7);
        cfg.color_contrast.decimal = static_cast<std::uint8_t>((value & 0x7F) << 1);
    }
    const auto ext = j.member(sensor, "ext");
    cfg.color_hue = static_cast<std::uint32_t>(j.integer(ext, "hue", 0));
    cfg.color_brightness = j.integer(ext, "brightness", 0);
    // Tasmota leaves saturation at 1 until a colour temperature estimate exists.
    cfg.color_saturation.integer = 1;
    if (esp_isp_color_configure(isp, &cfg) == ESP_OK)
        (void)esp_isp_color_enable(isp);
}

void apply_bf(isp_proc_handle_t isp, const Json& j, const std::size_t sensor) {
    const auto best = j.nearest(j.path(sensor, "adn", "bf"), "gain", 1);
    const auto param = j.member(best, "param");
    if (!j.is(param, JsonKind::object))
        return;
    esp_isp_bf_config_t cfg{};
    cfg.denoising_level = static_cast<std::uint8_t>(j.integer(param, "level", 2));
    j.numbers(j.member(param, "matrix"), 9, [&](const std::size_t i, const float value) {
        cfg.bf_template[i / 3][i % 3] = static_cast<std::uint8_t>(value);
    });
    cfg.padding_mode = ISP_BF_EDGE_PADDING_MODE_SRND_DATA;
    if (esp_isp_bf_configure(isp, &cfg) == ESP_OK)
        (void)esp_isp_bf_enable(isp);
}

void init_awb(isp_proc_handle_t isp, const Json& j, const std::size_t sensor, const int width,
    const int height) {
    const auto block = j.member(sensor, "awb");
    const auto range = j.member(block, "range");
    if (!j.is(range, JsonKind::object))
        return;
    const auto green = j.member(range, "green"), rg = j.member(range, "rg"),
               bg = j.member(range, "bg");
    esp_isp_awb_config_t cfg{};
    cfg.sample_point = static_cast<isp_awb_sample_point_t>(j.integer(block, "sample_point", 1));
    cfg.window.top_left.x = static_cast<std::uint32_t>(width / 10);
    cfg.window.top_left.y = static_cast<std::uint32_t>(height / 10);
    cfg.window.btm_right.x = static_cast<std::uint32_t>(width - width / 10);
    cfg.window.btm_right.y = static_cast<std::uint32_t>(height - height / 10);
    cfg.subwindow = cfg.window;
    cfg.white_patch.luminance.min = static_cast<std::uint32_t>(j.integer(green, "min", 128));
    cfg.white_patch.luminance.max = static_cast<std::uint32_t>(j.integer(green, "max", 240));
    cfg.white_patch.red_green_ratio.min = j.number(rg, "min", 0.7F);
    cfg.white_patch.red_green_ratio.max = j.number(rg, "max", 1.3F);
    cfg.white_patch.blue_green_ratio.min = j.number(bg, "min", 0.7F);
    cfg.white_patch.blue_green_ratio.max = j.number(bg, "max", 1.3F);
    isp_awb_ctlr_t handle{};
    if (esp_isp_new_awb_controller(isp, &cfg, &handle) != ESP_OK)
        return;
    if (esp_isp_awb_controller_enable(handle) != ESP_OK) {
        (void)esp_isp_del_awb_controller(handle);
        return;
    }
    awb.handle = handle;
    awb.gain_r = awb.gain_b = 1.0F;
    awb.min_counted = static_cast<std::uint32_t>(j.integer(block, "min_counted", 2000));
    awb.min_r_step = j.number(block, "min_red_gain_step", 0.034F);
    awb.min_b_step = j.number(block, "min_blue_gain_step", 0.034F);
}
} // namespace

void isp_apply(isp_proc_handle_t isp, const std::string_view json, const char* sensor,
    const int width, const int height) {
    isp_release();
    with_section(json, sensor, "ISP", [&](const Json& j, const std::size_t section) {
        apply_ccm(isp, j, section);
        apply_gamma(isp, j, section);
        apply_sharpen(isp, j, section);
        apply_color(isp, j, section);
        apply_bf(isp, j, section);
        init_awb(isp, j, section, width, height);
        AddLog(LogLevel::info, "CAM: ISP tuned for %s", sensor);
    });
}

#if USE_CSI_STILL
Ccm isp_ccm(const std::string_view json, const char* sensor) {
    Ccm result = kIdentityCcm;
    with_section(json, sensor, "still colour", [&](const Json& j, const std::size_t section) {
        float matrix[3][3]{};
        if (!read_ccm(j, section, matrix))
            return;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<std::int16_t>(
                std::clamp(std::lround(matrix[i / 3][i % 3] * 256.0F), -4096L, 4096L));
        AddLog(LogLevel::info, "CAM: still colour matrix from /isp.json for %s", sensor);
    });
    return result;
}
#endif

void isp_awb(isp_proc_handle_t isp) {
    if (!awb.handle)
        return;
    isp_awb_stat_result_t result{};
    if (esp_isp_awb_controller_get_oneshot_statistics(awb.handle, 100, &result) != ESP_OK ||
        result.white_patch_num < awb.min_counted)
        return;
    const float count = static_cast<float>(result.white_patch_num);
    const float avg_g = static_cast<float>(result.sum_g) / count;
    if (avg_g < 8.0F)
        return; // too dark to judge
    const float avg_r = std::fmax(static_cast<float>(result.sum_r) / count, 1.0F);
    const float avg_b = std::fmax(static_cast<float>(result.sum_b) / count, 1.0F);
    const float gain_r = std::fmin(std::fmax(avg_g / avg_r, 0.5F), 2.0F);
    const float gain_b = std::fmin(std::fmax(avg_g / avg_b, 0.5F), 2.0F);
    if (std::fabs(gain_r - awb.gain_r) < awb.min_r_step &&
        std::fabs(gain_b - awb.gain_b) < awb.min_b_step)
        return;
    awb.gain_r = gain_r;
    awb.gain_b = gain_b;
    // Grey world: corrected = diag(gain_r, 1, gain_b) * base matrix.
    esp_isp_ccm_config_t cfg{};
    for (int c = 0; c < 3; ++c) {
        cfg.matrix[0][c] = awb.base_ccm[0][c] * gain_r;
        cfg.matrix[1][c] = awb.base_ccm[1][c];
        cfg.matrix[2][c] = awb.base_ccm[2][c] * gain_b;
    }
    (void)esp_isp_ccm_configure(isp, &cfg);
}

void isp_release() {
    if (awb.handle) {
        (void)esp_isp_awb_controller_disable(awb.handle);
        (void)esp_isp_del_awb_controller(awb.handle);
    }
    awb = {};
}

} // namespace newmota::platform::camera_csi::detail
