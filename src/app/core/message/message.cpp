/*
  message.cpp - canonical JSON messages and bounded routing for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "core/message/message.h"

#include <cstring>

namespace newmota {
namespace {

/** @brief Copies bounded metadata text and retains its exact byte length. */
template <std::size_t Capacity>
bool set_text(
    std::array<char, Capacity>& destination, std::uint8_t& size, const std::string_view value) {
    if (value.size() >= Capacity) {
        return false;
    }
    destination.fill('\0');
    if (!value.empty()) {
        std::memcpy(destination.data(), value.data(), value.size());
    }
    size = static_cast<std::uint8_t>(value.size());
    return true;
}

/** @brief Returns the number of values represented by a wrapping interval. */
std::uint32_t distance(const std::uint32_t first, const std::uint32_t last) {
    return last - first;
}

} // namespace

// Keeping this constructor out of line prevents constant initialization from placing
// every empty payload buffer in the firmware's initialized-data flash segment.
MessageRouter::MessageRouter() = default;

/** @brief Copies one bounded source name into the metadata record. */
bool MessageMetadata::set_source(const std::string_view value) {
    return set_text(source_, source_size_, value);
}

bool MessageMetadata::set_response_topic_suffix(const std::string_view value) {
    return set_text(response_suffix_, response_suffix_size_, value);
}

/** @brief Copies one optional bounded device identity into the metadata record. */
bool MessageMetadata::set_device(const std::string_view value) {
    return set_text(device_, device_size_, value);
}

/** @brief Returns the exact source bytes retained by this metadata record. */
std::string_view MessageMetadata::source() const {
    return {source_.data(), source_size_};
}

/** @brief Returns the exact optional device identity bytes. */
std::string_view MessageMetadata::device() const {
    return {device_.data(), device_size_};
}

/** @brief Validates the metadata required for bounded routing. */
bool MessageMetadata::valid() const {
    return schema_version != 0U && source_size_ != 0U;
}

/** @brief Copies one complete bounded JSON payload without partial replacement. */
bool CanonicalMessage::assign(const MessageMetadata& metadata, const std::string_view payload) {
    if (!metadata.valid() || payload.empty() || payload.size() > payload_.size()) {
        return false;
    }
    metadata_ = metadata;
    std::memcpy(payload_.data(), payload.data(), payload.size());
    payload_size_ = payload.size();
    valid_ = true;
    return true;
}

/** @brief Advances one non-zero wrapping routing sequence. */
std::uint32_t MessageRouter::advance(const std::uint32_t value) {
    const auto next = value + 1U;
    return next == 0U ? 1U : next;
}

/** @brief Compares two routing sequences within the bounded wrap-safe window. */
bool MessageRouter::before(const std::uint32_t left, const std::uint32_t right) {
    return static_cast<std::int32_t>(left - right) < 0;
}

/** @brief Reports whether any ordered consumer currently owns a queue slot. */
bool MessageRouter::has_consumer() const {
    for (const auto& consumer : consumers_) {
        if (consumer.active) {
            return true;
        }
    }
    return false;
}

/** @brief Reclaims ordered messages consumed by every active consumer. */
void MessageRouter::reclaim_ordered() {
    if (!has_consumer()) {
        ordered_count_ = 0U;
        oldest_ordered_ = next_ordered_;
        return;
    }

    bool found = false;
    std::uint32_t minimum = next_ordered_;
    for (const auto& consumer : consumers_) {
        if (!consumer.active) {
            continue;
        }
        if (!found || before(consumer.next_ordered, minimum)) {
            minimum = consumer.next_ordered;
            found = true;
        }
    }
    while (ordered_count_ != 0U && oldest_ordered_ != minimum) {
        oldest_ordered_ = advance(oldest_ordered_);
        --ordered_count_;
    }
}

/** @brief Routes one publication to its snapshot or ordered storage policy. */
MessagePublishResult MessageRouter::publish(
    const MessageMetadata& metadata, const std::string_view payload) {
    switch (metadata.kind) {
    case MessageKind::snapshot:
        return publish_snapshot(metadata, payload);
    case MessageKind::response:
        return publish_ordered(metadata, payload);
    }
    return MessagePublishResult::invalid;
}

/** @brief Publishes one latest-value snapshot, replacing its source's value. */
MessagePublishResult MessageRouter::publish_snapshot(
    const MessageMetadata& metadata, const std::string_view payload) {
    if (!metadata.valid() || payload.empty() ||
        payload.size() > CanonicalMessage::kPayloadCapacity) {
        return MessagePublishResult::invalid;
    }
    std::size_t slot = kSnapshotCapacity;
    bool replaced = false;
    for (std::size_t index = 0U; index < snapshots_.size(); ++index) {
        if (has_snapshot_[index] && snapshots_[index].metadata().source() == metadata.source()) {
            slot = index;
            replaced = true;
            break;
        }
        if (!has_snapshot_[index] && slot == kSnapshotCapacity)
            slot = index;
    }
    if (slot == kSnapshotCapacity) {
        ++snapshot_slot_rejections_;
        return MessagePublishResult::snapshot_slots_full;
    }
    MessageMetadata copy = metadata;
    copy.kind = MessageKind::snapshot;
    copy.sequence = advance(message_sequence_);
    if (!snapshots_[slot].assign(copy, payload)) {
        return MessagePublishResult::invalid;
    }
    has_snapshot_[slot] = true;
    latest_snapshot_index_ = static_cast<std::uint8_t>(slot);
    message_sequence_ = copy.sequence;
    if (replaced) {
        ++snapshot_replacements_;
        return MessagePublishResult::replaced_snapshot;
    }
    return MessagePublishResult::published;
}

/** @brief Publishes one response into the bounded ordered queue. */
MessagePublishResult MessageRouter::publish_ordered(
    const MessageMetadata& metadata, const std::string_view payload) {
    MessageMetadata copy = metadata;
    copy.sequence = advance(message_sequence_);
    if (!copy.valid() || payload.empty() || payload.size() > CanonicalMessage::kPayloadCapacity) {
        return MessagePublishResult::invalid;
    }

    reclaim_ordered();
    if (!has_consumer()) {
        message_sequence_ = copy.sequence;
        return MessagePublishResult::published;
    }
    if (ordered_count_ == kOrderedQueueCapacity) {
        ++ordered_queue_overflows_;
        return MessagePublishResult::queue_full;
    }

    const auto slot = next_ordered_ % kOrderedQueueCapacity;
    if (!ordered_[slot].assign(copy, payload)) {
        return MessagePublishResult::invalid;
    }
    ordered_sequences_[slot] = next_ordered_;
    next_ordered_ = advance(next_ordered_);
    if (ordered_count_ == 0U) {
        oldest_ordered_ = ordered_sequences_[slot];
    }
    ++ordered_count_;
    message_sequence_ = copy.sequence;
    return MessagePublishResult::published;
}

/** @brief Allocates one independent ordered consumer cursor. */
std::uint8_t MessageRouter::subscribe() {
    for (std::size_t index = 0; index < consumers_.size(); ++index) {
        auto& consumer = consumers_[index];
        if (!consumer.active) {
            consumer = {true, next_ordered_, 0U};
            return static_cast<std::uint8_t>(index);
        }
    }
    return kInvalidConsumer;
}

/** @brief Releases one ordered consumer cursor and its retained queue space. */
bool MessageRouter::unsubscribe(const std::uint8_t consumer) {
    if (consumer >= consumers_.size() || !consumers_[consumer].active) {
        return false;
    }
    consumers_[consumer] = {};
    reclaim_ordered();
    return true;
}

/** @brief Returns the current non-consuming latest snapshot. */
const CanonicalMessage* MessageRouter::latest_snapshot() const {
    return latest_snapshot_index_ < snapshots_.size() ? &snapshots_[latest_snapshot_index_]
                                                      : nullptr;
}

/** @brief Returns the most recent snapshot retained for one source. */
const CanonicalMessage* MessageRouter::latest_snapshot(const std::string_view source) const {
    for (std::size_t index = 0U; index < snapshots_.size(); ++index) {
        if (has_snapshot_[index] && snapshots_[index].metadata().source() == source) {
            return &snapshots_[index];
        }
    }
    return nullptr;
}

/** @brief Advances one consumer and returns its next ordered message. */
const CanonicalMessage* MessageRouter::next_ordered(const std::uint8_t consumer_index) {
    if (consumer_index >= consumers_.size() || !consumers_[consumer_index].active) {
        return nullptr;
    }
    auto& consumer = consumers_[consumer_index];
    if (before(consumer.next_ordered, oldest_ordered_)) {
        consumer.dropped += distance(consumer.next_ordered, oldest_ordered_);
        consumer.next_ordered = oldest_ordered_;
    }
    if (consumer.next_ordered == next_ordered_) {
        return nullptr;
    }

    const auto slot = consumer.next_ordered % kOrderedQueueCapacity;
    if (ordered_sequences_[slot] != consumer.next_ordered) {
        consumer.dropped += distance(consumer.next_ordered, oldest_ordered_);
        consumer.next_ordered = oldest_ordered_;
        if (consumer.next_ordered == next_ordered_) {
            return nullptr;
        }
    }
    const auto current_slot = consumer.next_ordered % kOrderedQueueCapacity;
    const auto* result = &ordered_[current_slot];
    consumer.next_ordered = advance(consumer.next_ordered);
    return result;
}

/** @brief Returns a consumer's defensive skipped-message count. */
std::uint32_t MessageRouter::dropped(const std::uint8_t consumer) const {
    return consumer < consumers_.size() ? consumers_[consumer].dropped : 0U;
}

/** @brief Reclaims consumed messages and reports bounded producer capacity. */
std::size_t MessageRouter::ordered_slots_available() {
    reclaim_ordered();
    return kOrderedQueueCapacity - ordered_count_;
}

} // namespace newmota
