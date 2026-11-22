/*
  command.h - command processing interfaces for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota_features.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {

class Settings;
class RestartController;
class TimeService;
class PowerService;
class BootCounter;
class UrlUpdateService;
class WgetService;
class PingService;
#if USE_WIFI
class NetworkService;
#endif
#if USE_BERRY
class BerryService;
#endif

enum class CommandSource : std::uint8_t {
    serial,
    web_console,
    http,
    rule,
    timer,
    berry,
    internal,
#if USE_MQTT
    mqtt,
#endif
#if USE_BACKLOG
    backlog,
#endif
};

enum class CommandStatus : std::uint8_t {
    handled,
    invalid,
    failed,
    unknown,
};

enum class CommandState : std::uint8_t {
    invalid,
    off,
    on,
    toggle,
};

struct CommandRequest final {
    CommandSource source;
    std::string_view name;
    std::string_view payload;
    std::uint16_t index;
    bool has_index;
    bool has_payload;

    /**
     * @brief Determines whether this request asks for the current value.
     * @return true for a missing payload or a question-mark payload.
     */
    [[nodiscard]] bool is_query() const;
};

class CommandOutput final {
  public:
    using Writer = bool (*)(void* context, std::string_view text);

    /**
     * @brief Creates an output stream backed by a caller-provided writer.
     */
    constexpr CommandOutput(Writer writer, void* context) : writer_(writer), context_(context) {}

    /**
     * @brief Writes unmodified text to the response stream.
     * @return false after the first writer failure.
     */
    [[nodiscard]] bool write(std::string_view text);
    /**
     * @brief Formats and writes one bounded response fragment.
     * @return false on formatting overflow or writer failure.
     */
    [[nodiscard]] bool format(const char* format, ...);
    /**
     * @brief Writes @p text as an escaped JSON string.
     * @return false when output fails.
     */
    [[nodiscard]] bool json_string(std::string_view text);

    /**
     * @brief Reports whether any output operation has failed.
     */
    [[nodiscard]] bool failed() const { return failed_; }

    /**
     * @brief Returns the number of response bytes written successfully.
     */
    [[nodiscard]] std::size_t size() const { return size_; }

  private:
    Writer writer_;
    void* context_;
    std::size_t size_{0};
    bool failed_{false};
};

struct CommandResult final {
    CommandStatus status;
    bool response_suppressed;
    bool output_failed;
};

/** @brief Maximum number of commands accepted in one synchronous chain. */
inline constexpr std::size_t kMaximumCommandSegments = 16;

/** @brief Maximum canonical response payload retained by the MQTT result mirror. */
inline constexpr std::size_t kMaximumCommandResponseLength = 1024;

/**
 * @brief Maximum storage needed to decode one transport command segment.
 *
 * MQTT currently permits a 1280-byte frame and serial command lines are
 * smaller.  The command core keeps this limit independent of any transport.
 */
inline constexpr std::size_t kMaximumCommandSegmentLength = 1280;

using CommandSegmentVisitor = bool (*)(void*, std::string_view);

/**
 * @brief Validates one complete command line and its bounded chain structure.
 *
 * Semicolons delimit commands outside quoted strings and balanced JSON-like
 * objects/arrays. Empty segments are ignored. The optional count receives the
 * number of non-empty command segments.
 */
[[nodiscard]] bool validate_command_line(std::string_view line, std::size_t* count = nullptr);

/**
 * @brief Visits the raw, bounded command segments in one valid command line.
 *
 * Callers must validate the line first. Segment views remain valid only for
 * the duration of the callback and retain command-line escape bytes.
 */
[[nodiscard]] bool for_each_command_segment(
    std::string_view line, CommandSegmentVisitor visitor, void* context);

/**
 * @brief Decodes command-line escapes in one segment into caller storage.
 *
 * Outside quoted strings, `\\;` becomes `;` and `\\\\` becomes `\\`. Other
 * backslash sequences and all quoted-string contents are preserved.
 */
[[nodiscard]] bool decode_command_segment(
    std::string_view segment, char* output, std::size_t capacity, std::size_t& size);

#if USE_BACKLOG
/**
 * @brief Recognises a line whose first word is `Backlog` or `Backlog<n>`.
 *
 * Such a line is one command, as in Tasmota: its payload goes to the Backlog queue without
 * Newmota's chain splitting, chain limit or escapes.
 */
[[nodiscard]] bool is_backlog_line(std::string_view line);
#endif

class CommandProcessor final {
  public:
    /**
     * @brief Binds the processor to the services selected for this image.
     */
    constexpr CommandProcessor(Settings& settings, RestartController& restart, PowerService& power
#if USE_TIME
        ,
        TimeService& time
#endif
#if USE_BERRY
        ,
        BerryService& berry
#endif
        ,
        BootCounter* boot_counter = nullptr
#if USE_WIFI
        ,
        NetworkService* network = nullptr
#endif
#if USE_OTA_PULL
        ,
        UrlUpdateService* url_update = nullptr
#endif
#if USE_WGET
        ,
        WgetService* wget = nullptr
#endif
#if USE_PING
        ,
        PingService* ping = nullptr
#endif
        )
        : settings_(settings), restart_(restart), power_(power)
#if USE_TIME
          ,
          time_(time)
#endif
#if USE_BERRY
          ,
          berry_(berry)
#endif
          ,
          boot_counter_(boot_counter)
#if USE_WIFI
          ,
          network_(network)
#endif
#if USE_OTA_PULL
          ,
          url_update_(url_update)
#endif
#if USE_WGET
          ,
          wget_(wget)
#endif
#if USE_PING
          ,
          ping_(ping)
#endif
    {
    }

#if USE_MQTT
    using MqttResultPublisher = bool (*)(void*, std::string_view, std::string_view);

    /** @brief Mirrors visible JSON results without coupling command handlers to MQTT. */
    void set_mqtt_result_publisher(MqttResultPublisher publisher, void* context) {
        mqtt_result_publisher_ = publisher;
        mqtt_result_context_ = context;
    }

    using MqttPublisher = bool (*)(void*, std::string_view, std::string_view, bool);

    void set_mqtt_publisher(MqttPublisher publisher, void* context) {
        mqtt_publisher_ = publisher;
        mqtt_publish_context_ = context;
    }
#endif
    /**
     * @brief Parses and executes one command line.
     * @return The command status and response disposition.
     */
    [[nodiscard]] CommandResult execute(std::string_view line, CommandSource source,
        CommandOutput& output, std::uint32_t* web_output_group = nullptr);

#if USE_BACKLOG
    /** @brief Tasmota's CommandsReady(): true while the Backlog queue is empty. */
    [[nodiscard]] bool commands_ready() const { return backlog_head_ == nullptr; }

    /** @brief Tasmota's backlog_nodelay, set by Backlog0 and Backlog2 until the queue empties. */
    [[nodiscard]] bool backlog_no_delay() const { return backlog_nodelay_; }

    /** @brief Tasmota's BacklogLoop: runs the next queued command once its time is reached. */
    void poll_backlog(std::uint32_t now);
#endif

  private:
#if USE_BACKLOG
    struct BacklogEntry;
    void append_backlog(std::string_view command);
    [[nodiscard]] BacklogEntry* pop_backlog();
    [[nodiscard]] CommandStatus backlog_command(
        const CommandRequest& request, CommandOutput& output);
    [[nodiscard]] CommandStatus delay_command(const CommandRequest& request, CommandOutput& output);
    void restart_backlog_gap();
#endif
    /**
     * @brief Implements command parsing with an optional forced response suppression.
     * @return The command status and response disposition.
     */
    [[nodiscard]] CommandResult execute_impl(
        std::string_view line, CommandSource source, CommandOutput& output, bool force_suppress);
    /** @brief Executes one already validated segment of a command chain. */
    static bool execute_chain_segment(void* context, std::string_view segment);
    /**
     * @brief Routes a normalized request to its owning subsystem.
     * @return The handler status, or unknown when no command matches.
     */
    [[nodiscard]] CommandStatus dispatch(const CommandRequest& request, CommandOutput& output);

#if USE_MQTT
    MqttResultPublisher mqtt_result_publisher_{};
    void* mqtt_result_context_{};
    MqttPublisher mqtt_publisher_{};
    void* mqtt_publish_context_{};
#endif
    Settings& settings_;
    RestartController& restart_;
    PowerService& power_;
#if USE_TIME
    TimeService& time_;
#endif
#if USE_BERRY
    BerryService& berry_;
#endif
    BootCounter* boot_counter_;
#if USE_WIFI
    NetworkService* network_;
#endif
#if USE_OTA_PULL
    UrlUpdateService* url_update_;
#endif
#if USE_WGET
    WgetService* wget_;
#endif
#if USE_PING
    PingService* ping_;
#endif
#if USE_BACKLOG
    BacklogEntry* backlog_head_{};
    BacklogEntry* backlog_tail_{};
    std::uint32_t backlog_timer_{};
    bool backlog_nodelay_{};
    bool backlog_no_mqtt_response_{};
#endif
};

/**
 * @brief Parses Tasmota-style OFF, ON, and TOGGLE values.
 * @return The parsed state, or invalid for an unsupported payload.
 */
[[nodiscard]] CommandState parse_command_state(std::string_view payload);

} // namespace newmota
