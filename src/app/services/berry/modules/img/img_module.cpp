/*
  img_module.cpp - Berry img class for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file img_module.cpp
 * @brief Tasmota's Berry `img` class: one image in a native buffer that is kept across frames.
 *
 * The pixels never live in the Berry heap. The buffer only grows, so a frame of the same or a
 * smaller size reuses it and `info()['buf_addr']` stays valid; a larger one moves it. JPEG is
 * decoded with JPEGDEC into RGB565 (both byte orders), grayscale or RGB888. Encoding JPEG is
 * not supported.
 */
#include "services/berry/modules/img/img_module.h"

#include "services/berry/berry_service.h"
extern "C" {
#include "berry.h"
#include "be_object.h"
#include "be_string.h"
#include "be_gc.h"
#include "be_class.h"
#include "be_vm.h"
}
#include "JPEGDEC.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace newmota::img {
struct Image final {
    std::uint8_t* data{};
    std::size_t size{};     ///< Bytes holding the image.
    std::size_t capacity{}; ///< Bytes allocated; never shrinks while the instance lives.
    std::uint16_t width{}, height{};
    Format format{Format::jpeg};
    JPEGDEC* decoder{}; ///< About 22 KiB of decoder state, kept for the next decode.
};
} // namespace newmota::img

namespace newmota {
namespace {
using img::Format;
using img::Image;
using img::Result;

/** @brief JPEG stores round their allocation up, since frame sizes vary from frame to frame. */
constexpr std::size_t kJpegGranule = 4096;
/** @brief Decoded blocks between two deadline checks. */
constexpr unsigned kDeadlineCheckInterval = 32;
/** @brief Size of Tasmota's region-of-interest descriptor. */
constexpr std::size_t kRoiDescriptorSize = 24;

/** @brief Bytes per pixel of a raw format; 0 for JPEG. */
unsigned bytes_per_pixel(const Format format) {
    switch (format) {
    case Format::grayscale:
        return 1;
    case Format::rgb565:
    case Format::rgb565le:
        return 2;
    case Format::rgb888:
        return 3;
    case Format::jpeg:
        break;
    }
    return 0;
}

struct Rgb final {
    std::uint8_t r, g, b;
};

/** @brief Reads one pixel of a raw format as RGB. */
Rgb read_pixel(const std::uint8_t* p, const Format format) {
    switch (format) {
    case Format::grayscale:
        return {p[0], p[0], p[0]};
    case Format::rgb565:
    case Format::rgb565le: {
        const unsigned v =
            format == Format::rgb565 ? (unsigned(p[0]) << 8) | p[1] : (unsigned(p[1]) << 8) | p[0];
        const unsigned r = (v >> 11) & 0x1fU, g = (v >> 5) & 0x3fU, b = v & 0x1fU;
        return {std::uint8_t((r << 3) | (r >> 2)), std::uint8_t((g << 2) | (g >> 4)),
            std::uint8_t((b << 3) | (b >> 2))};
    }
    default:
        return {p[0], p[1], p[2]};
    }
}

/** @brief Writes one RGB pixel in a raw format; grayscale takes the BT.601 luma. */
void write_pixel(std::uint8_t* p, const Format format, const Rgb c) {
    switch (format) {
    case Format::grayscale:
        p[0] = std::uint8_t((77U * c.r + 150U * c.g + 29U * c.b + 128U) >> 8);
        return;
    case Format::rgb565:
    case Format::rgb565le: {
        const unsigned v = ((c.r & 0xf8U) << 8) | ((c.g & 0xfcU) << 3) | (c.b >> 3);
        p[format == Format::rgb565 ? 0 : 1] = std::uint8_t(v >> 8);
        p[format == Format::rgb565 ? 1 : 0] = std::uint8_t(v);
        return;
    }
    default:
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
    }
}

/** @brief Bytes of a raw image, or 0 when the product does not fit. */
std::size_t raw_size(const unsigned width, const unsigned height, const Format format) {
    const auto bytes = std::uint64_t(width) * height * bytes_per_pixel(format);
    return bytes > std::uint64_t(SIZE_MAX / 2) ? 0U : static_cast<std::size_t>(bytes);
}

/** @brief Tests whether @p data lies inside the image's own buffer. */
bool inside(const Image& image, const std::uint8_t* data) {
    return image.data != nullptr && data >= image.data && data < image.data + image.capacity;
}

/**
 * @brief Copies @p size bytes into the buffer, growing it first when needed.
 *
 * Data inside the buffer itself, such as a mapped `bytes` over `buf_addr`, moves in place.
 */
bool assign(Image& image, const std::uint8_t* data, const std::size_t size, std::size_t need) {
    if (inside(image, data)) {
        if (data + size > image.data + image.capacity)
            return false;
        std::memmove(image.data, data, size);
        return true;
    }
    if (need > image.capacity) {
        auto* const grown = static_cast<std::uint8_t*>(std::malloc(need));
        if (grown == nullptr)
            return false;
        std::free(image.data);
        image.data = grown;
        image.capacity = need;
    }
    std::memcpy(image.data, data, size);
    return true;
}

/**
 * @brief Reads the frame size from the first SOFn segment of a JPEG.
 *
 * Accepts any start-of-frame type, so progressive files can be stored, though not decoded.
 */
bool jpeg_size(const std::uint8_t* d, const std::size_t n, std::uint16_t& w, std::uint16_t& h) {
    if (n < 4U || d[0] != 0xffU || d[1] != 0xd8U)
        return false;
    std::size_t at = 2;
    while (at + 4U <= n) {
        if (d[at] != 0xffU)
            return false;
        const std::uint8_t marker = d[at + 1];
        if (marker == 0xffU) { // fill byte
            ++at;
            continue;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd8U)) {
            at += 2;
            continue;
        }
        if (marker == 0xd9U || marker == 0xdaU) // EOI or SOS before any frame header
            return false;
        const std::size_t length = (std::size_t(d[at + 2]) << 8) | d[at + 3];
        if (length < 2U || at + 2U + length > n)
            return false;
        const bool frame = marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U &&
                           marker != 0xc8U && marker != 0xccU;
        if (frame) {
            if (length < 7U)
                return false;
            h = std::uint16_t((d[at + 5] << 8) | d[at + 6]);
            w = std::uint16_t((d[at + 7] << 8) | d[at + 8]);
            return w != 0U && h != 0U;
        }
        at += 2U + length;
    }
    return false;
}

/** @brief Target and state of one JPEGDEC run. */
struct Decode final {
    std::uint8_t* target;
    unsigned width, height;
    Format format;
    unsigned blocks;
    bool timed_out, failed; ///< A stopped draw callback still lets decode() succeed.
};

/** @brief JPEGDEC pixel type for a raw format; RGB888 is decoded as RGB8888 and packed. */
int pixel_type(const Format format) {
    switch (format) {
    case Format::grayscale:
        return EIGHT_BIT_GRAYSCALE;
    case Format::rgb565:
        return RGB565_BIG_ENDIAN;
    case Format::rgb565le:
        return RGB565_LITTLE_ENDIAN;
    default:
        return RGB8888;
    }
}

/**
 * @brief JPEGDEC draw callback: copies one block of rows into the target.
 *
 * Rows are iWidth pixels apart in the block; iWidthUsed of them lie inside the image.
 * RGB8888 arrives as R, G, B, A bytes and loses the A byte.
 */
int jpeg_draw(JPEGDRAW* draw) {
    auto& d = *static_cast<Decode*>(draw->pUser);
    if (++d.blocks % kDeadlineCheckInterval == 0U && berry_execution_deadline_reached()) {
        d.timed_out = true;
        return 0;
    }
    if (draw->x < 0 || draw->y < 0 || draw->iWidthUsed < 0 || draw->iHeight < 0 ||
        unsigned(draw->x + draw->iWidthUsed) > d.width ||
        unsigned(draw->y + draw->iHeight) > d.height) {
        d.failed = true;
        return 0;
    }
    const unsigned bpp = bytes_per_pixel(d.format);
    const unsigned in = d.format == Format::rgb888 ? 4U : bpp;
    const auto* const pixels = reinterpret_cast<const std::uint8_t*>(draw->pPixels);
    for (int row = 0; row < draw->iHeight; ++row) {
        const auto* src = pixels + std::size_t(row) * unsigned(draw->iWidth) * in;
        auto* dst = d.target + (std::size_t(draw->y + row) * d.width + unsigned(draw->x)) * bpp;
        if (in == bpp) {
            std::memcpy(dst, src, std::size_t(draw->iWidthUsed) * bpp);
            continue;
        }
        for (int x = 0; x < draw->iWidthUsed; ++x, src += 4, dst += 3) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
        }
    }
    return 1;
}

/** @brief Pushes one raw image's bytes, raising above the Berry bytes limit. */
void push_bytes(bvm* vm, const std::uint8_t* data, const std::size_t size) {
    if (size > static_cast<std::size_t>(vm->bytesmaxsize))
        be_raise(vm, "value_error", "img data exceeds the Berry bytes limit");
    be_pushbytes(vm, data, size);
}

/** @brief Reads an integer argument within a range. */
int integer(bvm* vm, const int at, const int low, const int high) {
    if (!be_isint(vm, at))
        be_raise(vm, "type_error", "img expects integer arguments");
    const auto value = be_toint(vm, at);
    if (value < low || value > high)
        be_raise(vm, "value_error", "img argument out of range");
    return static_cast<int>(value);
}

/** @brief Reads a Berry bytes argument. */
const std::uint8_t* bytes_argument(bvm* vm, const int at, std::size_t& size) {
    if (!be_isbytes(vm, at))
        be_raise(vm, "type_error", "img expects bytes");
    return static_cast<const std::uint8_t*>(be_tobytes(vm, at, &size));
}

/** @brief Reads the optional format argument; nil or absent means JPEG. */
Format optional_format(bvm* vm, const int at) {
    return be_top(vm) >= at && !be_isnil(vm, at) ? img::format(vm, at) : Format::jpeg;
}

/** @brief `img()` allocates the native image; the buffer comes with the first data. */
int Init(bvm* vm) {
    auto* const image = new (std::nothrow) Image;
    if (image == nullptr)
        be_raise(vm, "memory_error", "img allocation failed");
    be_pushcomptr(vm, image);
    be_setmember(vm, 1, ".p");
    be_pop(vm, 1);
    be_return_nil(vm);
}

/** @brief Frees the buffer, the decoder and the native image. */
int Deinit(bvm* vm) {
    be_getmember(vm, 1, ".p");
    auto* const image = static_cast<Image*>(be_tocomptr(vm, -1));
    be_pop(vm, 1);
    if (image != nullptr) {
        std::free(image->data);
        delete image->decoder;
        delete image;
        be_pushcomptr(vm, nullptr);
        be_setmember(vm, 1, ".p");
        be_pop(vm, 1);
    }
    be_return_nil(vm);
}

/** @brief `from_jpg(jpeg [, format])` stores the JPEG, or decodes it into a raw format. */
int FromJpg(bvm* vm) {
    auto& image = img::instance(vm, 1);
    std::size_t size{};
    const auto* const data = bytes_argument(vm, 2, size);
    const auto format = optional_format(vm, 3);
    img::raise(vm, format == Format::jpeg ? img::store_jpeg(image, data, size)
                                          : img::decode_jpeg(image, data, size, format));
    be_return_nil(vm);
}

/** @brief `from_buffer(data, width, height, format)` copies raw pixels, or a JPEG as it is. */
int FromBuffer(bvm* vm) {
    auto& image = img::instance(vm, 1);
    std::size_t size{};
    const auto* const data = bytes_argument(vm, 2, size);
    const auto width = integer(vm, 3, 1, 0xffff), height = integer(vm, 4, 1, 0xffff);
    const auto format = img::format(vm, 5);
    if (format != Format::jpeg && size != raw_size(width, height, format))
        be_raise(vm, "value_error", "img data size does not match width, height and format");
    if (!assign(image, data, size, size))
        img::raise(vm, Result::no_memory);
    image.size = size;
    image.width = static_cast<std::uint16_t>(width);
    image.height = static_cast<std::uint16_t>(height);
    image.format = format;
    be_return_nil(vm);
}

/**
 * @brief `get_buffer([descriptor])` copies the image, or samples a region of interest.
 *
 * The 24-byte descriptor is Tasmota's: floats scale_x, shear_x, shear_y, scale_y, then uint16
 * translation_x, translation_y, width, height, little endian. Output pixel (x, y) is source
 * pixel (x * scale_x + y * shear_y + translation_x, x * shear_x + y * scale_y + translation_y),
 * truncated; samples outside the image are 0.
 */
int GetBuffer(bvm* vm) {
    auto& image = img::instance(vm, 1);
    if (be_top(vm) < 2 || be_isnil(vm, 2)) {
        push_bytes(vm, image.data, image.size);
        be_return(vm);
    }
    if (image.format == Format::jpeg)
        be_raise(vm, "value_error", "img region needs a raw format, not JPEG");
    std::size_t length{};
    const auto* const raw = bytes_argument(vm, 2, length);
    if (length != kRoiDescriptorSize)
        be_raise(vm, "value_error", "img region descriptor must be 24 bytes");
    float matrix[4];
    std::memcpy(matrix, raw, sizeof(matrix));
    std::uint16_t fields[4];
    for (int i = 0; i < 4; ++i)
        fields[i] = std::uint16_t(raw[16 + 2 * i] | (raw[17 + 2 * i] << 8));
    const float scale_x = matrix[0], shear_x = matrix[1], shear_y = matrix[2], scale_y = matrix[3];
    const unsigned width = fields[2], height = fields[3];
    const unsigned bpp = bytes_per_pixel(image.format);
    const std::size_t out = raw_size(width, height, image.format);
    if (out == 0U)
        be_raise(vm, "value_error", "img region is empty");
    if (out > static_cast<std::size_t>(vm->bytesmaxsize))
        be_raise(vm, "value_error", "img region exceeds the Berry bytes limit");
    be_pushbytes(vm, nullptr, out);
    std::size_t pushed{};
    auto* p = static_cast<std::uint8_t*>(const_cast<void*>(be_tobytes(vm, -1, &pushed)));
    if (p == nullptr || pushed < out)
        img::raise(vm, Result::no_memory);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x, p += bpp) {
            const auto sx = static_cast<long>(x * scale_x + y * shear_y + fields[0]);
            const auto sy = static_cast<long>(x * shear_x + y * scale_y + fields[1]);
            if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height) {
                std::memset(p, 0, bpp);
                continue;
            }
            std::memcpy(
                p, image.data + (std::size_t(sy) * image.width + std::size_t(sx)) * bpp, bpp);
        }
    }
    be_return(vm);
}

/**
 * @brief `convert_to(format)` converts in place; the same format does nothing.
 *
 * JPEG decodes into the raw format; raw formats convert pixel by pixel and keep the buffer
 * when the result fits. Encoding to JPEG raises.
 */
int ConvertTo(bvm* vm) {
    auto& image = img::instance(vm, 1);
    const auto format = img::format(vm, 2);
    if (image.size == 0U)
        be_raise(vm, "value_error", "img has no data");
    if (format == image.format)
        be_return_nil(vm);
    if (format == Format::jpeg)
        be_raise(vm, "value_error", "img cannot encode JPEG");
    if (image.format == Format::jpeg) {
        img::raise(vm, img::decode_jpeg(image, image.data, image.size, format));
        be_return_nil(vm);
    }
    const unsigned from = bytes_per_pixel(image.format), to = bytes_per_pixel(format);
    const std::size_t pixels = std::size_t(image.width) * image.height;
    const std::size_t need = pixels * to;
    std::uint8_t* target = image.data;
    if (need > image.capacity) {
        target = static_cast<std::uint8_t*>(std::malloc(need));
        if (target == nullptr)
            img::raise(vm, Result::no_memory);
    }
    // In place, a narrower result runs forward and a wider one backward, so each pixel is
    // read before any write reaches it.
    if (to <= from || target != image.data) {
        for (std::size_t i = 0; i < pixels; ++i)
            write_pixel(target + i * to, format, read_pixel(image.data + i * from, image.format));
    } else {
        for (std::size_t i = pixels; i-- > 0;)
            write_pixel(target + i * to, format, read_pixel(image.data + i * from, image.format));
    }
    if (target != image.data) {
        std::free(image.data);
        image.data = target;
        image.capacity = need;
    }
    image.size = need;
    image.format = format;
    be_return_nil(vm);
}

/** @brief `info()` returns buf_addr, size, width, height, bpp and format. */
int Info(bvm* vm) {
    const auto& image = img::instance(vm, 1);
    be_newobject(vm, "map");
    const auto insert = [vm](const char* key, const bint value) {
        be_pushstring(vm, key);
        be_pushint(vm, value);
        be_data_insert(vm, -3);
        be_pop(vm, 2);
    };
    insert("buf_addr", static_cast<bint>(reinterpret_cast<std::uintptr_t>(image.data)));
    insert("size", static_cast<bint>(image.size));
    insert("width", image.width);
    insert("height", image.height);
    insert("bpp", bytes_per_pixel(image.format));
    insert("format", static_cast<bint>(image.format));
    be_pop(vm, 1);
    be_return(vm);
}

const bnfuncinfo methods[] = {{".p", nullptr}, {"init", Init}, {"deinit", Deinit},
    {"from_jpg", FromJpg}, {"from_buffer", FromBuffer}, {"get_buffer", GetBuffer},
    {"convert_to", ConvertTo}, {"info", Info}, {nullptr, nullptr}};

struct Constant final {
    const char* name;
    Format value;
};

constexpr Constant kConstants[] = {{"RGB565", Format::rgb565}, {"RGB565LE", Format::rgb565le},
    {"GRAYSCALE", Format::grayscale}, {"JPEG", Format::jpeg}, {"RGB888", Format::rgb888}};
} // namespace

namespace img {
Image& instance(bvm* vm, const int index) {
    bool valid = be_isinstance(vm, index);
    if (valid) {
        be_getglobal(vm, "img");
        valid = be_isderived(vm, index);
        be_pop(vm, 1);
    }
    if (!valid)
        be_raise(vm, "type_error", "expected img instance");
    be_getmember(vm, index, ".p");
    auto* const image = static_cast<Image*>(be_tocomptr(vm, -1));
    be_pop(vm, 1);
    if (image == nullptr)
        be_raise(vm, "value_error", "img is closed");
    return *image;
}

Format format(bvm* vm, const int index) {
    const auto value = integer(vm, index, 0, 5);
    if (value == 2)
        be_raise(vm, "value_error", "unknown img format");
    return static_cast<Format>(value);
}

Result store_jpeg(Image& image, const std::uint8_t* data, const std::size_t size) {
    std::uint16_t width{}, height{};
    if (!jpeg_size(data, size, width, height))
        return Result::invalid_jpeg;
    if (!assign(image, data, size, (size + kJpegGranule - 1U) / kJpegGranule * kJpegGranule))
        return Result::no_memory;
    image.size = size;
    image.width = width;
    image.height = height;
    image.format = Format::jpeg;
    return Result::ok;
}

Result decode_jpeg(
    Image& image, const std::uint8_t* data, const std::size_t size, const Format format) {
    if (format == Format::jpeg)
        return store_jpeg(image, data, size);
    if (size > std::size_t(INT32_MAX))
        return Result::invalid_jpeg;
    if (image.decoder == nullptr && (image.decoder = new (std::nothrow) JPEGDEC) == nullptr)
        return Result::no_memory;
    auto& jpeg = *image.decoder;
    // JPEGDEC only reads through this pointer.
    if (!jpeg.openRAM(const_cast<std::uint8_t*>(data), static_cast<int>(size), jpeg_draw))
        return jpeg.getLastError() == JPEG_UNSUPPORTED_FEATURE ? Result::unsupported_jpeg
                                                               : Result::invalid_jpeg;
    // A progressive JPEG would only give its first scan, at an eighth of the size.
    const unsigned width = unsigned(jpeg.getWidth()), height = unsigned(jpeg.getHeight());
    if (jpeg.getJPEGType() != JPEG_MODE_BASELINE || width == 0U || height == 0U ||
        width > 0xffffU || height > 0xffffU) {
        jpeg.close();
        return Result::unsupported_jpeg;
    }
    const std::size_t need = raw_size(width, height, format);
    if (need == 0U) {
        jpeg.close();
        return Result::unsupported_jpeg;
    }
    // Decoding its own JPEG needs a second buffer until the source is consumed.
    const bool own = inside(image, data);
    std::uint8_t* target = image.data;
    if (own || need > image.capacity) {
        target = static_cast<std::uint8_t*>(std::malloc(need));
        if (target == nullptr) {
            jpeg.close();
            return Result::no_memory;
        }
    }
    Decode d{target, width, height, format, 0, false, false};
    jpeg.setUserPointer(&d);
    jpeg.setPixelType(pixel_type(format));
    const bool decoded = jpeg.decode(0, 0, 0) == 1 && !d.timed_out && !d.failed;
    jpeg.close();
    if (target != image.data) {
        if (!decoded) {
            std::free(target);
            return d.timed_out ? Result::timeout : Result::invalid_jpeg;
        }
        std::free(image.data);
        image.data = target;
        image.capacity = need;
    }
    image.size = need;
    image.width = static_cast<std::uint16_t>(width);
    image.height = static_cast<std::uint16_t>(height);
    image.format = format;
    if (!decoded)
        return d.timed_out ? Result::timeout : Result::invalid_jpeg;
    return Result::ok;
}

void raise(bvm* vm, const Result result) {
    switch (result) {
    case Result::ok:
        return;
    case Result::no_memory:
        be_raise(vm, "memory_error", "img buffer allocation failed");
        break;
    case Result::invalid_jpeg:
        be_raise(vm, "value_error", "img: invalid JPEG");
        break;
    case Result::unsupported_jpeg:
        be_raise(vm, "value_error", "img: unsupported JPEG, only baseline decodes");
        break;
    case Result::timeout:
        be_raise(vm, "timeout_error", "Berry code running for too long");
        break;
    }
}
} // namespace img

bool install_img_module(bvm* vm) {
    be_pushclass(vm, "img", methods);
    auto* const type = static_cast<bclass*>(var_toobj(be_indexof(vm, -1)));
    for (const auto& constant : kConstants) {
        be_class_member_bind(vm, type, be_newstr(vm, constant.name), bfalse);
        be_pushint(vm, static_cast<bint>(constant.value));
        be_setmember(vm, -2, constant.name);
        be_pop(vm, 1);
    }
    be_setglobal(vm, "img");
    be_pop(vm, 1);
    return true;
}
} // namespace newmota
