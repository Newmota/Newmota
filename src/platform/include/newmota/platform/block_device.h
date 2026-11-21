/*
  block_device.h - portable flash block-device interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota::platform {

class BlockDevice final {
  public:
    using Read = bool (*)(
        const void* context, std::uint32_t offset, void* buffer, std::size_t size);
    using Program = bool (*)(
        const void* context, std::uint32_t offset, const void* buffer, std::size_t size);
    using Erase = bool (*)(const void* context, std::uint32_t offset, std::size_t size);
    using Sync = bool (*)(const void* context);

    /** @brief Creates an empty block-device handle. */
    constexpr BlockDevice() = default;

    /**
     * @brief Binds geometry and operations for a bounded storage region.
     */
    constexpr BlockDevice(const void* context, const std::uint32_t size,
        const std::uint32_t read_size, const std::uint32_t program_size,
        const std::uint32_t erase_size, Read read, Program program, Erase erase, Sync sync)
        : context_(context), size_(size), read_size_(read_size), program_size_(program_size),
          erase_size_(erase_size), read_(read), program_(program), erase_(erase), sync_(sync) {}

    /** @brief Validates callbacks, geometry, and operation alignment. */
    [[nodiscard]] constexpr bool valid() const {
        return context_ != nullptr && read_ != nullptr && program_ != nullptr &&
               erase_ != nullptr && sync_ != nullptr && size_ != 0 && read_size_ != 0 &&
               program_size_ != 0 && erase_size_ != 0 && size_ % erase_size_ == 0 &&
               erase_size_ % read_size_ == 0 && erase_size_ % program_size_ == 0;
    }

    /** @brief Reads an aligned range into @p buffer. */
    [[nodiscard]] bool read(std::uint32_t offset, void* buffer, std::size_t size) const {
        return read_ != nullptr && valid_range(offset, size, read_size_) && buffer != nullptr &&
               read_(context_, offset, buffer, size);
    }

    /** @brief Programs an aligned range from @p buffer. */
    [[nodiscard]] bool program(std::uint32_t offset, const void* buffer, std::size_t size) const {
        return program_ != nullptr && valid_range(offset, size, program_size_) &&
               buffer != nullptr && program_(context_, offset, buffer, size);
    }

    /** @brief Erases the block beginning at @p offset. */
    [[nodiscard]] bool erase(std::uint32_t offset) const {
        return erase_ != nullptr && valid_range(offset, erase_size_, erase_size_) &&
               erase_(context_, offset, erase_size_);
    }

    /** @brief Flushes pending block-device operations. */
    [[nodiscard]] bool sync() const {
        return context_ != nullptr && sync_ != nullptr && sync_(context_);
    }

    /** @brief Returns the region size in bytes. */
    [[nodiscard]] constexpr std::uint32_t size() const { return size_; }

    /** @brief Returns the minimum read alignment in bytes. */
    [[nodiscard]] constexpr std::uint32_t read_size() const { return read_size_; }

    /** @brief Returns the minimum program alignment in bytes. */
    [[nodiscard]] constexpr std::uint32_t program_size() const { return program_size_; }

    /** @brief Returns the erase-block size in bytes. */
    [[nodiscard]] constexpr std::uint32_t erase_size() const { return erase_size_; }

  private:
    /** @brief Checks bounds and alignment for one device operation. */
    [[nodiscard]] constexpr bool valid_range(
        const std::uint32_t offset, const std::size_t size, const std::uint32_t alignment) const {
        return alignment != 0 && size != 0 && offset % alignment == 0 && size % alignment == 0 &&
               offset <= size_ && size <= size_ - offset;
    }

    const void* context_{nullptr};
    std::uint32_t size_{0};
    std::uint32_t read_size_{0};
    std::uint32_t program_size_{0};
    std::uint32_t erase_size_{0};
    Read read_{nullptr};
    Program program_{nullptr};
    Erase erase_{nullptr};
    Sync sync_{nullptr};
};

/**
 * @brief Opens the platform filesystem storage region as a block device.
 * @return true when @p device was populated with verified geometry.
 */
[[nodiscard]] bool open_filesystem_block_device(BlockDevice& device);

} // namespace newmota::platform
