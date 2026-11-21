/*
  mqtt_client.cpp - bounded MQTT session owner for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_client.h"

#include "newmota/log.h"

#include <cstring>

namespace newmota {
namespace {

/** @brief Copies one bounded text field while rejecting embedded NUL bytes. */
template <std::size_t N>
bool copy_text(std::array<char, N>& destination, std::size_t& destination_size,
    const std::string_view value, const bool allow_empty) {
    if ((!allow_empty && value.empty()) || value.size() >= N ||
        value.find('\0') != std::string_view::npos) {
        return false;
    }
    destination.fill('\0');
    if (!value.empty()) {
        std::memcpy(destination.data(), value.data(), value.size());
    }
    destination_size = value.size();
    return true;
}

/** @brief Converts a bounded elapsed interval to milliseconds. */
std::uint32_t keep_alive_milliseconds(const std::uint16_t seconds) {
    return static_cast<std::uint32_t>(seconds) * 1000U;
}

/** @brief Returns the larger of one second and half the MQTT keep-alive. */
std::uint32_t keep_alive_trigger(const std::uint16_t seconds) {
    const auto half = keep_alive_milliseconds(seconds) / 2U;
    return half < 1000U ? 1000U : half;
}

} // namespace

/** @brief Copies a nonempty broker host name or address. */
bool MqttClientConfig::set_host(const std::string_view value) {
    return copy_text(host_, host_size_, value, false);
}

/** @brief Copies a nonempty MQTT client identifier. */
bool MqttClientConfig::set_client_id(const std::string_view value) {
    return copy_text(client_id_, client_id_size_, value, false);
}

/** @brief Copies an optional MQTT username. */
bool MqttClientConfig::set_username(const std::string_view value) {
    return copy_text(username_, username_size_, value, true);
}

/** @brief Copies an optional MQTT password. */
bool MqttClientConfig::set_password(const std::string_view value) {
    return copy_text(password_, password_size_, value, true);
}

/** @brief Sets the broker TCP or TLS port; zero is invalid. */
bool MqttClientConfig::set_port(const std::uint16_t value) {
    if (value == 0U) {
        return false;
    }
    port_ = value;
    return true;
}

bool MqttClientConfig::set_will(const std::string_view topic, const bool retained) {
    std::size_t length = 0U;
    if (topic.find_first_of("+#") != std::string_view::npos)
        return false;
    if (!copy_text(will_topic_, length, topic, true))
        return false;
    will_retained_ = retained;
    return true;
}

/** @brief Reports whether all fields satisfy the session contract. */
bool MqttClientConfig::valid() const {
    return !host().empty() && !client_id().empty() && port_ != 0U &&
           (password().empty() || !username().empty());
}

/** @brief Constructs an idle client around one native stream boundary. */
MqttClient::MqttClient(const platform::TcpStream stream, const MqttClientConfig config)
    : stream_(stream), config_(config) {}

/** @brief Replaces the native stream and clears the active session. */
void MqttClient::set_stream(const platform::TcpStream stream) {
    clear_session();
    stream_ = stream;
    reconnect_delay_ = config_.retry_milliseconds();
}

/** @brief Replaces valid broker fields and starts a fresh session. */
bool MqttClient::set_config(const MqttClientConfig& config) {
    if (!config.valid()) {
        return false;
    }
    if (connection_test_active()) {
        last_error_ = "configuration changed during test";
        finish_connection_test(false, test_status_.started_at);
    }
    clear_session();
    config_ = config;
    reconnect_delay_ = config_.retry_milliseconds();
    return true;
}

/** @brief Replaces the bounded command subscription filter. */
bool MqttClient::set_subscription(const std::string_view topic_filter) {
    return set_subscriptions(&topic_filter, topic_filter.empty() ? 0U : 1U);
}

bool MqttClient::set_subscriptions(const std::string_view* filters, const std::size_t count) {
    if (count > subscription_filters_.size() || (count != 0U && filters == nullptr))
        return false;
    for (std::size_t i = 0U; i < count; ++i)
        if (filters[i].empty() || filters[i].size() >= MqttClientConfig::kSubscriptionCapacity ||
            filters[i].find('\0') != std::string_view::npos)
            return false;
    clear_session();
    subscription_filters_ = {};
    subscription_count_ = count;
    for (std::size_t i = 0U; i < count; ++i)
        std::memcpy(subscription_filters_[i].data(), filters[i].data(), filters[i].size());
    return true;
}
#if USE_BERRY
bool MqttClient::valid_filter(const std::string_view filter) {
    if (filter.empty() || filter.size() >= MqttClientConfig::kSubscriptionCapacity ||
        filter.find('\0') != std::string_view::npos)
        return false;
    for (std::size_t i = 0; i < filter.size(); ++i) {
        if (filter[i] == '+' &&
            ((i && filter[i - 1] != '/') || (i + 1 < filter.size() && filter[i + 1] != '/')))
            return false;
        if (filter[i] == '#' && ((i && filter[i - 1] != '/') || i + 1 != filter.size()))
            return false;
    }
    return true;
}

bool MqttClient::set_script_subscriptions(
    const std::string_view* filters, const std::size_t count) {
    if (count > script_filters_.size() || (count && !filters))
        return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!valid_filter(filters[i]))
            return false;
    script_filters_ = {};
    script_filter_count_ = 0;
    for (std::size_t i = 0; i < count; ++i) {
        bool duplicate = false;
        for (std::size_t j = 0; j < script_filter_count_; ++j)
            if (filters[i] == script_filters_[j].data())
                duplicate = true;
        if (!duplicate)
            std::memcpy(script_filters_[script_filter_count_++].data(), filters[i].data(),
                filters[i].size());
    }
    return true;
}

bool MqttClient::desired_filter(const std::string_view filter) const {
    for (std::size_t i = 0; i < subscription_count_; ++i)
        if (filter == subscription_filters_[i].data())
            return true;
    for (std::size_t i = 0; i < script_filter_count_; ++i)
        if (filter == script_filters_[i].data())
            return true;
    return false;
}

bool MqttClient::acknowledged_filter(const std::string_view filter) const {
    for (std::size_t i = 0; i < acknowledged_count_; ++i)
        if (filter == acknowledged_filters_[i].data())
            return true;
    return false;
}
#endif
bool MqttClient::start_connection_test(const std::uint32_t now) {
    if (!stream_available() || connection_test_active() || config_.secure())
        return false;
    clear_session();
    last_error_ = {};
    test_error_ = {};
    test_status_ = {MqttConnectionTestState::running, now, 0U};
    test_deadline_ = now + 30000U;
    return true;
}

void MqttClient::finish_connection_test(const bool success, const std::uint32_t now) {
    if (success && !test_draining_disconnect_) {
        const std::uint8_t packet[]{0xe0U, 0x00U};
        if (!writer_.queue(packet, sizeof packet)) {
            last_error_ = "test disconnect queue failed";
            finish_connection_test(false, now);
            return;
        }
        test_draining_disconnect_ = true;
        return;
    }
    test_draining_disconnect_ = false;
    test_status_.state =
        success ? MqttConnectionTestState::passed : MqttConnectionTestState::failed;
    test_status_.finished_at = now;
    test_error_ = success ? std::string_view{} : last_error_;
    clear_session();
    next_attempt_ = now + 1000U;
    phase_ = SessionPhase::backoff;
}

/** @brief Closes the current session and returns to the idle phase. */
void MqttClient::disconnect() {
    if (connection_test_active()) {
        last_error_ = "connection test interrupted";
        finish_connection_test(false, test_status_.started_at);
    }
    clear_session();
    reconnect_delay_ = config_.retry_milliseconds();
}

/** @brief Returns the policy callback boundary used by MqttService. */
MqttTransport MqttClient::transport() {
    return {this, &MqttClient::transport_state, &MqttClient::transport_publish};
}

/** @brief Returns the current transport-visible lifecycle state. */
MqttTransportState MqttClient::state() const {
    if (!stream_available()) {
        return MqttTransportState::unavailable;
    }
    if (phase_ == SessionPhase::connected &&
        stream_.state(stream_.context) == platform::TcpStreamState::connected) {
        return MqttTransportState::connected;
    }
    if (phase_ == SessionPhase::connecting || phase_ == SessionPhase::awaiting_connack ||
        phase_ == SessionPhase::awaiting_suback ||
        (phase_ == SessionPhase::connected &&
            stream_.state(stream_.context) == platform::TcpStreamState::connecting)) {
        return MqttTransportState::connecting;
    }
    return MqttTransportState::disconnected;
}

/** @brief Returns whether the callbacks required by this client exist. */
bool MqttClient::stream_available() const {
    return config_.valid() && stream_.state != nullptr && stream_.open != nullptr &&
           stream_.read != nullptr && stream_.write != nullptr;
}

/** @brief Reports whether a wrapping deadline has elapsed. */
bool MqttClient::deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::uint32_t>(now - deadline) < 0x80000000U;
}

/** @brief Clears packet state and asks the native stream to close. */
void MqttClient::clear_session() {
    if (stream_.close != nullptr) {
        stream_.close(stream_.context);
    }
    writer_.clear();
    reader_.reset();
    packet_buffer_.fill(0U);
    read_buffer_.fill(0U);
    read_size_ = 0U;
    read_offset_ = 0U;
    phase_ = SessionPhase::idle;
    pending_packet_ = PendingPacket::none;
    next_attempt_ = 0U;
    connect_write_deadline_ = 0U;
    connack_deadline_ = 0U;
    last_activity_ = 0U;
    ping_sent_at_ = 0U;
    awaiting_ping_ = false;
    subscribe_packet_id_ = 0U;
    subscription_pending_ = false;
#if USE_BERRY
    acknowledged_filters_ = {};
    acknowledged_count_ = 0U;
    pending_filter_ = {};
    unsubscribe_pending_ = false;
#else
    subscription_index_ = 0U;
#endif
    session_present_ = false;
}

/** @brief Schedules a bounded reconnect after one failure. */
void MqttClient::schedule_retry(const std::uint32_t now, const bool transport_failure) {
    if (transport_failure) {
        ++stats_.transport_errors;
    }
    ++stats_.reconnects;
    if (last_error_.empty())
        last_error_ = transport_failure ? "transport failure or timeout" : "invalid broker packet";
    if (connection_test_active()) {
        finish_connection_test(false, now);
        return;
    }
    clear_session();
    phase_ = SessionPhase::backoff;
    next_attempt_ = now + reconnect_delay_;
    if (reconnect_delay_ < kMaximumReconnectDelayMs / 2U) {
        reconnect_delay_ *= 2U;
    } else {
        reconnect_delay_ = kMaximumReconnectDelayMs;
    }
}

/** @brief Queues the CONNECT packet after TCP/TLS becomes ready. */
bool MqttClient::queue_connect() {
    const MqttConnectParameters parameters{config_.client_id(), config_.username(),
        config_.password(), config_.keep_alive_seconds(), true, config_.will_topic(), "Offline",
        config_.will_retained()};
    std::size_t packet_size = 0U;
    if (MqttCodec::serialize_connect(parameters, packet_buffer_.data(), packet_buffer_.size(),
            packet_size) != MqttCodecResult::success ||
        !writer_.queue(packet_buffer_.data(), packet_size)) {
        return false;
    }
    pending_packet_ = PendingPacket::connect;
    return true;
}

/** @brief Allocates one nonzero packet identifier for a QoS handshake. */
std::uint16_t MqttClient::allocate_packet_id() {
    const auto result = next_packet_id_;
    ++next_packet_id_;
    if (next_packet_id_ == 0U)
        next_packet_id_ = 1U;
    return result == 0U ? 1U : result;
}

/** @brief Queues the configured command subscription after CONNACK. */
#if USE_BERRY
bool MqttClient::queue_subscribe(const std::uint32_t now) {
    if (writer_.pending() || subscription_pending_)
        return true;
    std::string_view next{};
    bool removing = false;
    for (std::size_t i = 0; i < acknowledged_count_; ++i) {
        if (!desired_filter(acknowledged_filters_[i].data())) {
            next = acknowledged_filters_[i].data();
            removing = true;
            break;
        }
    }
    if (next.empty()) {
        for (std::size_t i = 0; i < subscription_count_; ++i) {
            if (!acknowledged_filter(subscription_filters_[i].data())) {
                next = subscription_filters_[i].data();
                break;
            }
        }
    }
    if (next.empty()) {
        for (std::size_t i = 0; i < script_filter_count_; ++i) {
            if (!acknowledged_filter(script_filters_[i].data())) {
                next = script_filters_[i].data();
                break;
            }
        }
    }
    if (next.empty()) {
        if (phase_ == SessionPhase::awaiting_suback)
            phase_ = SessionPhase::connected;
        return true;
    }
    const auto packet_id = allocate_packet_id();
    std::size_t packet_size = 0U;
    const auto result = removing ? MqttCodec::serialize_unsubscribe(next, packet_id,
                                       packet_buffer_.data(), packet_buffer_.size(), packet_size)
                                 : MqttCodec::serialize_subscribe(next, packet_id,
                                       packet_buffer_.data(), packet_buffer_.size(), packet_size);
    if (result != MqttCodecResult::success || !writer_.queue(packet_buffer_.data(), packet_size))
        return false;
    pending_filter_ = {};
    std::memcpy(pending_filter_.data(), next.data(), next.size());
    subscribe_packet_id_ = packet_id;
    subscription_pending_ = true;
    unsubscribe_pending_ = removing;
    pending_packet_ = removing ? PendingPacket::unsubscribe : PendingPacket::subscribe;
    // The acknowledgement deadline starts after this bounded control write.
    connack_deadline_ = now + config_.timeout_milliseconds();
    return true;
}

#else
bool MqttClient::queue_subscribe(const std::uint32_t now) {
    (void)now;
    if (subscription_index_ >= subscription_count_)
        return true;
    const auto packet_id = allocate_packet_id();
    std::size_t packet_size = 0U;
    if (MqttCodec::serialize_subscribe(
            std::string_view{subscription_filters_[subscription_index_].data()}, packet_id,
            packet_buffer_.data(), packet_buffer_.size(),
            packet_size) != MqttCodecResult::success ||
        !writer_.queue(packet_buffer_.data(), packet_size)) {
        return false;
    }
    subscribe_packet_id_ = packet_id;
    subscription_pending_ = true;
    pending_packet_ = PendingPacket::subscribe;
    return true;
}

#endif

/** @brief Queues one keep-alive PINGREQ. */
bool MqttClient::queue_ping() {
    std::size_t packet_size = 0U;
    if (MqttCodec::serialize_pingreq(packet_buffer_.data(), packet_buffer_.size(), packet_size) !=
            MqttCodecResult::success ||
        !writer_.queue(packet_buffer_.data(), packet_size)) {
        return false;
    }
    pending_packet_ = PendingPacket::ping;
    return true;
}

/** @brief Processes one available inbound frame. */
bool MqttClient::read_one(const std::uint32_t now) {
    if (read_offset_ == read_size_) {
        read_size_ = 0U;
        read_offset_ = 0U;
        const auto result = stream_.read(stream_.context, read_buffer_.data(), read_buffer_.size());
        if (result.size > read_buffer_.size() ||
            result.state == platform::TcpStreamIoState::closed ||
            result.state == platform::TcpStreamIoState::failed ||
            result.state == platform::TcpStreamIoState::unavailable) {
            schedule_retry(now, true);
            return false;
        }
        if (result.size == 0U) {
            return true;
        }
        read_size_ = result.size;
    }

    std::size_t consumed = 0U;
    const auto result =
        reader_.feed(read_buffer_.data() + read_offset_, read_size_ - read_offset_, consumed);
    read_offset_ += consumed;
    if (result == MqttFrameResult::invalid || result == MqttFrameResult::too_large) {
        ++stats_.protocol_errors;
        schedule_retry(now, false);
        return false;
    }
#if USE_BERRY // Only script builds skip large publications; see MqttFrameReader::feed.
    if (result == MqttFrameResult::skipped) {
        last_activity_ = now;
        AddLog(LogLevel::info, "MQT: skipped a publication above the packet limit");
        return true;
    }
#endif
    if (result != MqttFrameResult::complete) {
        return true;
    }

    last_activity_ = now;
    const bool valid = process_frame(now);
    reader_.reset();
    if (!valid) {
        ++stats_.protocol_errors;
        schedule_retry(now, false);
        return false;
    }
    return true;
}

/** @brief Applies one complete inbound MQTT frame to session state. */
bool MqttClient::process_frame(const std::uint32_t now) {
    if (phase_ == SessionPhase::awaiting_connack) {
        bool session_present = false;
        std::uint8_t return_code = 0U;
        const auto result =
            MqttCodec::decode_connack(reader_.data(), reader_.size(), session_present, return_code);
        if (result == MqttCodecResult::refused) {
            ++stats_.refused;
            last_error_ = return_code == 4U   ? "bad username or password"
                          : return_code == 5U ? "not authorized"
                                              : "broker refused connection";
        }
        if (result != MqttCodecResult::success) {
            return false;
        }
        session_present_ = session_present;
        phase_ = SessionPhase::connected;
        reconnect_delay_ = config_.retry_milliseconds();
        connect_write_deadline_ = 0U;
        connack_deadline_ = 0U;
        awaiting_ping_ = false;
        last_activity_ = now;
        ++stats_.connections;
        last_error_ = {};
        if (subscription_count_ != 0U
#if USE_BERRY
            || script_filter_count_ != 0U
#endif
        ) {
            phase_ = SessionPhase::awaiting_suback;
            connack_deadline_ = now + config_.timeout_milliseconds();
            if (!queue_subscribe(now))
                return false;
        }
        return true;
    }

    if (reader_.size() == 0U) {
        return false;
    }
    const auto packet_type = reader_.data()[0];
#if USE_BERRY
    if ((packet_type & 0xf0U) == 0x90U || packet_type == 0xb0U) {
        if (!subscription_pending_ || (packet_type == 0xb0U) != unsubscribe_pending_)
            return false;
        std::uint16_t packet_id = 0U;
        const auto result =
            unsubscribe_pending_
                ? MqttCodec::decode_unsuback(reader_.data(), reader_.size(), packet_id)
                : MqttCodec::decode_suback(reader_.data(), reader_.size(), packet_id);
        if (result == MqttCodecResult::refused)
            ++stats_.refused;
        if (result != MqttCodecResult::success || packet_id != subscribe_packet_id_)
            return false;
        if (unsubscribe_pending_) {
            for (std::size_t i = 0; i < acknowledged_count_; ++i) {
                if (std::string_view{acknowledged_filters_[i].data()} == pending_filter_.data()) {
                    for (std::size_t j = i + 1; j < acknowledged_count_; ++j)
                        acknowledged_filters_[j - 1] = acknowledged_filters_[j];
                    --acknowledged_count_;
                    break;
                }
            }
        } else {
            if (acknowledged_count_ == acknowledged_filters_.size())
                return false;
            acknowledged_filters_[acknowledged_count_++] = pending_filter_;
        }
        subscription_pending_ = false;
        unsubscribe_pending_ = false;
        connack_deadline_ = 0U;
        return true;
    }
#else
    if (phase_ == SessionPhase::awaiting_suback && (packet_type & 0xf0U) == 0x90U) {
        if (!subscription_pending_)
            return false;
        std::uint16_t packet_id = 0U;
        const auto result = MqttCodec::decode_suback(reader_.data(), reader_.size(), packet_id);
        if (result == MqttCodecResult::refused)
            ++stats_.refused;
        if (result != MqttCodecResult::success || packet_id != subscribe_packet_id_) {
            return false;
        }
        subscription_pending_ = false;
        ++subscription_index_;
        if (subscription_index_ < subscription_count_) {
            connack_deadline_ = now + config_.timeout_milliseconds();
            return queue_subscribe(now);
        }
        phase_ = SessionPhase::connected;
        connack_deadline_ = 0U;
        return true;
    }
#endif
    if (phase_ != SessionPhase::connected && phase_ != SessionPhase::awaiting_suback) {
        return false;
    }
    if (packet_type == 0xD0U) {
        if (MqttCodec::decode_pingresp(reader_.data(), reader_.size()) !=
            MqttCodecResult::success) {
            return false;
        }
        awaiting_ping_ = false;
        last_activity_ = now;
        return true;
    }
    if ((packet_type & 0xF0U) == 0x30U) {
        MqttPublishPacket packet{};
        const auto result = MqttCodec::decode_publish(reader_.data(), reader_.size(), packet);
        if (result != MqttCodecResult::success || packet.qos != 0U) {
            return false;
        }
        ++stats_.received_publishes;
        if (publish_handler_ != nullptr && !connection_test_active()) {
            publish_handler_(publish_context_, packet);
        }
        return true;
    }
    return false;
}

/** @brief Returns whether keep-alive should enqueue a ping or fail. */
bool MqttClient::poll_keep_alive(const std::uint32_t now) {
    if (phase_ != SessionPhase::connected || subscription_pending_ ||
        config_.keep_alive_seconds() == 0U) {
        return true;
    }
    if (awaiting_ping_) {
        return now - ping_sent_at_ < keep_alive_milliseconds(config_.keep_alive_seconds());
    }
    if (writer_.pending() ||
        now - last_activity_ < keep_alive_trigger(config_.keep_alive_seconds())) {
        return true;
    }
    return queue_ping();
}

/** @brief Serializes and queues one QoS-0 publication. */
bool MqttClient::publish(
    const std::string_view topic, const std::string_view payload, const bool retained) {
    if (connection_test_active() || state() != MqttTransportState::connected || writer_.pending() ||
        awaiting_ping_) {
        ++stats_.publish_rejected;
        return false;
    }
    std::size_t packet_size = 0U;
    if (MqttCodec::serialize_publish(topic, payload, retained, packet_buffer_.data(),
            packet_buffer_.size(), packet_size) != MqttCodecResult::success ||
        !writer_.queue(packet_buffer_.data(), packet_size)) {
        ++stats_.publish_rejected;
        return false;
    }
    pending_packet_ = PendingPacket::publish;
    ++stats_.publish_queued;
    return true;
}

/** @brief Adapts the client state to MqttTransport's callback signature. */
MqttTransportState MqttClient::transport_state(void* const context) {
    if (context == nullptr) {
        return MqttTransportState::unavailable;
    }
    return static_cast<MqttClient*>(context)->state();
}

/** @brief Adapts publication handoff to MqttTransport's callback signature. */
bool MqttClient::transport_publish(void* const context, const std::string_view topic,
    const std::string_view payload, const bool retained) {
    if (context == nullptr) {
        return false;
    }
    return static_cast<MqttClient*>(context)->publish(topic, payload, retained);
}

/** @brief Advances one bounded nonblocking MQTT session step. */
void MqttClient::poll(const std::uint32_t now) {
    if (connection_test_active() && deadline_reached(now, test_deadline_)) {
        last_error_ = "connection test timed out";
        finish_connection_test(false, now);
        return;
    }
    if (!stream_available()) {
        return;
    }
    if (test_draining_disconnect_) {
        const auto progress = writer_.poll(stream_);
        if (progress == MqttWriteProgress::sent)
            finish_connection_test(true, now);
        else if (progress != MqttWriteProgress::pending) {
            last_error_ = "test disconnect transport failed";
            finish_connection_test(false, now);
        }
        return;
    }
    if (phase_ == SessionPhase::backoff && !deadline_reached(now, next_attempt_)) {
        return;
    }
    if (phase_ == SessionPhase::idle || phase_ == SessionPhase::backoff) {
        ++stats_.connect_attempts;
        if (stream_.open(stream_.context, config_.host(), config_.port(), config_.secure()) !=
            platform::TcpStreamOpenResult::accepted) {
            last_error_ = config_.secure() ? "TLS unavailable" : "transport open failed";
            schedule_retry(now, true);
            return;
        }
        phase_ = SessionPhase::connecting;
    }

    const auto native_state = stream_.state(stream_.context);
    if (native_state != platform::TcpStreamState::connected) {
        if (phase_ == SessionPhase::connecting &&
            (native_state == platform::TcpStreamState::idle ||
                native_state == platform::TcpStreamState::connecting)) {
            return;
        }
        schedule_retry(now, true);
        return;
    }
    if (phase_ == SessionPhase::connecting) {
        if (!queue_connect()) {
            ++stats_.protocol_errors;
            schedule_retry(now, false);
            return;
        }
        phase_ = SessionPhase::awaiting_connack;
        connect_write_deadline_ = now + config_.timeout_milliseconds();
        connack_deadline_ = 0U;
    }

    const auto write_result = writer_.poll(stream_);
    if (write_result == MqttWriteProgress::failed ||
        write_result == MqttWriteProgress::unavailable ||
        write_result == MqttWriteProgress::not_connected) {
        schedule_retry(now, true);
        return;
    }
    if (write_result == MqttWriteProgress::pending || write_result == MqttWriteProgress::sent) {
        last_activity_ = now;
    }
    if (write_result == MqttWriteProgress::sent) {
        if (pending_packet_ == PendingPacket::ping) {
            awaiting_ping_ = true;
            ping_sent_at_ = now;
        }
        if (pending_packet_ == PendingPacket::subscribe
#if USE_BERRY
            || pending_packet_ == PendingPacket::unsubscribe
#endif
        )
            connack_deadline_ = now + config_.timeout_milliseconds();
        pending_packet_ = PendingPacket::none;
    }

    // A broker cannot legally send CONNACK until the complete CONNECT has
    // reached it. Keep both halves of this handshake bounded when the native
    // stream or broker stops making progress.
    if (phase_ == SessionPhase::awaiting_connack || phase_ == SessionPhase::awaiting_suback ||
        subscription_pending_) {
        if (writer_.pending()) {
            const auto write_deadline = phase_ == SessionPhase::awaiting_connack
                                            ? connect_write_deadline_
                                            : connack_deadline_;
            if (deadline_reached(now, write_deadline)) {
                schedule_retry(now, true);
            }
            return;
        }
        if (connack_deadline_ == 0U) {
            connack_deadline_ = now + config_.timeout_milliseconds();
        }
        if (deadline_reached(now, connack_deadline_)) {
            schedule_retry(now, true);
            return;
        }
    }

    if (!read_one(now) || phase_ == SessionPhase::backoff) {
        return;
    }
#if USE_BERRY
    if ((phase_ == SessionPhase::connected || phase_ == SessionPhase::awaiting_suback) &&
        !queue_subscribe(now)) {
        schedule_retry(now, false);
        return;
    }
#endif
    if (connection_test_active() && state() == MqttTransportState::connected) {
        finish_connection_test(true, now);
        return;
    }
    if (!poll_keep_alive(now)) {
        schedule_retry(now, true);
    }
}

} // namespace newmota
