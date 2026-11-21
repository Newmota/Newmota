/*
  touch.cpp - universal I2C touch compiler and runtime

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file touch.cpp
 * @brief Bounded descriptor bytecode over repeated-start I2C with cached pointer samples.
 * Touch language and coordinate transforms adapted from Tasmota UDisplay,
 * Copyright (C) 2021 Gerhard Mutz and Theo Arends; GPL-3.0-or-later.
 * Baseline: fb629eddb5047764781851ee059366328814a552.
 */
#include "newmota/platform/display/touch.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/i2c.h"
#include "newmota/platform/platform.h"
#include "newmota/log.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace newmota::platform::display {
namespace {
using View = std::string_view;
using detail::TouchConfig;
using detail::TouchProgram;

enum Op : std::uint8_t {
    rd,
    rdm,
    cp,
    rtf,
    mv,
    mvb,
    rt,
    rtt,
    rdw,
    rdwm,
    wr,
    wrw,
    cpr,
    bit_and,
    scale,
    limit,
    debug,
    cpm,
    rdl,
    rdlm
};

const TouchConfig* configuration{};
TouchState cached_touch{};
std::array<std::uint8_t, 16> buffer{};
std::uint16_t base_width{}, base_height{};
std::uint32_t sampled_at{};
bool sampled{};

View trim(View value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == View::npos)
        return {};
    value.remove_prefix(first);
    value = value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
    return value;
}

View token(View& input, bool comma = false) {
    input = trim(input);
    const auto end = comma ? input.find(',') : input.find_first_of(" \t\r\n");
    auto value = trim(input.substr(0, end));
    input = end == View::npos ? View{} : input.substr(end + 1);
    return value;
}

bool number(View text, int base, std::uint32_t maximum, std::uint32_t& value) {
    if (text.empty())
        return false;
    if (base == 16 && text.size() > 2 && text.substr(0, 2) == "0x")
        text.remove_prefix(2);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           value <= maximum;
}

bool signed_number(View text, int minimum, int maximum, int& value) {
    if (text.empty())
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
           value >= minimum && value <= maximum;
}

/** @brief Executes only prevalidated, forward-only instructions; I/O failure aborts immediately. */
bool execute(const TouchProgram& program, std::int32_t& result) {
    result = 0;
    std::size_t at{};
    auto byte = [&]() { return program.code[at++]; };
    auto word = [&]() {
        const unsigned high = byte();
        return (high << 8) | byte();
    };
    while (at < program.size) {
        const auto opcode = byte();
        if (opcode == rd || opcode == rdm || opcode == rdw || opcode == rdwm || opcode == rdl ||
            opcode == rdlm) {
            const unsigned width = opcode == rd || opcode == rdm     ? 1
                                   : opcode == rdw || opcode == rdwm ? 2
                                                                     : 4;
            const auto* address = program.code.data() + at;
            at += width;
            const unsigned count = opcode == rdm || opcode == rdwm || opcode == rdlm ? byte() : 1;
            buffer.fill(0);
            if (i2c::transfer(configuration->bus, configuration->address, address, width,
                    buffer.data(), count) != i2c::Result::success) {
                buffer.fill(0);
                return false;
            }
        } else if (opcode == wr || opcode == wrw) {
            const auto count = opcode == wr ? 2U : 3U;
            const auto* data = program.code.data() + at;
            at += count;
            if (i2c::transfer(configuration->bus, configuration->address, data, count, nullptr,
                    0) != i2c::Result::success) {
                buffer.fill(0);
                return false;
            }
        } else
            switch (opcode) {
            case cp:
                result = buffer[0] == byte();
                break;
            case cpr:
                result = result == byte();
                break;
            case cpm: {
                const auto count = byte();
                result = 0;
                for (unsigned i = 0; i < count; ++i)
                    result |= buffer[0] == byte();
                break;
            }
            case rtf:
                if (!result)
                    return true;
                break;
            case rtt:
                if (result > 0) {
                    result = 0;
                    return true;
                }
                break;
            case rt:
                return true;
            case mv: {
                const auto index = byte(), mode = byte();
                result = mode == 1   ? buffer[index]
                         : mode == 2 ? (buffer[index] << 8) | buffer[index + 1]
                                     : buffer[index] | (buffer[index + 1] << 8);
                result &= 0xfff;
                break;
            }
            case mvb: {
                const auto high = byte(), index = byte();
                result = high ? (result & 0xff) | (buffer[index] << 8)
                              : (result & 0xff00) | buffer[index];
                break;
            }
            case bit_and:
                result &= word();
                break;
            case scale: {
                const auto offset = static_cast<std::int16_t>(word());
                std::uint32_t bits{};
                for (unsigned i = 0; i < 4; ++i)
                    bits = (bits << 8) | byte();
                float factor{};
                std::memcpy(&factor, &bits, sizeof(factor));
                const float value = static_cast<float>(result - offset) * factor;
                // Avoid undefined float-to-integer conversions, including overflow.
                result = value <= -32768.0f  ? -32768
                         : value >= 32767.0f ? 32767
                                             : static_cast<std::int32_t>(value);
                break;
            }
            case limit:
                result = std::min<std::int32_t>(result, word());
                break;
            case debug:
                AddLog(LogLevel::info, "UTDBG %u: %ld : %02x,%02x,%02x,%02x", byte(),
                    static_cast<long>(result), buffer[0], buffer[1], buffer[2], buffer[3]);
                break;
            default:
                return false;
            }
    }
    return true;
}

int calibrate(int value, int minimum, int maximum, unsigned extent) {
    const auto scaled =
        static_cast<std::int64_t>(value - minimum) * (extent - 1) / (maximum - minimum);
    return static_cast<int>(std::clamp<std::int64_t>(scaled, 0, extent - 1));
}
} // namespace

TouchState touch_state() {
    return cached_touch;
}

namespace detail {
bool touch_header(View input, TouchConfig& config) {
    if (config.configured)
        return false;
    const auto name = token(input, true), mode = token(input, true);
    if (name.empty() || name.size() >= sizeof(config.name) || (mode != "I1" && mode != "I2"))
        return false;
    std::uint32_t address{};
    if (!number(token(input, true), 16, 0x77, address) || address < 8)
        return false;
    auto pin = [&](int& value) {
        const auto text = token(input, true);
        return text == "*" ? (value = -1, true) : signed_number(text, -1, 65534, value);
    };
    if (!pin(config.reset) || !pin(config.irq) || !trim(input).empty())
        return false;
    std::memcpy(config.name, name.data(), name.size());
    config.bus = mode == "I2";
    config.address = address;
    config.configured = true;
    return true;
}

bool touch_compile(View input, TouchProgram& program) {
    while (!(input = trim(input)).empty()) {
        const auto name = token(input);
        std::array<std::uint8_t, 10> instruction{};
        std::size_t size{};
        auto emit = [&](std::uint32_t value, unsigned count = 1) {
            for (unsigned i = count; i; --i)
                instruction[size++] = value >> ((i - 1) * 8);
        };
        auto operand = [&](unsigned width, int base = 16, std::uint32_t maximum = 0xffffffffU) {
            std::uint32_t value{};
            if (!number(token(input), base, maximum, value) ||
                (width < 4 && value >= (1U << (width * 8))))
                return false;
            emit(value, width);
            return true;
        };
        if (name == "RD" || name == "RDM" || name == "RDW" || name == "RDWM" || name == "RDL" ||
            name == "RDLM") {
            const auto width = name == "RD" || name == "RDM"     ? 1U
                               : name == "RDW" || name == "RDWM" ? 2U
                                                                 : 4U;
            const bool many = name.back() == 'M';
            emit(width == 1   ? (many ? rdm : rd)
                 : width == 2 ? (many ? rdwm : rdw)
                              : (many ? rdlm : rdl));
            if (!operand(width))
                return false;
            if (many) {
                if (!operand(1, 10, buffer.size()) || instruction[size - 1] == 0)
                    return false;
            }
        } else if (name == "WR" || name == "WRW") {
            emit(name == "WR" ? wr : wrw);
            if (!operand(name == "WR" ? 1 : 2) || !operand(1))
                return false;
        } else if (name == "CP" || name == "CPR") {
            emit(name == "CP" ? cp : cpr);
            if (!operand(1))
                return false;
        } else if (name == "CPM") {
            emit(cpm);
            if (!operand(1, 16, 8) || instruction[size - 1] == 0)
                return false;
            const auto count = instruction[size - 1];
            for (unsigned i = 0; i < count; ++i)
                if (!operand(1))
                    return false;
        } else if (name == "RTF" || name == "RTT" || name == "RT") {
            emit(name == "RTF" ? rtf : name == "RTT" ? rtt : rt);
        } else if (name == "MV" || name == "MVB") {
            emit(name == "MV" ? mv : mvb);
            if (!operand(1, 10, name == "MV" ? 15 : 1) || !operand(1, 10, name == "MV" ? 3 : 15))
                return false;
            if (name == "MV" && (!instruction[2] || (instruction[2] != 1 && instruction[1] == 15)))
                return false;
        } else if (name == "AND" || name == "LIM") {
            emit(name == "AND" ? bit_and : limit);
            if (!operand(2, name == "AND" ? 16 : 10))
                return false;
        } else if (name == "SCL") {
            emit(scale);
            int offset{};
            if (!signed_number(token(input), -32768, 32767, offset))
                return false;
            emit(static_cast<std::uint16_t>(offset), 2);
            const auto value = token(input);
            char text[48]{};
            if (value.empty() || value.size() >= sizeof(text))
                return false;
            std::memcpy(text, value.data(), value.size());
            char* end{};
            const float factor = std::strtof(text, &end);
            if (*end || !std::isfinite(factor))
                return false;
            std::uint32_t bits{};
            std::memcpy(&bits, &factor, sizeof(bits));
            emit(bits, 4);
        } else if (name == "DBG") {
            emit(debug);
            if (!operand(1, 10))
                return false;
        } else
            return false; // SPI/XPT and direct resistive GSRT require separate owned transports.
        if (program.size + size >= program.code.size())
            return false;
        std::memcpy(program.code.data() + program.size, instruction.data(), size);
        program.size += size;
    }
    return true;
}

Result touch_open(const TouchConfig& config, std::uint16_t width, std::uint16_t height) {
    configuration = &config;
    cached_touch = {};
    base_width = width;
    base_height = height;
    buffer.fill(0);
    if (config.reset >= 0) {
        const PinId pin{static_cast<std::uint16_t>(config.reset)};
        if (gpio_configure_output(pin, true) != GpioResult::success)
            return Result::failed;
        sleep_for_milliseconds(10);
        if (gpio_write(pin, false) != GpioResult::success)
            return Result::failed;
        sleep_for_milliseconds(10);
        if (gpio_write(pin, true) != GpioResult::success)
            return Result::failed;
        sleep_for_milliseconds(50);
    }
    if (config.irq >= 0 && gpio_configure_input(PinId{static_cast<std::uint16_t>(config.irq)},
                               GpioPull::none) != GpioResult::success)
        return Result::failed;
    std::int32_t result{};
    if (!execute(config.programs[0], result) || !result)
        return Result::failed;
    cached_touch.available = cached_touch.healthy = true;
    sampled = false;
    return Result::success;
}

void touch_release() {
    cached_touch.pressed = false;
    sampled = false;
}

void touch_poll(std::uint8_t rotation, std::uint16_t width, std::uint16_t height) {
    if (!cached_touch.available)
        return;
    const auto now = monotonic_milliseconds();
    if (sampled && now - sampled_at < 20)
        return;
    sampled = true;
    sampled_at = now;
    cached_touch.pressed = false;
    cached_touch.healthy = false;
    buffer.fill(0);
    std::int32_t pressed{}, x{}, y{};
    if (!execute(configuration->programs[1], pressed))
        return;
    cached_touch.healthy = true;
    if (pressed <= 0)
        return;
    if (!execute(configuration->programs[2], x) || !execute(configuration->programs[3], y)) {
        cached_touch.healthy = false;
        return;
    }
    const auto transform = configuration->transforms[rotation];
    if (transform & 0x80)
        std::swap(x, y);
    if (configuration->calibrated) {
        const auto& limits = configuration->calibration;
        x = calibrate(x, limits[0], limits[1], base_width);
        y = calibrate(y, limits[2], limits[3], base_height);
    }
    switch (transform & 0xf) {
    case 1: {
        const auto old = x;
        x = y;
        y = height - 1 - old;
        break;
    }
    case 2:
        x = width - 1 - x;
        y = height - 1 - y;
        break;
    case 3: {
        const auto old = y;
        y = x;
        x = width - 1 - old;
        break;
    }
    case 4:
        x = width - 1 - x;
        break;
    case 5:
        y = height - 1 - y;
        break;
    }
    cached_touch.x = std::clamp<std::int32_t>(x, 0, width - 1);
    cached_touch.y = std::clamp<std::int32_t>(y, 0, height - 1);
    cached_touch.pressed = true;
}
} // namespace detail
} // namespace newmota::platform::display
