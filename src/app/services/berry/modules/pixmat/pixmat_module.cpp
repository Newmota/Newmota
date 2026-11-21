/*
  pixmat_module.cpp - Berry pixel matrix and framebuffer class

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file pixmat_module.cpp
 *  @brief Implements the Berry pixel matrix and framebuffer class.
 */

#include "services/berry/modules/pixmat/pixmat_module.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
extern "C" {
#include "berry.h"
#include "be_byteslib.h"
}

namespace {

using Matrix = newmota::PixmatBuffer;

/** @brief Reads an integer argument or raises a Berry type error. */
bint integer(bvm* vm, int at) {
    if (!be_isint(vm, at))
        be_raise(vm, "type_error", "pixmat expects integer arguments");
    return be_toint(vm, at);
}

/** @brief Reads and bounds an integer argument. */
int bounded(bvm* vm, int at, int low, int high) {
    const auto value = integer(vm, at);
    if (value < low || value > high)
        be_raise(vm, "value_error", "pixmat argument out of range");
    return static_cast<int>(value);
}

/** @brief Stores an integer-valued hidden field on a pixmat instance. */
void member(bvm* vm, const char* name, bint value) {
    be_pushint(vm, value);
    be_setmember(vm, 1, name);
    be_pop(vm, 1);
}

/** @brief Reads a nonnegative hidden integer field from a pixmat instance. */
int metadata(bvm* vm, int at, const char* name) {
    be_getmember(vm, at, name);
    const auto value = bounded(vm, -1, 0, std::numeric_limits<int>::max());
    be_pop(vm, 1);
    return value;
}

/** @brief Validates matrix dimensions and returns the required buffer length. */
std::size_t dimensions(bvm* vm, int w, int h, int bpp) {
    if (w < 1 || h < 1 || bpp < 0 || bpp > 4)
        be_raise(vm, "value_error", "invalid pixmat dimensions or bpp (0..4)");
    const auto wide = static_cast<std::uint64_t>(w), tall = static_cast<std::uint64_t>(h);
    const auto need = bpp ? wide * tall * static_cast<unsigned>(bpp) : wide * ((tall + 7U) / 8U);
    if (need > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        be_raise(vm, "value_error", "invalid pixmat dimensions or bpp (0..4)");
    return static_cast<std::size_t>(need);
}

/** @brief Confirms that a Berry value is an instance of the pixmat class. */
void class_check(bvm* vm, int at) {
    if (!be_isinstance(vm, at))
        be_raise(vm, "type_error", "expected pixmat instance");
    be_getglobal(vm, "pixmat");
    const bool valid = be_isderived(vm, at);
    be_pop(vm, 1);
    if (!valid)
        be_raise(vm, "type_error", "expected pixmat instance");
}

/** @brief Validates a Berry bytes buffer and returns its current address. */
const std::uint8_t* bytes(bvm* vm, int at, std::size_t need, bool write) {
    if (!be_isbytes(vm, at))
        be_raise(vm, "type_error", "expected bytes buffer");
    if (write) {
        be_getmember(vm, at, ".size");
        const bool readonly = be_toint(vm, -1) == BYTES_SIZE_SOLIDIFIED;
        be_pop(vm, 1);
        if (readonly)
            be_raise(vm, "value_error", "pixmat buffer is read only");
    }
    std::size_t length{};
    const auto* data = static_cast<const std::uint8_t*>(be_tobytes(vm, at, &length));
    if (!data || length < need)
        be_raise(vm, "value_error", "pixmat buffer too small or closed");
    return data;
}

/** @brief Resolves matrix metadata and the current backing buffer address. */
Matrix matrix(bvm* vm, int at, bool write) {
    class_check(vm, at);
    Matrix m;
    m.width = metadata(vm, at, ".width");
    m.height = metadata(vm, at, ".height");
    m.bpp = metadata(vm, at, ".bpp");
    m.serpentine = metadata(vm, at, ".serp") != 0;
    const auto need = dimensions(vm, m.width, m.height, m.bpp);
    be_getmember(vm, at, ".buf");
    m.data = const_cast<std::uint8_t*>(bytes(vm, -1, need, write));
    be_pop(vm, 1);
    return m;
}

/** @brief Tests whether two contiguous byte ranges overlap. */
bool overlap(const Matrix& a, const Matrix& b) {
    const auto x = reinterpret_cast<std::uintptr_t>(a.data);
    const auto y = reinterpret_cast<std::uintptr_t>(b.data);
    return x <= y ? y - x < a.size() : x - y < b.size();
}

/** @brief Unpacks a packed integer into big-endian per-channel bytes. */
void unpack(std::uint32_t color, int bpp, std::uint8_t* out) {
    for (int c = bpp - 1; c >= 0; --c) {
        out[c] = static_cast<std::uint8_t>(color);
        color >>= 8;
    }
}

/** @brief Scales one channel by an 8-bit factor with rounding. */
std::uint8_t scale(std::uint8_t value, int factor) {
    return static_cast<std::uint8_t>(
        (static_cast<unsigned>(value) * static_cast<unsigned>(factor) + 127U) / 255U);
}

/** @brief Checks whether a logical coordinate lies inside a matrix. */
bool inside(const Matrix& m, bint x, bint y) {
    return x >= 0 && x < m.width && y >= 0 && y < m.height;
}

/** @brief Initializes owned, wrapped, or bitplane-backed matrix storage. */
int Init(bvm* vm) {
    const int argc = be_top(vm);
    be_getmember(vm, 1, ".buf");
    const bool initialized = !be_isnil(vm, -1);
    be_pop(vm, 1);
    if (initialized)
        be_raise(vm, "runtime_error", "deinit pixmat before reinitializing");
    const bool external = argc >= 2 && be_isbytes(vm, 2);
    const bool bitplane = external && argc == 3;
    int w{}, h{}, bpp{}, serp{};
    std::size_t length{};
    const std::uint8_t* bits{};
    int stride{};
    if (bitplane) {
        stride = bounded(vm, 3, 1, std::numeric_limits<int>::max() / 8);
        bits = static_cast<const std::uint8_t*>(be_tobytes(vm, 2, &length));
        if (!length || length % static_cast<unsigned>(stride) ||
            length / static_cast<unsigned>(stride) >
                static_cast<unsigned>(std::numeric_limits<int>::max()))
            be_raise(vm, "value_error", "invalid bitplane dimensions");
        w = stride * 8;
        h = static_cast<int>(length / static_cast<unsigned>(stride));
        bpp = 1;
    } else {
        const int first = external ? 3 : 2;
        if (argc != first + 2 && argc != first + 3)
            be_raise(vm, "type_error", "pixmat([bytes,]width,height,bpp[,serpentine])");
        w = bounded(vm, first, 1, std::numeric_limits<int>::max());
        h = bounded(vm, first + 1, 1, std::numeric_limits<int>::max());
        bpp = bounded(vm, first + 2, 0, 4);
        if (argc == first + 3) {
            if (!be_isbool(vm, argc))
                be_raise(vm, "type_error", "serpentine expects bool");
            serp = be_tobool(vm, argc) ? 1 : 0;
            if (serp && !bpp)
                be_raise(vm, "value_error", "1-bit pixmat cannot be serpentine");
        }
    }
    const auto need = dimensions(vm, w, h, bpp);
    if (external && !bitplane) {
        (void)bytes(vm, 2, need, true);
        be_pushvalue(vm, 2);
    } else {
        be_pushbytes(vm, nullptr, need);
        auto* data = const_cast<std::uint8_t*>(bytes(vm, -1, need, true));
        if (bitplane) {
            /* Re-resolve source storage because allocation may invoke the collector. */
            bits = static_cast<const std::uint8_t*>(be_tobytes(vm, 2, &length));
            for (std::size_t i = 0; i < need; ++i)
                data[i] = (bits[i / 8] & (0x80U >> (i & 7U))) ? 255 : 0;
        }
    }
    be_setmember(vm, 1, ".buf");
    be_pop(vm, 1);
    member(vm, ".width", w);
    member(vm, ".height", h);
    member(vm, ".bpp", bpp);
    member(vm, ".serp", serp);
    be_return_nil(vm);
}

/** @brief Releases the instance's reference to its framebuffer. */
int Deinit(bvm* vm) {
    be_pushnil(vm);
    be_setmember(vm, 1, ".buf");
    be_pop(vm, 1);
    be_return_nil(vm);
}

/** @brief Returns the retained Berry bytes object without copying it. */
int Buffer(bvm* vm) {
    if (be_top(vm) != 1)
        be_raise(vm, "type_error", "buffer()");
    be_getmember(vm, 1, ".buf");
    be_return(vm);
}

/** @brief Returns the logical matrix width. */
int Width(bvm* vm) {
    be_pushint(vm, matrix(vm, 1, false).width);
    be_return(vm);
}

/** @brief Returns the logical matrix height. */
int Height(bvm* vm) {
    be_pushint(vm, matrix(vm, 1, false).height);
    be_return(vm);
}

/** @brief Reads a packed pixel, returning nil outside the matrix bounds. */
int Get(bvm* vm) {
    if (be_top(vm) != 3)
        be_raise(vm, "type_error", "get(x,y)");
    const auto x = integer(vm, 2), y = integer(vm, 3);
    const auto m = matrix(vm, 1, false);
    if (!inside(m, x, y))
        be_return_nil(vm);
    if (!m.bpp) {
        be_pushint(vm, m.bit(static_cast<int>(x), static_cast<int>(y)) ? 1 : 0);
        be_return(vm);
    }
    const auto* p = m.pixel(static_cast<int>(x), static_cast<int>(y));
    std::uint32_t color{};
    for (int c = 0; c < m.bpp; ++c)
        color = (color << 8) | p[c];
    be_pushint(vm, static_cast<bint>(static_cast<std::int32_t>(color)));
    be_return(vm);
}

/** @brief Writes a packed color or HSV value to one logical pixel. */
int Set(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 4 || argc > 7)
        be_raise(vm, "type_error", "set(x,y,color[,brightness]) or set(x,y,h,s,v[,brightness])");
    const auto x = integer(vm, 2), y = integer(vm, 3);
    const auto m = matrix(vm, 1, true);
    std::uint8_t p[4]{};
    const int bri = argc == 5 || argc == 7 ? bounded(vm, argc, 0, 255) : 255;
    if (!m.bpp) {
        bool on{};
        if (argc <= 5) {
            on = integer(vm, 4) != 0 && bri != 0;
        } else {
            (void)integer(vm, 4);
            (void)bounded(vm, 5, 0, 255);
            on = scale(static_cast<std::uint8_t>(bounded(vm, 6, 0, 255)), bri) != 0;
        }
        if (inside(m, x, y))
            m.put_bit(static_cast<int>(x), static_cast<int>(y), on);
        be_return_nil(vm);
    }
    if (argc <= 5)
        unpack(static_cast<std::uint32_t>(integer(vm, 4)), m.bpp, p);
    else {
        auto hue = integer(vm, 4) % 360;
        if (hue < 0)
            hue += 360;
        const int sat = bounded(vm, 5, 0, 255), val = bounded(vm, 6, 0, 255);
        const int h = static_cast<int>(hue) * 1536 / 360;
        const auto v = static_cast<std::uint8_t>(val);
        const auto low = scale(v, 255 - sat), q = scale(v, 255 - (sat * (h & 255) / 255));
        const auto t = scale(v, 255 - (sat * (255 - (h & 255)) / 255));
        if (m.bpp <= 2) {
            p[0] = v;
            p[1] = v;
        } else {
            const std::uint8_t rgb[6][3] = {
                {v, t, low}, {q, v, low}, {low, v, t}, {low, q, v}, {t, low, v}, {v, low, q}};
            // RGBW stores white last, initially zero.
            std::memcpy(p, rgb[h >> 8], 3);
        }
    }
    for (int c = 0; c < m.bpp; ++c)
        p[c] = scale(p[c], bri);
    if (inside(m, x, y))
        std::memcpy(
            m.pixel(static_cast<int>(x), static_cast<int>(y)), p, static_cast<std::size_t>(m.bpp));
    be_return_nil(vm);
}

/** @brief Fills every matrix byte with one value. */
int Clear(bvm* vm) {
    if (be_top(vm) > 2)
        be_raise(vm, "type_error", "clear([byte])");
    const int value = be_top(vm) == 2 ? bounded(vm, 2, 0, 255) : 0;
    const auto m = matrix(vm, 1, true);
    std::memset(m.data, value, m.size());
    be_return_nil(vm);
}

/** @brief Describes clipped source and destination rectangles for a blit. */
struct Region {
    int sx{}, sy{}, dx{}, dy{}, width{}, height{};
};

/** @brief Clips a source rectangle against the destination matrix bounds. */
Region clip(const Matrix& dest, int sw, int sh, bint dx, bint dy) {
    Region r;
    if (dx >= dest.width || dy >= dest.height || dx <= -static_cast<bint>(sw) ||
        dy <= -static_cast<bint>(sh))
        return r;
    r.sx = dx < 0 ? static_cast<int>(-dx) : 0;
    r.sy = dy < 0 ? static_cast<int>(-dy) : 0;
    r.dx = dx > 0 ? static_cast<int>(dx) : 0;
    r.dy = dy > 0 ? static_cast<int>(dy) : 0;
    r.width = std::min(sw - r.sx, dest.width - r.dx);
    r.height = std::min(sh - r.sy, dest.height - r.dy);
    return r;
}

/** @brief Blits a matrix or draws an MSB-first packed glyph with clipping. */
int Blit(bvm* vm) {
    const int argc = be_top(vm);
    const bool glyph = argc >= 2 && be_isbytes(vm, 2);
    if (glyph) {
        if (argc != 7 && argc != 8)
            be_raise(vm, "type_error", "blit(bytes,width,height,dx,dy,color[,stride])");
        const int w = bounded(vm, 3, 1, std::numeric_limits<int>::max());
        const int h = bounded(vm, 4, 1, std::numeric_limits<int>::max());
        const auto dx = integer(vm, 5), dy = integer(vm, 6);
        const auto color = static_cast<std::uint32_t>(integer(vm, 7));
        const int rowbytes = w / 8 + (w % 8 != 0 ? 1 : 0);
        const int stride =
            argc == 8 ? bounded(vm, 8, rowbytes, std::numeric_limits<int>::max()) : rowbytes;
        const auto required = static_cast<std::uint64_t>(h - 1) * static_cast<unsigned>(stride) +
                              static_cast<unsigned>(rowbytes);
        if (required > std::numeric_limits<std::size_t>::max())
            be_raise(vm, "value_error", "glyph size overflow");
        const auto m = matrix(vm, 1, true);
        const auto* bits = bytes(vm, 2, static_cast<std::size_t>(required), false);
        const auto a = reinterpret_cast<std::uintptr_t>(m.data),
                   b = reinterpret_cast<std::uintptr_t>(bits);
        if (a <= b ? b - a < m.size() : a - b < required)
            be_raise(vm, "value_error", "glyph and destination buffers overlap");
        newmota::pixmat_draw_glyph(m, bits, w, h, stride, dx, dy, color, nullptr);
        be_return_nil(vm);
    }
    if (argc < 4 || argc > 6)
        be_raise(vm, "type_error", "blit(pixmat,dx,dy[,brightness[,tint]])");
    const auto dx = integer(vm, 3), dy = integer(vm, 4);
    const int bri = argc >= 5 ? bounded(vm, 5, 0, 255) : 255;
    const bool tinted = argc == 6;
    const auto color = tinted ? static_cast<std::uint32_t>(integer(vm, 6)) : 0U;
    const auto dest = matrix(vm, 1, true), src = matrix(vm, 2, false);
    if (dest.bpp != src.bpp && (src.bpp > 1 || !dest.bpp))
        be_raise(vm, "value_error", "unsupported pixmat bpp conversion");
    const bool alias = overlap(dest, src);
    if (alias && (dest.data != src.data || dest.width != src.width || dest.height != src.height ||
                     dest.bpp != src.bpp || dest.serpentine != src.serpentine))
        be_raise(vm, "value_error", "overlapping pixmat views require identical layouts");
    const auto r = clip(dest, src.width, src.height, dx, dy);
    std::uint8_t tint[4]{}, p[4]{};
    unpack(color, dest.bpp, tint);
    const bool reverse_y = alias && r.dy > r.sy;
    const bool reverse_x = alias && r.dy == r.sy && r.dx > r.sx;
    const bool bit_on = bri != 0 && (!tinted || color != 0);
    for (int n = 0; n < r.height; ++n) {
        const int y = reverse_y ? r.height - 1 - n : n;
        const int sy = r.sy + y, ty = r.dy + y;
        const bool source_reversed = src.serpentine && (sy & 1);
        const bool target_reversed = dest.serpentine && (ty & 1);
        if (dest.bpp && src.bpp == dest.bpp && bri == 255 && !tinted &&
            source_reversed == target_reversed) {
            auto* out = dest.pixel(r.dx + (target_reversed ? r.width - 1 : 0), ty);
            const auto* in = src.pixel(r.sx + (source_reversed ? r.width - 1 : 0), sy);
            if (r.width) {
                const auto count = static_cast<std::size_t>(r.width) * dest.bpp;
                if (alias)
                    std::memmove(out, in, count);
                else
                    std::memcpy(out, in, count);
            }
            continue;
        }
        for (int k = 0; k < r.width; ++k) {
            const int x = reverse_x ? r.width - 1 - k : k;
            if (!dest.bpp) {
                dest.put_bit(r.dx + x, ty, bit_on && src.bit(r.sx + x, sy));
                continue;
            }
            std::uint8_t level{};
            const std::uint8_t* in = &level;
            if (src.bpp)
                in = src.pixel(r.sx + x, sy);
            else
                level = src.bit(r.sx + x, sy) ? 255 : 0;
            if (src.bpp <= 1 && dest.bpp != src.bpp && in[0] == 0)
                continue;
            for (int c = 0; c < dest.bpp; ++c) {
                p[c] = in[src.bpp <= 1 ? 0 : c];
                if (tinted)
                    p[c] = scale(p[c], tint[c]);
                p[c] = scale(p[c], bri);
            }
            std::memcpy(dest.pixel(r.dx + x, ty), p, static_cast<std::size_t>(dest.bpp));
        }
    }
    be_return_nil(vm);
}

/** @brief Scrolls one pixel, wrapping the edge or taking it from a source. */
int Scroll(bvm* vm) {
    const int argc = be_top(vm);
    if (argc < 2 || argc > 3)
        be_raise(vm, "type_error", "scroll(direction[,source])");
    const int dir = bounded(vm, 2, 0, 3);
    const auto d = matrix(vm, 1, true);
    const auto s = argc == 3 && !be_isnil(vm, 3) ? matrix(vm, 3, false) : d;
    if (d.width != s.width || d.height != s.height || d.bpp != s.bpp)
        be_raise(vm, "value_error", "scroll source dimensions and bpp must match");
    const bool alias = overlap(d, s);
    if (alias && (d.data != s.data || d.serpentine != s.serpentine))
        be_raise(vm, "value_error", "overlapping scroll views require identical layouts");
    const bool vertical = dir == 0 || dir == 2;
    const bool forward = dir < 2;
    const int length = vertical ? d.height : d.width;
    const int lines = vertical ? d.width : d.height;
    const int first = forward ? 0 : length - 1;
    const int source_edge = alias ? first : length - 1 - first;
    for (int line = 0; line < lines; ++line) {
        if (!d.bpp) {
            auto read = [&](const Matrix& m, int n) {
                return vertical ? m.bit(line, n) : m.bit(n, line);
            };
            auto write = [&](int n, bool on) {
                vertical ? d.put_bit(line, n, on) : d.put_bit(n, line, on);
            };
            const bool edge = read(s, source_edge);
            for (int n = 0; n < length - 1; ++n) {
                const int at = forward ? n : length - 1 - n;
                write(at, read(d, at + (forward ? 1 : -1)));
            }
            write(length - 1 - first, edge);
            continue;
        }
        auto point = [&](const Matrix& m, int n) {
            return vertical ? m.pixel(line, n) : m.pixel(n, line);
        };
        std::uint8_t edge[4]{};
        std::memcpy(edge, point(s, source_edge), static_cast<std::size_t>(d.bpp));
        for (int n = 0; n < length - 1; ++n) {
            const int at = forward ? n : length - 1 - n;
            std::memcpy(
                point(d, at), point(d, at + (forward ? 1 : -1)), static_cast<std::size_t>(d.bpp));
        }
        std::memcpy(point(d, length - 1 - first), edge, static_cast<std::size_t>(d.bpp));
    }
    be_return_nil(vm);
}

const bnfuncinfo methods[] = {{".buf", nullptr}, {".width", nullptr}, {".height", nullptr},
    {".bpp", nullptr}, {".serp", nullptr}, {"init", Init}, {"deinit", Deinit}, {"width", Width},
    {"height", Height}, {"get", Get}, {"set", Set}, {"clear", Clear}, {"blit", Blit},
    {"scroll", Scroll}, {"buffer", Buffer}, {nullptr, nullptr}};
} // namespace

namespace newmota {

PixmatBuffer pixmat_buffer(bvm* vm, int index, bool write) {
    return matrix(vm, index, write);
}

void pixmat_draw_glyph(const PixmatBuffer& target, const std::uint8_t* bits, int width, int height,
    int stride, std::int64_t x, std::int64_t y, std::uint32_t color,
    const std::uint32_t* background) {
    const auto r = clip(target, width, height, static_cast<bint>(x), static_cast<bint>(y));
    std::uint8_t on[4]{}, off[4]{};
    unpack(color, target.bpp, on);
    if (background)
        unpack(*background, target.bpp, off);
    for (int row = 0; row < r.height; ++row) {
        const auto* line = bits + static_cast<std::size_t>(r.sy + row) * stride;
        for (int column = 0; column < r.width; ++column) {
            const int sx = r.sx + column;
            const bool set = line[sx / 8] & (0x80U >> (sx & 7));
            if (!set && !background)
                continue;
            const int px = r.dx + column, py = r.dy + row;
            if (target.bpp)
                std::memcpy(
                    target.pixel(px, py), set ? on : off, static_cast<std::size_t>(target.bpp));
            else
                target.put_bit(px, py, (set ? color : *background) != 0);
        }
    }
}

/** @brief Registers the global Berry pixel matrix class. */
bool install_pixmat_module(bvm* vm) {
    be_pushclass(vm, "pixmat", methods);
    be_setglobal(vm, "pixmat");
    be_pop(vm, 1);
    return true;
}
} // namespace newmota
