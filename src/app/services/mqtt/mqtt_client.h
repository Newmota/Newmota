/*
  mqtt_client.h - bounded MQTT session owner for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota_features.h"
#include "newmota/platform/tcp_stream.h"
#include "services/mqtt/mqtt_codec.h"
#include "services/mqtt/mqtt_service.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

/** @brief Owns the bounded broker fields used by one MQTT session. */
class MqttClientConfig final {
  public:
    static constexpr std::size_t kHostCapacity = 128;
    static constexpr std::size_t kClientIdCapacity = 64;
    static constexpr std::size_t kUsernameCapacity = 64;
    static constexpr std::size_t kPasswordCapacity = 64;
    static constexpr std::size_t kSubscriptionCapacity = 256;

    /** @brief Copies a nonempty broker host name or address. */
    [[nodiscard]] bool set_host(std::string_view value);
    /** @brief Copies a nonempty MQTT client identifier. */
    [[nodiscard]] bool set_client_id(std::string_view value);
    /** @brief Copies an optional MQTT username. */
    [[nodiscard]] bool set_username(std::string_view value);
    /** @brief Copies an optional MQTT password. */
    [[nodiscard]] bool set_password(std::string_view value);
    /** @brief Sets the broker TCP or TLS port; zero is invalid. */
    [[nodiscard]] bool set_port(std::uint16_t value);

    /** @brief Sets the MQTT keep-alive interval; zero disables keep-alive. */
    void set_keep_alive_seconds(std::uint16_t value) { keep_alive_seconds_ = value; }

    /** @brief Selects plain TCP or TLS for the native stream. */
    void set_secure(bool value) { secure_ = value; }

    /** @brief Sets the optional last-will topic and retain flag. */
    [[nodiscard]] bool set_will(std::string_view topic, bool retained);

    /** @brief Sets the delay before retrying a failed connection. */
    void set_retry_seconds(std::uint16_t value) { retry_seconds_ = value; }

    /** @brief Sets the connection and packet response timeout. */
    void set_timeout_seconds(std::uint16_t value) { timeout_seconds_ = value; }

    /** @brief Returns the configured last-will topic. */
    [[nodiscard]] std::string_view will_topic() const { return {will_topic_.data()}; }

    /** @brief Reports whether the last-will publication is retained. */
    [[nodiscard]] bool will_retained() const { return will_retained_; }

    /** @brief Returns the configured retry delay in milliseconds. */
    [[nodiscard]] std::uint32_t retry_milliseconds() const {
        return static_cast<std::uint32_t>(retry_seconds_) * 1000U;
    }

    /** @brief Returns the configured timeout in milliseconds. */
    [[nodiscard]] std::uint32_t timeout_milliseconds() const {
        return static_cast<std::uint32_t>(timeout_seconds_) * 1000U;
    }

    /** @brief Returns the configured broker host. */
    [[nodiscard]] std::string_view host() const { return {host_.data(), host_size_}; }

    /** @brief Returns the configured client identifier. */
    [[nodiscard]] std::string_view client_id() const {
        return {client_id_.data(), client_id_size_};
    }

    /** @brief Returns the configured username, if any. */
    [[nodiscard]] std::string_view username() const { return {username_.data(), username_size_}; }

    /** @brief Returns the configured password, if any. */
    [[nodiscard]] std::string_view password() const { return {password_.data(), password_size_}; }

    /** @brief Returns the broker port. */
    [[nodiscard]] std::uint16_t port() const { return port_; }

    /** @brief Returns the MQTT keep-alive interval. */
    [[nodiscard]] std::uint16_t keep_alive_seconds() const { return keep_alive_seconds_; }

    /** @brief Reports whether the native stream must use TLS. */
    [[nodiscard]] bool secure() const { return secure_; }

    /** @brief Reports whether all fields satisfy the session contract. */
    [[nodiscard]] bool valid() const;

  private:
    std::array<char, kHostCapacity> host_{};
    std::array<char, kClientIdCapacity> client_id_{};
    std::array<char, kUsernameCapacity> username_{};
    std::array<char, kPasswordCapacity> password_{};
    std::size_t host_size_{};
    std::size_t client_id_size_{};
    std::size_t username_size_{};
    std::size_t password_size_{};
    std::uint16_t port_{1883U};
    std::uint16_t keep_alive_seconds_{60U};
    bool secure_{};
    std::array<char, kSubscriptionCapacity> will_topic_{};
    bool will_retained_{true};
    std::uint16_t retry_seconds_{1U}, timeout_seconds_{15U};
};

/** @brief Exposes bounded protocol and reconnect counters for diagnostics. */
struct MqttClientStats final {
    std::uint32_t connect_attempts{};
    std::uint32_t connections{};
    std::uint32_t reconnects{};
    std::uint32_t publish_queued{};
    std::uint32_t publish_rejected{};
    std::uint32_t protocol_errors{};
    std::uint32_t transport_errors{};
    std::uint32_t refused{};
    std::uint32_t received_publishes{};
};

enum class MqttConnectionTestState : std::uint8_t { idle, running, passed, failed };

struct MqttConnectionTestStatus final {
    MqttConnectionTestState state{MqttConnectionTestState::idle};
    std::uint32_t started_at{}, finished_at{};
};

/**
 * @brief Owns the MQTT 3.1.1 session above a portable nonblocking stream.
 *
 * This class owns MQTT lifecycle, bounded packet flow, keep-alive, and retry
 * timing. The platform stream owns one DNS/TCP/TLS connection attempt and its
 * native objects. A publish callback receives views valid only during the
 * callback and must copy them before returning.
 */
class MqttClient final {
  public:
    using PublishHandler = void (*)(void* context, const MqttPublishPacket& packet);

    /** @brief Constructs an idle client around one native stream boundary. */
    MqttClient(platform::TcpStream stream, MqttClientConfig config = {});

    /** @brief Replaces the native stream and clears the active session. */
    void set_stream(platform::TcpStream stream);
    /** @brief Replaces valid broker fields and starts a fresh session. */
    [[nodiscard]] bool set_config(const MqttClientConfig& config);
    /**
     * @brief Replaces the one bounded QoS-0 subscription filter.
     *
     * An empty filter disables subscription for callers that only need
     * outbound publications. A changed filter starts a fresh session.
     */
    [[nodiscard]] bool set_subscription(std::string_view topic_filter);
    /** @brief Replaces the bounded QoS-0 subscription set. */
    [[nodiscard]] bool set_subscriptions(const std::string_view* filters, std::size_t count);
#if USE_BERRY
    // A bridged remote holds two filters, and discovery one more.
    static constexpr std::size_t kScriptFilterLimit = USE_MATTER_BRIDGE ? 16 : 8;
    /** @brief Atomically replaces desired script filters without resetting the session.
     */
    [[nodiscard]] bool set_script_subscriptions(const std::string_view* filters, std::size_t count);
    /** @brief Validates a bounded MQTT topic filter, including wildcard placement. */
    [[nodiscard]] static bool valid_filter(std::string_view filter);
#endif
    /** @brief Starts a bounded connection attempt for the diagnostic test. */
    [[nodiscard]] bool start_connection_test(std::uint32_t now);

    /** @brief Returns the current connection-test result and timestamps. */
    [[nodiscard]] MqttConnectionTestStatus connection_test_status() const { return test_status_; }

    /** @brief Reports whether a connection test is still in progress. */
    [[nodiscard]] bool connection_test_active() const {
        return test_status_.state == MqttConnectionTestState::running;
    }

    /** @brief Returns the latest session error text, if any. */
    [[nodiscard]] std::string_view last_error() const { return last_error_; }

    /** @brief Returns the latest connection-test error text, if any. */
    [[nodiscard]] std::string_view test_error() const { return test_error_; }

    /** @brief Reports whether an outbound publication remains queued. */
    [[nodiscard]] bool publication_pending() const { return writer_.pending(); }

    /** @brief Closes the current session and returns to the idle phase. */
    void disconnect();

    /** @brief Installs an optional non-owning inbound PUBLISH callback. */
    void set_publish_handler(PublishHandler handler, void* context) {
        publish_handler_ = handler;
        publish_context_ = context;
    }

    /** @brief Returns the policy callback boundary used by MqttService. */
    [[nodiscard]] MqttTransport transport();
    /** @brief Returns the current transport-visible lifecycle state. */
    [[nodiscard]] MqttTransportState state() const;
    /** @brief Advances one bounded nonblocking MQTT session step. */
    void poll(std::uint32_t now);

    /** @brief Returns a copy of bounded session counters. */
    [[nodiscard]] MqttClientStats stats() const { return stats_; }

  private:
    enum class SessionPhase : std::uint8_t {
        idle,
        connecting,
        awaiting_connack,
        awaiting_suback,
        connected,
        backoff,
    };

    enum class PendingPacket : std::uint8_t {
        none,
        connect,
        subscribe,
#if USE_BERRY
        unsubscribe,
#endif
        publish,
        ping,
    };

    static constexpr std::uint32_t kInitialReconnectDelayMs = 1000U;
    static constexpr std::uint32_t kMaximumReconnectDelayMs = 60000U;
    static constexpr std::uint32_t kHandshakeTimeoutMs = 15000U;
    static constexpr std::size_t kReadBufferCapacity = 256;

    /** @brief Returns whether the callbacks required by this client exist. */
    [[nodiscard]] bool stream_available() const;
    /** @brief Reports whether a wrapping deadline has elapsed. */
    [[nodiscard]] static bool deadline_reached(std::uint32_t now, std::uint32_t deadline);
    /** @brief Clears packet state and asks the native stream to close. */
    void clear_session();
    /** @brief Schedules a bounded reconnect after one failure. */
    void schedule_retry(std::uint32_t now, bool transport_failure);
    /** @brief Queues the CONNECT packet after TCP/TLS becomes ready. */
    [[nodiscard]] bool queue_connect();
    /** @brief Queues the one configured QoS-0 command subscription. */
    [[nodiscard]] bool queue_subscribe(std::uint32_t now);
    /** @brief Queues one keep-alive PINGREQ. */
    [[nodiscard]] bool queue_ping();
    /** @brief Processes one available inbound frame. */
    [[nodiscard]] bool read_one(std::uint32_t now);
    /** @brief Applies one complete inbound MQTT frame to session state. */
    [[nodiscard]] bool process_frame(std::uint32_t now);
    /** @brief Returns whether keep-alive should enqueue a ping or fail. */
    [[nodiscard]] bool poll_keep_alive(std::uint32_t now);
    /** @brief Serializes and queues one QoS-0 publication. */
    [[nodiscard]] bool publish(std::string_view topic, std::string_view payload, bool retained);
    /** @brief Allocates one nonzero MQTT packet identifier. */
    [[nodiscard]] std::uint16_t allocate_packet_id();

    /** @brief Adapts the client state to MqttTransport's callback signature. */
    static MqttTransportState transport_state(void* context);
    /** @brief Adapts publication handoff to MqttTransport's callback signature. */
    static bool transport_publish(
        void* context, std::string_view topic, std::string_view payload, bool retained);

    platform::TcpStream stream_{};
    MqttClientConfig config_{};
    MqttFrameReader reader_{};
    MqttPacketWriter writer_{};
    std::array<std::uint8_t, MqttFrameReader::kMaximumPacketSize> packet_buffer_{};
    std::array<std::uint8_t, kReadBufferCapacity> read_buffer_{};
    std::array<std::array<char, MqttClientConfig::kSubscriptionCapacity>, 6>
        subscription_filters_{};
    std::size_t subscription_count_{};
#if USE_BERRY
    using Filter = std::array<char, MqttClientConfig::kSubscriptionCapacity>;
    std::array<Filter, kScriptFilterLimit> script_filters_{};
    std::size_t script_filter_count_{};
    std::array<Filter, 6 + kScriptFilterLimit> acknowledged_filters_{};
    std::size_t acknowledged_count_{};
    Filter pending_filter_{};
    bool unsubscribe_pending_{};
    [[nodiscard]] bool desired_filter(std::string_view filter) const;
    [[nodiscard]] bool acknowledged_filter(std::string_view filter) const;
#else
    std::size_t subscription_index_{};
#endif
    MqttConnectionTestStatus test_status_{};
    std::uint32_t test_deadline_{};
    bool test_draining_disconnect_{};
    std::string_view last_error_{};
    std::string_view test_error_{};
    void finish_connection_test(bool success, std::uint32_t now);
    std::size_t read_size_{};
    std::size_t read_offset_{};
    SessionPhase phase_{SessionPhase::idle};
    PendingPacket pending_packet_{PendingPacket::none};
    PublishHandler publish_handler_{};
    void* publish_context_{};
    std::uint32_t next_attempt_{};
    std::uint32_t reconnect_delay_{kInitialReconnectDelayMs};
    std::uint32_t connect_write_deadline_{};
    std::uint32_t connack_deadline_{};
    std::uint32_t last_activity_{};
    std::uint32_t ping_sent_at_{};
    std::uint16_t next_packet_id_{1U};
    std::uint16_t subscribe_packet_id_{};
    bool awaiting_ping_{};
    bool subscription_pending_{};
    bool session_present_{};
    MqttClientStats stats_{};
};

} // namespace newmota
