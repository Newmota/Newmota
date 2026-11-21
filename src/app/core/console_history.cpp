/*
  console_history.cpp - bounded browser console output histories

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/console_history.h"

#include "newmota/log.h"
#include "newmota/platform/platform.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace newmota::console_history {
namespace {

constexpr std::size_t kRingBytes = 2048U;
/** @brief sequence (4), group (4), kind (1), text size (1). */
constexpr std::size_t kHeaderBytes = 10U;
constexpr std::uint32_t kBootWindowMilliseconds = 5U * 60U * 1000U;
constexpr std::uint32_t kViewerLeaseMilliseconds = 30U * 1000U;
static_assert(kWebLineCapacity <= std::numeric_limits<std::uint8_t>::max());

/** @brief Stored kinds; berry entries form the Berry Console stream. */
enum class StoredKind : std::uint8_t { log, command_result, berry };
enum class Stream : std::uint8_t { web, berry };
/** @brief boot: collecting the boot log; viewer: a console polls; idle: nothing retained. */
enum class Phase : std::uint8_t { boot, viewer, idle };

/** @brief Variable-length entries in one circular byte buffer, oldest at begin. */
struct Ring final {
    std::array<std::uint8_t, kRingBytes> bytes{};
    std::size_t begin{};
    std::size_t used{};
};

struct Header final {
    std::uint32_t sequence{};
    std::uint32_t group{};
    StoredKind kind{StoredKind::log};
    std::uint8_t size{};
};

// Application task only, like AddLog: no lock.
std::unique_ptr<Ring> ring;
Phase phase = Phase::boot;
std::uint32_t next_sequence = 0U;
/** @brief Newest sequence per stream that a viewer can no longer read. */
std::array<std::uint32_t, 2> lost{};
std::uint32_t last_poll_at = 0U;

[[nodiscard]] Stream stream_of(const StoredKind kind) {
    return kind == StoredKind::berry ? Stream::berry : Stream::web;
}

void copy_in(const std::size_t offset, const void* const data, const std::size_t size) {
    const auto* const source = static_cast<const std::uint8_t*>(data);
    const auto start = offset % kRingBytes;
    const auto first = std::min(size, kRingBytes - start);
    std::memcpy(ring->bytes.data() + start, source, first);
    std::memcpy(ring->bytes.data(), source + first, size - first);
}

void copy_out(const std::size_t offset, void* const data, const std::size_t size) {
    auto* const target = static_cast<std::uint8_t*>(data);
    const auto start = offset % kRingBytes;
    const auto first = std::min(size, kRingBytes - start);
    std::memcpy(target, ring->bytes.data() + start, first);
    std::memcpy(target + first, ring->bytes.data(), size - first);
}

[[nodiscard]] Header read_header(const std::size_t offset) {
    std::array<std::uint8_t, kHeaderBytes> raw{};
    copy_out(offset, raw.data(), raw.size());
    Header header{};
    std::memcpy(&header.sequence, raw.data(), 4U);
    std::memcpy(&header.group, raw.data() + 4U, 4U);
    header.kind = static_cast<StoredKind>(raw[8]);
    header.size = raw[9];
    return header;
}

void evict_oldest() {
    const auto header = read_header(ring->begin);
    lost[static_cast<std::size_t>(stream_of(header.kind))] = header.sequence;
    const auto entry = kHeaderBytes + header.size;
    ring->begin = (ring->begin + entry) % kRingBytes;
    ring->used -= entry;
}

/** @brief Frees the ring; every line recorded so far is lost to later viewers. */
void release() {
    if (!ring)
        return;
    ring.reset();
    lost = {next_sequence, next_sequence};
}

void record(const StoredKind kind, const std::uint32_t group, const std::string_view text) {
    if (!ring) {
        // Without a boot window or a viewer nobody can read the line.
        if (phase != Phase::boot)
            return;
        ring.reset(new (std::nothrow) Ring{});
        if (!ring) {
            phase = Phase::idle;
            return;
        }
    }
    if (next_sequence == std::numeric_limits<std::uint32_t>::max()) {
        // A viewer cursor above next_sequence reads as a reset.
        ring->begin = ring->used = 0U;
        next_sequence = 0U;
        lost = {};
    }
    std::array<char, kWebLineCapacity> line{};
    const auto size = std::min(text.size(), line.size());
    std::copy_n(text.data(), size, line.data());
    for (std::size_t index = 0U; index < size; ++index) {
        const auto character = static_cast<unsigned char>(line[index]);
        if (character < 0x20U || character == 0x7FU)
            line[index] = ' ';
    }
    const auto entry = kHeaderBytes + size;
    while (kRingBytes - ring->used < entry)
        evict_oldest();
    const auto sequence = ++next_sequence;
    std::array<std::uint8_t, kHeaderBytes> raw{};
    std::memcpy(raw.data(), &sequence, 4U);
    std::memcpy(raw.data() + 4U, &group, 4U);
    raw[8] = static_cast<std::uint8_t>(kind);
    raw[9] = static_cast<std::uint8_t>(size);
    const auto end = ring->begin + ring->used;
    copy_in(end, raw.data(), raw.size());
    copy_in(end + kHeaderBytes, line.data(), size);
    ring->used += entry;
}

[[nodiscard]] ReadResult read(const Stream stream, const std::uint32_t requested_cursor,
    const std::size_t max_events, const EventWriter writer, void* const context) {
    ReadResult result{};
    auto cursor = requested_cursor;
    if (cursor > next_sequence) {
        cursor = 0U;
        result.reset = true;
    }
    const auto lost_until = lost[static_cast<std::size_t>(stream)];
    if (lost_until > cursor) {
        cursor = lost_until;
        result.dropped = true;
    }
    result.next = cursor;
    if (!ring)
        return result;
    std::array<char, kWebLineCapacity> text{};
    std::size_t offset = ring->begin;
    std::size_t remaining = ring->used;
    while (remaining != 0U) {
        const auto header = read_header(offset);
        if (stream_of(header.kind) == stream && header.sequence > cursor) {
            if (result.count >= max_events || writer == nullptr) {
                result.more = true;
                break;
            }
            copy_out(offset + kHeaderBytes, text.data(), header.size);
            // The Berry Console shows its lines as plain log items.
            const auto kind = header.kind == StoredKind::command_result ? OutputKind::command_result
                                                                        : OutputKind::log;
            if (!writer(context, header.sequence, kind, header.group, {text.data(), header.size})) {
                result.more = true;
                break;
            }
            result.next = header.sequence;
            ++result.count;
        }
        const auto entry = kHeaderBytes + header.size;
        offset += entry;
        remaining -= entry;
    }
    return result;
}

} // namespace

void record_web_output(
    const std::string_view text, const OutputKind kind, const std::uint32_t group) {
    record(kind == OutputKind::command_result ? StoredKind::command_result : StoredKind::log, group,
        text);
}

void record_berry_output(const std::string_view text) {
    record(StoredKind::berry, 0U, text);
}

bool attach_viewer() {
    if (!ring) {
        ring.reset(new (std::nothrow) Ring{});
        if (!ring)
            return false;
    }
    phase = Phase::viewer;
    last_poll_at = platform::monotonic_milliseconds();
    return true;
}

void maintain(const std::uint32_t now) {
    if (phase == Phase::boot) {
        if (log_detail::web_log_level == 0U || now >= kBootWindowMilliseconds) {
            release();
            phase = Phase::idle;
        }
    } else if (phase == Phase::viewer && now - last_poll_at >= kViewerLeaseMilliseconds) {
        release();
        phase = Phase::idle;
    }
}

ReadResult read_web_output(const std::uint32_t cursor, const std::size_t max_events,
    const EventWriter writer, void* const context) {
    return read(Stream::web, cursor, max_events, writer, context);
}

ReadResult read_berry_output(const std::uint32_t cursor, const std::size_t max_events,
    const EventWriter writer, void* const context) {
    return read(Stream::berry, cursor, max_events, writer, context);
}

} // namespace newmota::console_history
