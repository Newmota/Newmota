/*
  audio_ring.h - Single-producer single-consumer byte ring

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_ring.h
 * @brief Carries encoded or PCM bytes between an audio worker and the application loop.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::audio {
/** @brief Contiguous readable bytes at the ring's consumer end. */
struct RingSpan {
    const std::uint8_t* data{};
    std::size_t size{};
};

/** @brief Lock-free byte ring for exactly one producer and one consumer.
 * Capacity is a power of two so the free-running 32-bit counters stay consistent when
 * they wrap. Producers copy with write(); consumers read peek() and then consume().
 */
class Ring {
  public:
    /** @brief Allocates storage; capacity must be a power of two no larger than 2^31. */
    bool allocate(std::size_t capacity) {
        if (!capacity || (capacity & (capacity - 1U)) || capacity > (std::size_t{1} << 31U))
            return false;
        storage_.reset(new (std::nothrow) std::uint8_t[capacity]);
        capacity_ = storage_ ? capacity : 0;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        return storage_ != nullptr;
    }

    std::size_t capacity() const { return capacity_; }

    /** @brief Bytes queued for the consumer. Either side may call this. */
    std::size_t size() const {
        return static_cast<std::uint32_t>(
            head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
    }

    /** @brief Bytes the producer can still write. */
    std::size_t free() const { return capacity_ - size(); }

    bool empty() const { return size() == 0U; }

    /** @brief Producer: copies up to `size` bytes, wrapping as needed.
     * @return Bytes written, which is less than `size` only when the ring is full.
     */
    std::size_t write(const std::uint8_t* data, std::size_t size) {
        const auto count = std::min(size, free());
        const auto head = head_.load(std::memory_order_relaxed);
        const std::size_t offset = head & (capacity_ - 1U);
        const auto first = std::min(count, capacity_ - offset);
        std::memcpy(storage_.get() + offset, data, first);
        std::memcpy(storage_.get(), data + first, count - first);
        head_.store(head + static_cast<std::uint32_t>(count), std::memory_order_release);
        return count;
    }

    /** @brief Consumer: returns at most `limit` contiguous bytes without removing them. */
    RingSpan peek(std::size_t limit) const {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const std::size_t offset = tail & (capacity_ - 1U);
        const std::size_t available =
            static_cast<std::uint32_t>(head_.load(std::memory_order_acquire) - tail);
        const std::size_t contiguous = capacity_ - offset;
        return {storage_.get() + offset, std::min({available, contiguous, limit})};
    }

    /** @brief Consumer: releases bytes previously returned by peek(). */
    void consume(std::size_t count) {
        tail_.store(tail_.load(std::memory_order_relaxed) + static_cast<std::uint32_t>(count),
            std::memory_order_release);
    }

  private:
    std::unique_ptr<std::uint8_t[]> storage_{};
    std::size_t capacity_{};
    std::atomic<std::uint32_t> head_{0}, tail_{0};
};
} // namespace newmota::audio
