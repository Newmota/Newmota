/*
  descriptor.h - validated UDisplay descriptor model

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file descriptor.h
 * @brief Hardware independent, constructed display configuration.
 */
#pragma once
#include "newmota/platform/display.h"
#include "newmota_features.h"
#if USE_UNIVERSAL_TOUCH
#include "touch.h"
#endif
#include <array>
#include <string_view>
#include <new>
#include <utility>

namespace newmota::platform::display::detail {
/**
 * @brief Checked owning storage for descriptor records with exceptions disabled.
 * @tparam T Constructed element type; moving an element must not allocate.
 * @tparam Limit Maximum number of elements accepted from one descriptor table.
 *
 * Allocation is explicit and fallible. Reallocation move-assigns already
 * constructed elements and preserves ownership of nested buffers.
 */
template <typename T, std::size_t Limit = 8192> class Buffer {
  public:
    Buffer() = default;

    /** @brief Releases constructed elements, including nested command buffers. */
    ~Buffer() { delete[] values_; }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    /** @brief Transfers array ownership without copying elements or allocating. */
    Buffer(Buffer&& other) noexcept
        : values_(std::exchange(other.values_, nullptr)), size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            delete[] values_;
            values_ = std::exchange(other.values_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    /** @brief Appends one value, preserving the existing contents on failure. */
    [[nodiscard]] bool push_back(T value) {
        if (size_ == Limit)
            return false;
        if (size_ == capacity_) {
            const auto next = capacity_ ? (capacity_ > Limit / 2 ? Limit : capacity_ * 2)
                                        : (Limit < 16 ? Limit : 16);
            T* allocation = new (std::nothrow) T[next];
            if (!allocation)
                return false;
            for (std::size_t i = 0; i < size_; ++i)
                allocation[i] = std::move(values_[i]);
            delete[] values_;
            values_ = allocation;
            capacity_ = next;
        }
        values_[size_++] = std::move(value);
        return true;
    }

    [[nodiscard]] std::size_t size() const { return size_; }

    [[nodiscard]] bool empty() const { return size_ == 0; }

    [[nodiscard]] T* data() { return values_; }

    [[nodiscard]] const T* data() const { return values_; }

    [[nodiscard]] T* begin() { return data(); }

    [[nodiscard]] const T* begin() const { return data(); }

    [[nodiscard]] T* end() { return size_ ? data() + size_ : data(); }

    [[nodiscard]] const T* end() const { return size_ ? data() + size_ : data(); }

    T& operator[](std::size_t index) { return values_[index]; }

    const T& operator[](std::size_t index) const { return values_[index]; }

  private:
    T* values_{};
    std::size_t size_{}, capacity_{};
};

using Bytes = Buffer<std::uint8_t>;

enum class Bus { spi, i2c, epd, rgb, i80, dsi, qspi };

struct Lut {
    std::uint8_t command{0x32};
    std::size_t declared{};
    Bytes bytes;
};

struct Config {
    Info info{};
#if USE_UNIVERSAL_TOUCH
    TouchConfig touch;
#endif
    Bus bus{Bus::spi};
    int reset{-1}, backlight{-1}, busy{-1};
    int spi_bus{1}, cs{-1}, clock{-1}, mosi{-1}, dc{-1}, miso{-1}, speed{10};
    std::array<int, 4> quad_pins{{-1, -1, -1, -1}};
    int i2c_bus{}, address{}, scl{-1}, sda{-1};
    int data_width{8}, wr{-1}, rd{-1};
    std::array<int, 16> data_pins{};
    int de{-1}, vsync{-1}, hsync{-1}, pclk{-1};
    std::array<int, 9> timing{};
    int lanes{2}, te{-1}, ldo{-1}, voltage{}, pixel_clock{}, lane_speed{}, rgb_order{}, endian{};
    int init_kind{}; // RGB 1=software SPI, 2=shared I2C.
    int init_clock{-1}, init_mosi{-1}, init_cs{-1}, init_reset{-1}, init_bus{}, init_address{};
    Buffer<Bytes, 4096> init_lines;
    Bytes init, full, partial;
    std::array<Lut, 7> luts{}; // Full, partial, then five independent tables.
    int epd_mode{}, full_time{350}, partial_time{35}, update_time{10};
    std::uint8_t on{0xff}, off{0xff}, inv_on{0xff}, inv_off{0xff}, madctl{0xff}, startline{0xff},
        dim{0xff};
    std::uint8_t addr_x{0xff}, addr_y{0xff}, ram{0xff};
    std::uint16_t address_mode{16}, color_mode{16};
    std::array<std::uint8_t, 4> rot{};
    std::array<std::uint16_t, 4> x_offset{}, y_offset{};
    std::uint8_t page_start{}, page_end{}, col_start{}, col_end{}, pack_width{}, pack_height{},
        pack_flags{}, backlight_flags{};
    std::array<std::uint8_t, 4> ram_bounds{};
    bool all_commands{}, inverted_mono{};

    Config() { data_pins.fill(-1); }
};

/** @brief Parses bounded descriptor text and validates every command/table span. */
Result parse(const char*, std::size_t, Config&, char*, std::size_t);
/** @brief Validates native capabilities and describes the pin reservation set. */
Result preflight(Config&, char*, std::size_t);
} // namespace newmota::platform::display::detail
