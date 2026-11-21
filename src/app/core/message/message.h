/*
  message.h - canonical JSON messages and bounded routing for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

/** @brief Selects the delivery policy used for one canonical message. */
enum class MessageKind : std::uint8_t {
    snapshot,
    response,
};

/**
 * @brief Routing metadata kept outside the Tasmota-compatible JSON payload.
 *
 * Source and device text is copied into the bounded metadata record.  A device
 * identity is optional until the platform has a qualified unique identifier.
 */
struct MessageMetadata final {
    static constexpr std::size_t kSourceCapacity = 32;
    static constexpr std::size_t kDeviceCapacity = 32;

    MessageKind kind{MessageKind::snapshot};
    std::uint16_t schema_version{1U};
    std::uint32_t sequence{};
    /**
     * Producer-owned semantic revision for a latest-value snapshot.
     *
     * The router sequence changes whenever a snapshot is refreshed. This
     * revision changes only when the producer considers the represented state
     * changed, allowing consumers to choose an independent delivery cadence.
     */
    std::uint32_t revision{};
    std::uint32_t measured_at_ms{};
    std::uint32_t correlation{};
    bool has_correlation{};

    /**
     * @brief Copies a bounded logical source name into the metadata.
     * @return false when the name does not fit including its terminator.
     */
    [[nodiscard]] bool set_source(std::string_view value);
    /**
     * @brief Copies an optional bounded device identity into the metadata.
     * @return false when the identity does not fit including its terminator.
     */
    [[nodiscard]] bool set_device(std::string_view value);
    /** @brief Returns the copied source name. */
    [[nodiscard]] std::string_view source() const;
    /** @brief Returns the copied device identity, which may be empty. */
    [[nodiscard]] std::string_view device() const;
    [[nodiscard]] bool set_response_topic_suffix(std::string_view value);

    [[nodiscard]] std::string_view response_topic_suffix() const {
        return {response_suffix_.data(), response_suffix_size_};
    }

    bool mqtt_fallback{};

    /** @brief Records the command correlation carried by this message. */
    void set_correlation(std::uint32_t value) {
        correlation = value;
        has_correlation = true;
    }

    /** @brief Removes any command correlation from this metadata. */
    void clear_correlation() {
        correlation = 0U;
        has_correlation = false;
    }

    /** @brief Checks the metadata fields required by the router. */
    [[nodiscard]] bool valid() const;

  private:
    std::array<char, kSourceCapacity> source_{};
    std::array<char, kDeviceCapacity> device_{};
    std::array<char, 32> response_suffix_{};
    std::uint8_t response_suffix_size_{};
    std::uint8_t source_size_{};
    std::uint8_t device_size_{};
};

/**
 * @brief Owns one immutable-after-publication bounded JSON payload.
 *
 * The router copies a complete payload before making it visible to consumers.
 * This prevents a transport from retaining a view into a producer's temporary
 * buffer.  The payload is expected to be one complete JSON value; parsing it
 * here would duplicate the semantic model and is deliberately out of scope.
 */
class CanonicalMessage final {
  public:
    static constexpr std::size_t kPayloadCapacity = 1024;

    /**
     * @brief Copies one complete payload and its metadata into this message.
     * @return false when metadata or payload bounds are invalid; the previous
     *         message remains intact on failure.
     */
    [[nodiscard]] bool assign(const MessageMetadata& metadata, std::string_view payload);

    /** @brief Returns the immutable routing metadata. */
    [[nodiscard]] const MessageMetadata& metadata() const { return metadata_; }

    /** @brief Returns the exact payload bytes supplied at publication. */
    [[nodiscard]] std::string_view payload() const { return {payload_.data(), payload_size_}; }

    /** @brief Reports whether a complete payload has been assigned. */
    [[nodiscard]] bool valid() const { return valid_; }

  private:
    MessageMetadata metadata_{};
    std::array<char, kPayloadCapacity> payload_{};
    std::size_t payload_size_{};
    bool valid_{};
};

/** @brief Reports the outcome of a bounded message publication. */
enum class MessagePublishResult : std::uint8_t {
    published,
    replaced_snapshot,
    /** A new snapshot source exceeded the fixed latest-value cache. */
    snapshot_slots_full,
    invalid,
    queue_full,
};

/** @brief Exposes bounded routing failures without exposing internal storage. */
struct MessageRouterStats final {
    std::uint32_t snapshot_replacements{};
    std::uint32_t snapshot_slot_rejections{};
    std::uint32_t ordered_queue_overflows{};
};

/**
 * @brief Routes immutable canonical messages without an unbounded queue.
 *
 * Snapshots are latest-value data and therefore replace the cache for their
 * logical source. A fixed number of sources prevents one producer from
 * replacing another producer's latest value.
 * Events and command responses use a small ordered queue observed by
 * independent consumers.  A full ordered queue rejects the new message and
 * increments an observable overflow counter; it never silently overwrites a
 * message that a slow consumer has not read.
 */
class MessageRouter final {
  public:
    /** @brief Initializes empty buffers at runtime so global payload storage stays in BSS. */
    MessageRouter();

    static constexpr std::size_t kMaximumConsumers = 4;
    static constexpr std::size_t kSnapshotCapacity = 2; // Power and sensors.
    static constexpr std::size_t kOrderedQueueCapacity = 4;
    static constexpr std::uint8_t kInvalidConsumer = 0xffU;

    /**
     * @brief Publishes one message according to its metadata kind.
     * @return The publication result; the payload is copied on success.
     */
    [[nodiscard]] MessagePublishResult publish(
        const MessageMetadata& metadata, std::string_view payload);
    /**
     * @brief Registers an independent ordered-message consumer.
     * @return A consumer handle, or kInvalidConsumer when all slots are used.
     */
    [[nodiscard]] std::uint8_t subscribe();
    /**
     * @brief Releases a consumer and makes its queue space reusable.
     * @return false for an invalid or already released handle.
     */
    [[nodiscard]] bool unsubscribe(std::uint8_t consumer);
    /**
     * @brief Returns the most recently published snapshot without consuming it.
     * @return A stable pointer until that source's next snapshot publication.
     */
    [[nodiscard]] const CanonicalMessage* latest_snapshot() const;
    /** @brief Returns the latest snapshot for one logical producer. */
    [[nodiscard]] const CanonicalMessage* latest_snapshot(std::string_view source) const;
    /**
     * @brief Returns the next response for one consumer.
     *
     * The returned pointer must be consumed before another router mutation.
     * Reading it advances only this consumer and never another consumer.
     * @return The next message, or nullptr when no message is pending.
     */
    [[nodiscard]] const CanonicalMessage* next_ordered(std::uint8_t consumer);
    /** @brief Returns messages skipped by this consumer's defensive recovery. */
    [[nodiscard]] std::uint32_t dropped(std::uint8_t consumer) const;
    /**
     * @brief Returns conservatively available ordered-message slots.
     *
     * Consumers are reclaimed before the count is returned.  A producer can
     * use this to admit a bounded command chain before it starts executing.
     */
    [[nodiscard]] std::size_t ordered_slots_available();

    /** @brief Returns bounded snapshot and ordered-queue counters. */
    [[nodiscard]] MessageRouterStats stats() const {
        return {snapshot_replacements_, snapshot_slot_rejections_, ordered_queue_overflows_};
    }

  private:
    struct Consumer final {
        bool active{};
        std::uint32_t next_ordered{};
        std::uint32_t dropped{};
    };

    /** @brief Returns the next non-zero wrapping publication sequence. */
    [[nodiscard]] static std::uint32_t advance(std::uint32_t value);
    /** @brief Tests ordering for values within the bounded wrap-safe window. */
    [[nodiscard]] static bool before(std::uint32_t left, std::uint32_t right);
    /** @brief Reclaims ordered slots already consumed by every active reader. */
    void reclaim_ordered();
    /** @brief Reports whether any ordered consumer is currently active. */
    [[nodiscard]] bool has_consumer() const;
    /** @brief Publishes one snapshot into its source's latest-value cache. */
    [[nodiscard]] MessagePublishResult publish_snapshot(
        const MessageMetadata& metadata, std::string_view payload);
    /** @brief Publishes one ordered response into the fixed queue. */
    [[nodiscard]] MessagePublishResult publish_ordered(
        const MessageMetadata& metadata, std::string_view payload);

    std::array<CanonicalMessage, kSnapshotCapacity> snapshots_{};
    std::array<bool, kSnapshotCapacity> has_snapshot_{};
    std::array<CanonicalMessage, kOrderedQueueCapacity> ordered_{};
    std::array<std::uint32_t, kOrderedQueueCapacity> ordered_sequences_{};
    std::array<Consumer, kMaximumConsumers> consumers_{};
    std::uint32_t message_sequence_{};
    std::uint32_t next_ordered_{};
    std::uint32_t oldest_ordered_{};
    std::size_t ordered_count_{};
    std::uint32_t snapshot_replacements_{};
    std::uint32_t snapshot_slot_rejections_{};
    std::uint32_t ordered_queue_overflows_{};
    std::uint8_t latest_snapshot_index_{kInvalidConsumer};
};

} // namespace newmota
