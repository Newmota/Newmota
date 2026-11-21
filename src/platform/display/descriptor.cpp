/*
  descriptor.cpp - bounded UDisplay descriptor parser

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file descriptor.cpp
 * @brief Bounded UDisplay parser. No hardware operations occur here.
 * Adapted descriptor semantics from Tasmota UDisplay, Copyright (C) 2021
 * Gerhard Mutz and Theo Arends; GPL-3.0-or-later.
 */
#include "newmota/platform/display/descriptor.h"
#include "newmota_features.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace newmota::platform::display::detail {
namespace {
using View = std::string_view;
constexpr std::size_t kMaxDescriptor = 65536, kMaxCommands = 8192;

struct Fields {
    View remaining;
    bool* allocation_failed{};
    bool valid{true};

    View next() {
        const auto comma = remaining.find(',');
        auto value = remaining.substr(0, comma);
        remaining = comma == View::npos ? View{} : remaining.substr(comma + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (
            !value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
            value.remove_suffix(1);
        return value;
    }

    int number(int base = 10, int fallback = -1) {
        auto token = next();
        if (token.empty())
            return fallback;
        if (token == "*")
            return -1;
        int result{};
        if (base == 16 && token.size() > 2 && token.substr(0, 2) == "0x")
            token.remove_prefix(2);
        const auto parsed =
            std::from_chars(token.data(), token.data() + token.size(), result, base);
        valid &= parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
        return result;
    }

    bool bytes(Bytes& output, bool bare_command_has_no_arguments = false) {
        const bool bare_command = !remaining.empty() && remaining.find(',') == View::npos;
        while (!remaining.empty()) {
            const int value = number(16);
            if (!valid || value < 0 || value > 255 || output.size() == kMaxCommands)
                return false;
            if (!output.push_back(static_cast<std::uint8_t>(value))) {
                *allocation_failed = true;
                return false;
            }
        }
        if (bare_command_has_no_arguments && bare_command) {
            if (output.size() == kMaxCommands)
                return false;
            if (!output.push_back(0)) {
                *allocation_failed = true;
                return false;
            }
        }
        return valid;
    }
};

bool sequence(const Bytes& data, bool dsi, bool special, bool epd = false) {
    std::size_t offset{};
    while (offset < data.size()) {
        if (data.size() - offset < 2)
            return false;
        const auto command = data[offset++];
        const auto argument = data[offset++];
        const std::size_t count = dsi ? argument : special ? argument & 0x7fU : argument & 0x1fU;
        if (count > data.size() - offset)
            return false;
        if (epd && command >= 0x60 && command <= 0x6a && count > 1)
            return false;
        offset += count;
        if (dsi) {
            if (offset == data.size())
                return false;
            ++offset;
        }
    }
    return true;
}
} // namespace

Result parse(const char* text, std::size_t length, Config& c, char* error, std::size_t capacity) {
    bool allocation_failed{};
    auto fail = [&](const char* reason) {
        if (error && capacity)
            std::snprintf(error, capacity, "%s", reason);
        return allocation_failed ? Result::no_memory : Result::invalid_descriptor;
    };
    if (!text || !length || length > kMaxDescriptor || std::memchr(text, 0, length))
        return fail("Invalid descriptor length");
    bool header{}, timing{};
    char section{};
#if USE_UNIVERSAL_TOUCH
    unsigned touch_section = 4;
#endif
    std::size_t lut_index{};
    View input{text, length};
    while (!input.empty()) {
        auto end = input.find_first_of("\n");
        auto line = input.substr(0, end);
        input = end == View::npos ? View{} : input.substr(end + 1);
        while (
            !line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.remove_prefix(1);
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '#')
            break;
        const auto comment = line.find(';');
        if (comment != View::npos)
            line = line.substr(0, comment);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        bool marker = line.front() == ':';
        View key;
        if (marker) {
            line.remove_prefix(1);
            const auto comma = line.find(',');
            key = line.substr(0, comma);
            line = comma == View::npos ? View{} : line.substr(comma + 1);
            if (key.empty())
                return fail("Empty section");
            section = key.front();
            if (section != 'H' && !header)
                return fail("Header must precede display sections");
        }
#if USE_UNIVERSAL_TOUCH
        if (marker && key == "UTI") {
            if (!touch_header(line, c.touch))
                return fail("Universal touch requires UTI,name,I1|I2,address,reset,irq; "
                            "SPI/resistive touch is unsupported");
            touch_section = 0;
            c.touch.programs[0].present = true;
            continue;
        }
        if (marker && (key == "UTT" || key == "UTX" || key == "UTY")) {
            if (!c.touch.configured || !line.empty())
                return fail(
                    "Touch program requires a preceding UTI header and no section arguments");
            touch_section = key == "UTT" ? 1 : key == "UTX" ? 2 : 3;
            if (c.touch.programs[touch_section].present)
                return fail("Duplicate universal touch program");
            c.touch.programs[touch_section].present = true;
            continue;
        }
        if (marker)
            touch_section = 4;
        else if (touch_section < 4) {
            if (!touch_compile(line, c.touch.programs[touch_section]))
                return fail("Invalid, unsupported or oversized universal touch instruction");
            continue;
        }
#endif
        Fields f{line, &allocation_failed};
        if (marker && section == 'H') {
            if (header)
                return fail("Duplicate display header");
            const auto name = f.next();
            if (name.empty() || name.size() >= sizeof(c.info.name))
                return fail("Display name is too long");
            std::memcpy(c.info.name, name.data(), name.size());
            const int width = f.number(), height = f.number(), depth = f.number();
            if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
                (depth != 1 && depth != -1 && depth != 16))
                return fail("Unsupported display geometry or depth");
            c.info.width = width;
            c.info.height = height;
            c.info.bpp = std::abs(depth);
            c.inverted_mono = depth < 0;
            const auto kind = f.next();
            if (kind == "SPI" || kind == "EPD") {
                c.bus = kind == "EPD" ? Bus::epd : Bus::spi;
                c.spi_bus = f.number();
                c.cs = f.number();
                c.clock = f.number();
                c.mosi = f.number();
                c.dc = f.number();
                c.backlight = f.number();
                c.reset = f.number();
                c.miso = f.number();
                c.speed = f.number();
                if (c.bus == Bus::epd) {
                    c.busy = c.miso;
                    c.miso = -1;
                }
                if (c.spi_bus < 1 || c.spi_bus > 4 || c.speed < 1 || c.speed > 80)
                    return fail("Invalid SPI bus or frequency");
            } else if (kind == "QSPI") {
#if USE_DISPLAY_QSPI
                c.bus = Bus::qspi;
                c.spi_bus = f.number();
                c.cs = f.number();
                c.clock = f.number();
                for (auto& pad : c.quad_pins)
                    pad = f.number();
                c.backlight = f.number();
                c.reset = f.number();
                c.speed = f.number();
                const auto lines = f.number(10, 4);
                if (c.info.bpp != 16 || c.spi_bus < 1 || c.spi_bus > 2 || c.speed < 1 ||
                    c.speed > 80 || lines != 4)
                    return fail("QSPI requires RGB565, native bus and four data lines");
#else
                return fail("QSPI requires USE_DISPLAY_QSPI");
#endif
            } else if (kind == "I2C" || kind == "I2C1" || kind == "I2C2") {
                c.bus = Bus::i2c;
                c.i2c_bus = kind == "I2C2";
                c.address = f.number(16);
                c.scl = f.number();
                c.sda = f.number();
                c.reset = f.number();
                if (c.info.bpp != 1 || c.address < 8 || c.address > 0x77 ||
                    (c.scl < 0) != (c.sda < 0))
                    return fail("Invalid I2C display configuration");
            } else if (kind == "PAR" || kind == "I80") {
                c.bus = Bus::i80;
                c.data_width = f.number();
                if (c.data_width != 8 && c.data_width != 16)
                    return fail("Invalid I80 bus width");
                c.reset = f.number();
                c.cs = f.number();
                c.dc = f.number();
                c.wr = f.number();
                c.rd = f.number();
                c.backlight = f.number();
                for (int i = 0; i < c.data_width; ++i)
                    c.data_pins[i] = f.number();
                c.speed = f.number();
                if (c.speed < 1 || c.speed > 80 || c.info.bpp != 16)
                    return fail("Invalid I80 display configuration");
            } else if (kind == "RGB") {
                c.bus = Bus::rgb;
                c.de = f.number();
                c.vsync = f.number();
                c.hsync = f.number();
                c.pclk = f.number();
                c.backlight = f.number();
                for (int i = 8; i < 16; ++i)
                    c.data_pins[i] = f.number();
                for (int i = 0; i < 8; ++i)
                    c.data_pins[i] = f.number();
                c.speed = f.number();
                if (c.speed < 1 || c.speed > 80 || c.info.bpp != 16)
                    return fail("Invalid RGB display configuration");
            } else if (kind == "DSI") {
                c.bus = Bus::dsi;
                c.lanes = f.number();
                c.te = f.number();
                c.backlight = f.number();
                c.reset = f.number();
                c.ldo = f.number();
                c.voltage = f.number();
                c.pixel_clock = f.number();
                c.lane_speed = f.number();
                c.rgb_order = f.number();
                c.endian = f.number();
                if (c.lanes < 1 || c.lanes > 2 || c.pixel_clock < 1000000 ||
                    c.pixel_clock > 100000000 || c.lane_speed < 80 || c.lane_speed > 2500 ||
                    c.rgb_order < 0 || c.rgb_order > 1 || c.endian < 0 || c.endian > 1 ||
                    c.info.bpp != 16)
                    return fail("Invalid DSI display configuration");
                if (c.ldo >= 0 && (c.ldo > 4 || c.voltage < 500 || c.voltage > 3300))
                    return fail("Invalid DSI LDO configuration");
            } else
                return fail("Unknown display type; e-paper requires EPD");
            header = true;
        } else if (marker && (key == "IS" || key == "IC" || key == "ICS" || key == "II")) {
            if (key == "IC") {
                c.all_commands = true;
                section = 'I';
            } else if (c.bus != Bus::rgb)
                return fail("RGB initialization section on another panel type");
            else if (key == "II") {
                c.init_kind = 2;
                c.init_bus = f.number() - 1;
                c.init_address = f.number(16);
                if (c.init_bus < 0 || c.init_bus > 1 || c.init_address < 8 || c.init_address > 0x77)
                    return fail("Invalid RGB initialization I2C bus");
            } else {
                c.init_kind = 1;
                c.init_clock = f.number();
                c.init_mosi = f.number();
                c.init_cs = f.number();
                c.init_reset = f.number();
            }
        } else if (section == 'I' || section == 'f' || section == 'p') {
            if (section != 'I' && c.bus != Bus::epd)
                return fail("E-paper sections require explicit EPD type");
            if (section == 'I' && c.bus == Bus::rgb) {
                Bytes bytes;
                if (!f.bytes(bytes))
                    return fail("Invalid RGB initialization bytes");
                if (c.init_lines.size() == 4096)
                    return fail("RGB initialization record limit exceeded");
                if (!c.init_lines.push_back(std::move(bytes))) {
                    allocation_failed = true;
                    return fail("Display descriptor allocation failed");
                }
            } else if (!f.bytes(section == 'I'   ? c.init
                                : section == 'f' ? c.full
                                                 : c.partial,
                           c.bus == Bus::epd))
                return fail("Invalid command bytes or command capacity exceeded");
        } else if (section == 'L' || section == 'l') {
            if (c.bus != Bus::epd)
                return fail("LUT sections require explicit EPD type");
            if (marker) {
                lut_index = section == 'l'    ? 1
                            : key.size() == 1 ? 0
                                              : static_cast<std::size_t>(key[1] - '1' + 2);
                if (lut_index >= c.luts.size() ||
                    (key.size() > 1 && (key.size() != 2 || key[1] < '1' || key[1] > '5')))
                    return fail("Invalid LUT index");
                auto& lut = c.luts[lut_index];
                int declared = f.number(), command = f.number(16);
                if (declared <= 0 || declared > 4096 || command < 0 || command > 255 ||
                    lut.declared)
                    return fail("Invalid or duplicate LUT declaration");
                lut.declared = declared;
                lut.command = command;
            } else {
                auto& lut = c.luts[lut_index];
                if (!f.bytes(lut.bytes) || lut.bytes.size() > lut.declared)
                    return fail("LUT exceeds declared size");
            }
        } else if (marker) {
            auto byte = [&](std::uint8_t& dest, int base = 16, int fallback = 255) {
                int value = f.number(base, fallback);
                if (value < 0 || value > 255) {
                    f.valid = false;
                    return;
                }
                dest = value;
            };
            switch (section) {
            case 'V':
                if (c.bus != Bus::rgb && c.bus != Bus::dsi)
                    return fail("Video timings require RGB or DSI");
                if (timing)
                    return fail("Duplicate video timing section");
                for (int i = 0; i < (c.bus == Bus::rgb ? 9 : 6); ++i) {
                    c.timing[i] = f.number();
                    if (c.timing[i] < 0 || c.timing[i] > 4096)
                        return fail("Invalid video timing");
                }
                timing = true;
                break;
            case 'O':
                byte(c.on);
                break;
            case 'o':
                byte(c.off);
                break;
            case 'i':
                byte(c.inv_off);
                byte(c.inv_on);
                break;
            case 'R':
                byte(c.madctl);
                byte(c.startline);
                break;
            case 'D':
                byte(c.dim);
                break;
            case 'b':
                byte(c.backlight_flags);
                break;
            case 'P': {
                int value = f.number();
                if (value != 16 && value != 18)
                    return fail("Unsupported pixel wire format");
                if (value == 18 && (c.bus == Bus::rgb || c.bus == Bus::dsi))
                    return fail("18-bit pixel wire format is unsupported on RGB and DSI");
                c.color_mode = value;
                break;
            }
            case 'B': {
                int lines = f.number(), flags = f.number();
                if (lines < 1 || lines > c.info.height || flags < 0 || flags > 63)
                    return fail("Invalid flush buffer configuration");
                c.info.flush_lines = lines;
                c.info.flags = flags;
                break;
            }
            case 'F':
                if (c.bus != Bus::spi || c.info.bpp != 1)
                    return fail("Packed mode requires monochrome SPI");
                byte(c.pack_width, 10, 0);
                byte(c.pack_height, 10, 0);
                byte(c.pack_flags, 10, 0);
                if (!c.pack_width || !c.pack_height || c.pack_width * c.pack_height > 8 ||
                    c.pack_flags > 3 || c.info.width % c.pack_width ||
                    c.info.height % c.pack_height)
                    return fail("Invalid packed monochrome geometry");
                break;
            case 'A':
            case 'a':
                byte(c.addr_x);
                byte(c.addr_y);
                byte(c.ram);
                if (c.info.bpp == 1 && c.bus != Bus::epd && !c.pack_width) {
                    // Legacy OLED fields are
                    // command,page-start,page-end,page-base,column-base,column-end,write-command.
                    c.page_start = c.addr_y;
                    c.page_end = c.ram;
                    byte(c.addr_y, 16, 0);
                    byte(c.col_start, 16, 0);
                    byte(c.col_end, 16, 0);
                    byte(c.ram);
                } else if (section == 'A') {
                    int mode = f.number(10, 16);
                    if (mode != 0 && mode != 8 && mode != 16 && mode != 32)
                        return fail("Unsupported address width");
                    c.address_mode = mode;
                    if (c.pack_width)
                        for (auto& bound : c.ram_bounds)
                            byte(bound);
                }
                break;
            case '0':
            case '1':
            case '2':
            case '3': {
                const auto index = static_cast<unsigned>(section - '0');
                if (c.bus != Bus::rgb) {
                    byte(c.rot[index]);
                    int x = f.number(16, 0), y = f.number(16, 0);
                    if (x < 0 || x > 65535 - c.info.width || y < 0 || y > 65535 - c.info.height)
                        return fail("Rotation address offset overflow");
                    c.x_offset[index] = x;
                    c.y_offset[index] = y;
                }
                if (!f.remaining.empty()) {
#if USE_UNIVERSAL_TOUCH
                    const auto transform = f.number(16, index);
                    if (transform < 0 || (transform & ~0x8f) || (transform & 0xf) > 5)
                        return fail("Unsupported touch rotation transform");
                    c.touch.transforms[index] = transform;
#else
                    (void)f.number(16, 0);
#endif
                }
                break;
            }
            case 'T':
                if (key.size() > 1) {
                    f.remaining = {};
                    break;
                } // Touch is a separate consumer.
                if (c.bus != Bus::epd)
                    return fail("E-paper timing requires EPD type");
                c.full_time = f.number();
                c.partial_time = f.number();
                c.update_time = f.number();
                if (c.full_time < -60000 || c.full_time > 60000 || c.partial_time < 0 ||
                    c.partial_time > 60000 || c.update_time < 0 || c.update_time > 60000)
                    return fail("Invalid EPD timing");
                break;
            case 'M':
#if USE_UNIVERSAL_TOUCH
                for (auto& limit : c.touch.calibration) {
                    limit = f.number();
                    if (limit < -32768 || limit > 32767)
                        return fail("Invalid touch calibration range");
                }
                c.touch.calibrated = c.touch.calibration[0] >= 0;
                if (c.touch.calibrated && (c.touch.calibration[0] == c.touch.calibration[1] ||
                                              c.touch.calibration[2] == c.touch.calibration[3]))
                    return fail("Touch calibration endpoints must differ");
                break;
#endif
            case 'S':
            case 'U':
                f.remaining = {};
                break; // Renderer and touch metadata.
            default:
                return fail("Unknown descriptor section");
            }
        } else if (section == 'U' || section == 'S' || section == 'M' || section == 'T') {
            f.remaining = {}; // Ignored Renderer/touch blocks may have continuation records.
        } else if (!line.empty())
            return fail("Unexpected descriptor data");
        if (!f.valid || !f.remaining.empty())
            return fail("Invalid or excess descriptor fields");
    }
    if (!header)
        return fail("Missing display header");
    if (c.bus == Bus::qspi &&
        (c.color_mode != 16 || c.address_mode != 16 || c.ram == 255 || c.all_commands))
        return fail("QSPI requires RGB565, 16-bit addressing and RAMWR");
#if USE_UNIVERSAL_TOUCH
    if (c.touch.configured)
        for (const auto& program : c.touch.programs)
            if (!program.present || !program.size)
                return fail("Universal touch requires nonempty UTI, UTT, UTX and UTY programs");
#endif
    c.info.flush_lines = std::min(c.info.flush_lines, c.info.height);
    if ((c.bus == Bus::rgb || c.bus == Bus::dsi) && !timing)
        return fail("Missing video timing");
    if (c.bus == Bus::rgb && (c.timing[0] > 1 || c.timing[4] > 1 || c.timing[8] > 1))
        return fail("Invalid RGB polarity");
    for (const auto& lut : c.luts)
        if (lut.bytes.size() != lut.declared)
            return fail("LUT length does not match declaration");
    if (c.bus == Bus::epd) {
        if (c.info.bpp != 1)
            return fail("EPD requires one-bit pixels");
        bool five = true;
        for (std::size_t i = 2; i < 7; ++i)
            five &= c.luts[i].declared != 0;
        bool any_five = false;
        for (std::size_t i = 2; i < 7; ++i)
            any_five |= c.luts[i].declared != 0;
        if (any_five && !five)
            return fail("Five-table EPD requires all five LUTs");
        if ((c.luts[0].declared != 0) != (c.luts[1].declared != 0))
            return fail("EPD requires both full and partial LUTs");
        if (five && c.luts[0].declared)
            return fail("Mixed EPD LUT modes");
        c.epd_mode = five ? 2 : c.luts[0].declared ? 1 : 3;
        if (!five && c.full.empty())
            return fail("Missing EPD full refresh sequence");
        if (five && (c.addr_x == 255 || c.addr_y == 255 || c.ram == 255))
            return fail("Missing five-table EPD transfer commands");
    }
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned width = (i & 1U) ? c.info.height : c.info.width,
                       height = (i & 1U) ? c.info.width : c.info.height;
        if (c.x_offset[i] + width > 65536U || c.y_offset[i] + height > 65536U)
            return fail("Rotated address window overflow");
    }
    if (c.bus == Bus::i2c) {
        // I2C initialization is a sequence of raw command bytes.
    } else if (c.bus == Bus::rgb) {
        if (!c.init_lines.empty() && !c.init_kind)
            return fail("RGB initialization transport is missing");
        for (const auto& bytes : c.init_lines) {
            if (c.init_kind == 1 && !sequence(bytes, false, true))
                return fail("Malformed RGB SPI initialization");
            if (c.init_kind == 2 && bytes.size() != 1 && bytes.size() != 2)
                return fail("Malformed RGB I2C initialization");
        }
    } else if (!sequence(c.init, c.bus == Bus::dsi, false, c.bus == Bus::epd && c.epd_mode != 2))
        return fail("Malformed initialization sequence");
    if (!sequence(c.full, false, false, true) || !sequence(c.partial, false, false, true))
        return fail("Malformed EPD refresh sequence");
    c.info.buffered = c.info.bpp == 1 || c.bus == Bus::rgb || c.bus == Bus::dsi;
    if (error && capacity)
        error[0] = 0;
    return Result::success;
}
} // namespace newmota::platform::display::detail
