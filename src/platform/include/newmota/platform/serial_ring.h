/*
  serial_ring.h - Fixed-capacity serial receive ring

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file serial_ring.h
 * @brief Configured UART ownership and bounded receive storage.
 */
#pragma once
#include "newmota/platform/serial.h"
#include <atomic>
#include <new>

namespace newmota::platform::serial {
// One IRQ producer, one app consumer. Drop newest bytes when full. Memory is
// allocated before enabling the IRQ and released only after disabling it.
class RxRing {
    std::uint8_t* data_{};
    std::size_t capacity_{}, write_position_{}, read_position_{};
    std::atomic<std::uint32_t> head_{0}, tail_{0}, dropped_{0}, errors_{0};

  public:
    /** @brief Allocates a bounded ring before enabling its capture IRQ. */
    bool allocate(std::size_t capacity) {
        if (!capacity || capacity > kMaximumRxCapacity || data_)
            return false;
        data_ = new (std::nothrow) std::uint8_t[capacity];
        capacity_ = capacity;
        write_position_ = read_position_ = 0;
        head_ = 0;
        tail_ = 0;
        dropped_ = 0;
        errors_ = 0;
        return data_ != nullptr;
    }

    /** @brief Frees ring storage after the producer IRQ has been disabled. */
    void release() {
        delete[] data_;
        data_ = nullptr;
    }

    /** @brief Captures one byte from the sole IRQ producer, dropping newest bytes if full. */
    void push(std::uint8_t byte) {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= capacity_) {
            dropped_.store(dropped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return;
        }
        data_[write_position_] = byte;
        write_position_ = (write_position_ + 1) % capacity_;
        head_.store(head + 1, std::memory_order_release);
    }

    /** @brief Records one native error from the sole capture producer. */
    void error() {
        errors_.store(errors_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    /** @brief Returns the currently buffered RX byte count without waiting. */
    std::size_t available() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }

    /** @brief Peeks or consumes currently buffered bytes without waiting for input. */
    Transfer read(std::uint8_t* data, std::size_t size, bool peek) {
        if (!data_ || (!data && size))
            return {Result::invalid_argument, 0};
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto pending = available();
        const auto count = pending < size ? pending : size;
        for (std::size_t i = 0; i < count; ++i)
            data[i] = data_[(read_position_ + i) % capacity_];
        if (!peek) {
            read_position_ = (read_position_ + count) % capacity_;
            tail_.store(tail + static_cast<std::uint32_t>(count), std::memory_order_release);
        }
        return {Result::success, count};
    }

    /** @brief Advances the consumer to the current producer position. */
    void discard() {
        if (!data_)
            return;
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_relaxed);
        read_position_ = (read_position_ + static_cast<std::uint32_t>(head - tail)) % capacity_;
        tail_.store(head, std::memory_order_release);
    }

    /** @brief Returns cumulative dropped-byte and native-error counters. */
    Status status() const {
        return {dropped_.load(std::memory_order_relaxed), errors_.load(std::memory_order_relaxed)};
    }
};
} // namespace newmota::platform::serial
