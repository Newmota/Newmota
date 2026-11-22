/*
  mqtt_service.h - bounded MQTT publication policy for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file mqtt_service.h
 * @brief Bounded MQTT publication and command-routing policy.
 */

#pragma once

#include "core/message/message.h"
#include "services/telemetry/telemetry_settings.h"
#include "services/mqtt/mqtt_settings.h"
#ifndef NEWMOTA_IMAGE_SAFEBOOT
#include "services/power/button_event.h"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

/** @brief Connection state exposed by the native MQTT transport. */
enum class MqttTransportState : std::uint8_t {
    unavailable,
    disconnected,
    connecting,
    connected,
};

/**
 * @brief Synchronous handoff boundary between policy and MQTT I/O.
 *
 * The transport owns connection lifecycle, TCP/TLS, packet encoding, and
 * partial writes.  A successful publish callback must copy or fully consume
 * the supplied topic and payload before returning.  The service itself keeps
 * both in bounded storage so the callback never sees a router-owned view.
 */
struct MqttTransport final {
    using State = MqttTransportState (*)(void* context);
    using Publish = bool (*)(
        void* context, std::string_view topic, std::string_view payload, bool retained);

    void* context{};
    State state{};
    Publish publish{};
};

struct MqttTopicPreview final {
    std::array<char, 256> command{}, result{}, state{}, lwt{}, fallback{};
    std::array<std::array<char, 256>, 4> groups{};
};

/** @brief Exposes policy-level MQTT publication outcomes. */
struct MqttServiceStats final {
    std::uint32_t published{};
    std::uint32_t responses_published{};
    std::uint32_t wifi_events_published{};
    std::uint32_t failed{};
    std::uint32_t invalid{};
    std::uint32_t unavailable{};
    std::uint32_t cleanup_failures{};
    std::uint32_t result_rejected{};
};

/**
 * @brief Routes canonical power and sensor snapshots to Tasmota telemetry topics.
 *
 * This policy remains transport-neutral. It publishes the immutable semantic
 * payloads owned by MessageRouter. An optional bounded SENSOR document provider
 * can add publication-specific fields at the actual transport handoff. MqttClient and the native
 * stream provide protocol/session and plain-TCP mechanics. Persistent broker settings are applied
 * by the application; MqttCommandService owns inbound command normalization; this policy only
 * routes command responses and asynchronous Wi-Fi result records to the
 * Tasmota result topic.
 */
#if USE_TASMOTA_STATE
/** @brief The reason Info3 reports, as text. */
using RestartReasonText = const char*;
#else
/** @brief Whether the start was a power-on, which is all Safeboot's Info3 reports. */
using RestartReasonText = bool;
#endif

class MqttService final {
  public:
    static constexpr std::size_t kBaseTopicCapacity = 64;
    static constexpr std::size_t kTopicCapacity = 256;

    /** @brief Binds the policy to the canonical message cache. */
    explicit MqttService(MessageRouter& messages, MqttTransport transport = {});

    /**
     * @brief Selects the Tasmota base topic used by this first STATE route.
     * @return false for an empty, oversized, wildcard, or slash-ended topic.
     */
    [[nodiscard]] bool set_topic(std::string_view topic);
    /** @brief Builds bounded topic previews without changing service state. */
    [[nodiscard]] static bool preview_topics(const MqttSettings& settings,
        std::string_view resolved_topic, std::string_view resolved_client,
        MqttTopicPreview& preview, std::string_view hostname = {}, std::string_view id = {});
    /** @brief Applies broker topic settings and prepares the derived routes. */
    [[nodiscard]] bool configure(const MqttSettings& settings, std::string_view resolved_topic,
        std::string_view resolved_client, std::string_view hostname = {}, std::string_view id = {});

    /** @brief Returns the number of active command filters. */
    [[nodiscard]] std::size_t command_filter_count() const { return filter_count_; }

    /** @brief Returns one active command filter or an empty view when out of range. */
    [[nodiscard]] std::string_view command_filter(std::size_t index) const {
        return index < filter_count_ ? std::string_view{filters_[index].data()}
                                     : std::string_view{};
    }

    /** @brief Returns `tele/<topic>/STATE` of the active configuration. */
    [[nodiscard]] std::string_view state_topic() const {
        return {state_topic_.data(), state_topic_size_};
    }

    /** @brief Returns `tele/<topic>/SENSOR` of the active configuration. */
    [[nodiscard]] std::string_view sensor_topic() const {
        return {sensor_topic_.data(), sensor_topic_size_};
    }

    /** @brief Returns the command prefix `cmnd/<topic>/` of the active configuration. */
    [[nodiscard]] std::string_view command_prefix() const {
        return {command_prefix_.data(), command_prefix_size_};
    }

    /** @brief Reports whether publication is enabled and the transport is connected. */
    [[nodiscard]] bool connected() const {
        return enabled_ && transport_.state != nullptr &&
               transport_.state(transport_.context) == MqttTransportState::connected;
    }

    /** @brief Returns the configured last-will topic. */
    [[nodiscard]] std::string_view lwt_topic() const { return preview_.lwt.data(); }

    /** @brief Returns the client identity after template expansion. */
    [[nodiscard]] std::string_view client() const { return resolved_client_.data(); }

    /** @brief Queues a bounded publication for the next nonblocking poll. */
    [[nodiscard]] bool queue_publish(
        std::string_view topic, std::string_view payload, bool retained);
    /** @brief Queues one complete asynchronous device event on the configured SENSOR topic.
     * @param payload Bounded complete JSON, copied before return.
     * @return false when disconnected, full, or the payload exceeds transport limits.
     */
    [[nodiscard]] bool queue_sensor_event(std::string_view payload);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Attempts a nonblocking ButtonTopic key send; consumes a valid connected remote route
     * independently of writer congestion. SO61 retains local fallback. No socket I/O occurs.
     */
    [[nodiscard]] bool route_button_key(const MqttSettings& current, const ButtonEvent& event,
        bool on, bool force_local, bool no_hold_retain);
    /** @brief Queues an SO73 Action only for this connection/configuration, never offline. */
    [[nodiscard]] bool queue_button_action(std::string_view suffix, std::string_view payload);

    /** @brief Sets the logical Power count used by ButtonTopic POWER indexing. */
    void set_button_endpoint_count(std::uint8_t count) { button_endpoint_count_ = count; }

    /** @brief Starts paced best-effort retained ButtonTopic clearing through the old session. */
    void request_button_retain_clear(std::uint32_t now);
#if USE_SWITCHES
    /** @brief Attempts a nonblocking SwitchTopic key send, as route_button_key; Tasmota indexes
     * switch keys by switch number, never limited to the Power endpoints.
     */
    [[nodiscard]] bool route_switch_key(const MqttSettings& current, const SwitchEvent& event,
        bool on, bool force_local, bool no_hold_retain);

    /** @brief Sets the number of switches whose retained SwitchTopic keys may need clearing. */
    void set_switch_count(std::uint8_t count) { switch_count_ = count; }

    /** @brief Starts paced best-effort retained SwitchTopic clearing, as SwitchRetain OFF. */
    void request_switch_retain_clear(std::uint32_t now);
#endif
#endif
    /** @brief Copies an unsolicited stat event using the configured base, never last-command
     * routing.
     * @param suffix ASCII topic leaf, normalized to uppercase as in the source publisher.
     * @param payload Complete bounded JSON document.
     * @param result_or_stat Use RESULT unless SO4 selects the supplied leaf.
     * @return false for invalid suffix, disconnected transport or a full queue.
     */
    [[nodiscard]] bool queue_status_event(
        std::string_view suffix, std::string_view payload, bool result_or_stat = false);
    /** @brief Queues an event under the configured telemetry prefix and base topic.
     * @param suffix Nonempty ASCII letters, digits or underscores; no topic separators.
     * @param payload Complete bounded payload copied into the publication queue.
     * @param retained Publish with the retain flag, as Tasmota's tele/DeepSleep.
     * @return false for an invalid suffix, unavailable transport or exhausted queue/buffer.
     */
    [[nodiscard]] bool queue_telemetry_event(
        std::string_view suffix, std::string_view payload, bool retained = false);

    /** @brief Reports whether queued publications are still waiting for the transport. */
    [[nodiscard]] bool publications_pending() const { return publication_count_ != 0U; }

    /** @brief Requests SENSOR without requesting an unrelated STATE publication. */
    void request_sensor_telemetry() { sensor_requested_ = true; }

    /** @brief Prepares a settings change while the current session drains. */
    [[nodiscard]] bool prepare_reconfigure(const MqttSettings& next, std::uint32_t now);

    /** @brief Reports whether the most recent inbound command matched a group filter. */
    [[nodiscard]] bool command_group() const { return last_command_group_; }

    /** @brief Reports whether the most recent command used fallback routing. */
    [[nodiscard]] bool command_fallback() const { return last_command_fallback_; }

    /** @brief Sets the command name paired with the next published result. */
    void set_response_command(std::string_view command);
    /** @brief Queues a bounded command result with its response suffix. */
    [[nodiscard]] bool queue_command_result(std::string_view suffix, std::string_view payload);
    /** @brief Sets retained connection metadata used in the next state publication. */
    void set_connection_info(std::string_view version, std::string_view address,
        RestartReasonText restart_reason, std::uint32_t boot_count);
    /** @brief Consumes an echoed publication when it matches the active topic. */
    [[nodiscard]] bool consume_echo(
        std::string_view topic, std::string_view payload, bool retained);
    /** @brief Enables or disables MQTT publication policy. */
    void set_enabled(bool enabled);

    /** @brief Reports whether MQTT publication policy is enabled. */
    [[nodiscard]] bool enabled() const { return enabled_; }

    /** @brief Replaces the transport boundary without changing cached state. */
    void set_transport(MqttTransport transport) { transport_ = transport; }

    /** @brief Forces the next connected session to republish its latest state. */
    void reset_connection() {
        was_connected_ = false;
        sensor_has_published_ = false;
    }

    /**
     * @brief Sets the shared periodic telemetry interval.
     * @return false for a value outside the Tasmota-compatible range.
     */
    [[nodiscard]] bool set_tele_period_seconds(std::uint16_t seconds);

    /** @brief Returns the effective periodic telemetry interval. */
    [[nodiscard]] constexpr std::uint16_t tele_period_seconds() const {
        return tele_period_seconds_;
    }

    /** @brief Optional SENSOR publication preparation/acceptance contract.
     * prepare writes one complete document and returns an opaque stable token. It must not
     * reenter this service. accepted is called only after synchronous transport acceptance,
     * never after preparation failure or failed send. Neither callback implies broker ACK.
     */
    struct SensorDocumentProvider {
        bool (*prepare)(void* context, char* output, std::size_t capacity, std::size_t& size,
            std::uint32_t& token){};
        void (*accepted)(void* context, std::uint32_t token){};
        void* context{};
    };

    /** @brief Binds both SENSOR hooks atomically, or clears them when both are absent.
     * @param provider Caller-owned context and non-reentrant callbacks. prepare capacity includes
     * a trailing-NUL byte; returned size excludes it and must fit the canonical payload limit.
     * @return false if only one callback is supplied, leaving the existing binding unchanged.
     */
    [[nodiscard]] bool bind_sensor_document(SensorDocumentProvider provider) {
        if (bool(provider.prepare) != bool(provider.accepted))
            return false;
        sensor_document_ = provider;
        return true;
    }

#if USE_STATE_TEXT
    /** @brief Binds typed Power truth so aliases (including identical ON/OFF words) are not parsed.
     * @param read Returns a mask only for the requested canonical semantic revision.
     * @param context Non-owning caller context, valid for the service lifetime.
     */
    void bind_power_snapshot(bool (*read)(void*, std::uint32_t, std::uint32_t&), void* context) {
        power_snapshot_ = read;
        power_snapshot_context_ = context;
    }
#endif
    /** @brief Requests immediate power and sensor snapshot publications. */
    void request_telemetry() {
        publish_requested_ = true;
        sensor_requested_ = true;
    }

    /** @brief Polls one bounded publication attempt without blocking. */
    void poll(std::uint32_t now);

    /** @brief Returns the configured base topic. */
    [[nodiscard]] std::string_view topic() const { return {base_topic_.data(), base_topic_size_}; }

    /** @brief Returns the bounded command filter for the active base topic. */
    [[nodiscard]] std::string_view command_filter() const {
        return {command_filter_.data(), command_filter_size_};
    }

    /**
     * @brief Extracts a command name from `cmnd/<topic>/<command>`.
     * @return false when the topic is not one of this device's command topics.
     */
    [[nodiscard]] bool command_topic(std::string_view topic, std::string_view& command);

    /** @brief Returns observable policy failures and successful publications. */
    [[nodiscard]] MqttServiceStats stats() const { return stats_; }

  private:
    /** @brief Builds `tele/<topic>/STATE` into owned storage. */
    [[nodiscard]] bool build_state_topic();
    /** @brief Builds `tele/<topic>/SENSOR` into owned storage. */
    [[nodiscard]] bool build_sensor_topic();
    /** @brief Builds the result topic, command prefix, and subscription. */
    [[nodiscard]] bool build_command_topics();
    /** @brief Copies the latest power snapshot before transport handoff. */
    [[nodiscard]] bool copy_snapshot(const CanonicalMessage& snapshot);
    /** @brief Copies one ordered result message before transport handoff. */
    [[nodiscard]] bool copy_response(const CanonicalMessage& response);

    struct QueuedPublication final {
        std::array<char, kTopicCapacity> topic{};
        std::array<char, CanonicalMessage::kPayloadCapacity> payload{};
        std::size_t payload_size{};
        bool retained{};
#ifndef NEWMOTA_IMAGE_SAFEBOOT
        bool session_scoped{};
#endif
    };

    MqttSettings config_{};
    MqttTopicPreview preview_{};
    std::array<std::array<char, kTopicCapacity>, 6> filters_{};
    std::size_t filter_count_{};
    std::array<QueuedPublication, 2> publications_{};
    std::size_t publication_count_{};
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Origin of a SendKey: Tasmota's KEY_BUTTON or KEY_SWITCH. */
    enum class KeyKind : std::uint8_t { button, switch_input };

    /** @brief One paced retained key clear; cursor 0 is idle, else the next one-based key. */
    struct KeyClear {
        std::uint8_t cursor{};
        std::uint32_t started{};
    };

    std::array<char, 13> button_identity_{};
    std::uint8_t button_endpoint_count_{};
    std::uint8_t switch_count_{};
    std::array<KeyClear, 2> key_clears_{};
    /** @brief Resolves the active Button/SwitchTopic command leaf without retaining topic text.
     */
    [[nodiscard]] bool build_key_topic(KeyKind kind, std::uint8_t instance,
        std::array<char, kTopicCapacity>& topic, const MqttSettings* route = nullptr) const;
    /** @brief Publishes one key command; true consumes it, as Tasmota's SendKey. */
    [[nodiscard]] bool route_key(const MqttSettings& current, KeyKind kind, std::uint8_t instance,
        std::uint8_t key_instance, std::uint8_t key_state, bool on, bool force_local,
        bool no_hold_retain);
    /** @brief Returns the number of keys of one kind that may carry retained commands. */
    [[nodiscard]] std::uint8_t key_count(KeyKind kind) const;
    void request_key_clear(KeyKind kind, std::uint32_t now);
    /** @brief Selects the next retained key topic of a reconfiguration cleanup, skipping kinds
     * without retain.
     */
    [[nodiscard]] bool cleanup_key_topic(std::array<char, kTopicCapacity>& topic);
    void discard_session_publications();
    /** @brief Publishes one empty retained key; true when a clear is still running. */
    [[nodiscard]] bool publish_key_clear(std::uint32_t now);
#endif
    std::array<char, 32> response_command_{};
    std::array<char, 64> resolved_client_{};
    std::array<char, 32> hostname_{};
    std::uint8_t announce_stage_{};
    std::uint8_t power_cursor_{};
#if USE_STATE_TEXT
    bool (*power_snapshot_)(void*, std::uint32_t, std::uint32_t&){};
    void* power_snapshot_context_{};
#endif
    std::uint32_t power_mask_{};
    std::uint32_t pending_power_mask_{};
    std::uint32_t power_present_mask_{};
    bool power_json_sent_{};
    std::array<char, 32> version_{};
    std::array<char, 16> address_{};
    RestartReasonText restart_reason_{};
    std::uint32_t boot_count_{};
    std::array<std::uint32_t, 8> echo_hashes_{};
    std::size_t echo_cursor_{};
    std::uint32_t last_power_revision_{};
    std::uint8_t power_count_{};
    std::uint8_t cleanup_cursor_{};
    std::uint32_t cleanup_started_{};
    bool cleanup_active_{};
    bool cleanup_finished_{};
    bool parity_configured_{};
    bool enabled_{true};
    bool last_command_group_{};
    bool last_command_fallback_{};
    bool response_fallback_{};
    [[nodiscard]] bool send(std::string_view topic, std::string_view payload, bool retained);
    [[nodiscard]] bool publish_announcements();
    [[nodiscard]] bool publish_power(const CanonicalMessage& snapshot);
    /** @brief Publishes one canonical sensor snapshot when its policy is due. */
    [[nodiscard]] bool publish_sensor(const CanonicalMessage& snapshot, std::uint32_t now);
    [[nodiscard]] bool publish_cleanup();
    SensorDocumentProvider sensor_document_{};
    MessageRouter& messages_;
    MqttTransport transport_{};
    std::array<char, kBaseTopicCapacity> base_topic_{};
    std::size_t base_topic_size_{};
    std::array<char, kTopicCapacity> state_topic_{};
    std::size_t state_topic_size_{};
    std::array<char, kTopicCapacity> sensor_topic_{};
    std::size_t sensor_topic_size_{};
    std::array<char, kTopicCapacity> result_topic_{};
    std::size_t result_topic_size_{};
    std::array<char, kTopicCapacity> command_prefix_{};
    std::size_t command_prefix_size_{};
    std::array<char, kTopicCapacity> command_filter_{};
    std::size_t command_filter_size_{};
    std::array<char, CanonicalMessage::kPayloadCapacity + 1> payload_{};
    std::size_t payload_size_{};
    std::array<char, CanonicalMessage::kPayloadCapacity> response_payload_{};
    std::size_t response_payload_size_{};
    std::uint8_t response_consumer_{MessageRouter::kInvalidConsumer};
    std::uint32_t published_revision_{};
    std::uint32_t last_published_at_{};
    std::uint32_t sensor_last_published_at_{};
    std::uint16_t tele_period_seconds_{TelemetrySettings::kDefaultPeriodSeconds};
    MqttServiceStats stats_{};
    bool has_published_{};
    bool sensor_has_published_{};
    bool response_pending_{};
    bool response_is_wifi_event_{};
    bool was_connected_{};
    bool publish_requested_{};
    bool sensor_requested_{};
};

} // namespace newmota
