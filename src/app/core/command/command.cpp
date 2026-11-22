/*
  command.cpp - command processing implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file command.cpp
 * @brief Routes shared commands to the selected application services and drivers.
 */

#include "core/command/command.h"
#include "newmota/log.h"
#if USE_STATUS_LEDS
#include "services/power/led_commands.h"
#endif
#if USE_STATE_TEXT
#include "services/settings/state_text.h"
#endif
#if USE_GLOBAL_TEMPERATURE_COMMANDS
#include "services/temperature/global_temperature_commands.h"
#endif

#if USE_DISPLAY
#include "drivers/display/display_commands.h"
#endif
#if USE_I2C
#include "drivers/i2c/i2c_commands.h"
#endif
#if USE_I2S
#include "drivers/audio/audio_commands.h"
#endif
#if USE_BLE
#include "services/ble/ble_commands.h"
#endif
#if USE_BLE_SENSORS
#include "drivers/sensors/ble_sensors.h"
#endif
#if USE_PING
#include "services/network/ping_service.h"
#endif
#if USE_WGET
#include "services/network/wget_service.h"
#endif
#if USE_ESP_HOSTED
#include "services/network/hosted_service.h"
#endif
#if USE_ETHERNET
#include "services/network/ethernet_service.h"
#endif
#if USE_DEEPSLEEP
#include "core/system/deep_sleep_service.h"
#endif
#if USE_DEFER
#include "core/command/defer.h"
#include "core/system/restart.h"
#endif
#include "core/storage/filesystem_commands.h"
#include "core/system/boot_counter.h"
#if USE_HARDWARE_COMMANDS
#include "core/hardware/hardware_commands.h"
#endif
#include "core/system/reset_commands.h"
#include "core/system/restart_commands.h"
#include "services/settings/settings.h"
#include "services/settings/settings_commands.h"
#if USE_WIFI
#include "services/network/network_commands.h"
#include "services/network/network_service.h"
#endif
#include "services/power/power_commands.h"
#include "services/power/power_service.h"
#if USE_PWM || USE_WS2812
#include "services/light/light_commands.h"
#endif
#include "services/status/status_commands.h"
#if USE_OTA_PULL
#include "services/update/update_commands.h"
#include "services/update/url_update_service.h"
#endif
#if USE_TIMERS
#include "services/timers/timer_commands.h"
#endif
#if USE_TIME
#include "services/time/time_commands.h"
#include "services/time/time_service.h"
#endif
#if USE_BERRY
#include "services/berry/berry_service.h"
#endif
#if USE_TUYA_MCU
#include "drivers/tuya/tuya_mcu.h"
#endif
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#endif
#include "support/number.h"
#include "support/text.h"
#if USE_WIFI
#include "newmota/platform/platform.h"
#endif

#include <array>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string_view>

#if USE_ENERGY
#include "services/energy/energy_service.h"
#endif
#if USE_MODBUS_RTU
#include "drivers/modbus/rtu_service.h"
#endif

namespace newmota {
namespace {

constexpr std::size_t kCommandNameCapacity = 32;
constexpr std::size_t kFormatCapacity = 160;
constexpr std::size_t kMaximumCommandNesting = 8;

/**
 * @brief Accepts output while intentionally discarding it.
 */
[[nodiscard]] bool discard_output(void*, std::string_view) {
    return true;
}

/**
 * @brief Emits the generic JSON response for an unsuccessful command status.
 */
[[nodiscard]] bool write_status_error(CommandOutput& output, const CommandStatus status) {
    switch (status) {
    case CommandStatus::invalid:
        return output.write("{\"Command\":\"Invalid arguments\"}");
    case CommandStatus::failed:
        return output.write("{\"Command\":\"Failed\"}");
    case CommandStatus::unknown:
        return output.write("{\"Command\":\"Unknown\"}");
    case CommandStatus::handled:
        return true;
    }
    return false;
}

/** @brief Splits one command line outside quotes and balanced JSON-like values. */
template <typename Visitor>
[[nodiscard]] bool scan_command_line(
    const std::string_view line, Visitor&& visitor, std::size_t* const count) {
    std::size_t start = 0;
    std::size_t segments = 0;
    std::array<char, kMaximumCommandNesting> brackets{};
    std::size_t depth = 0;
    char quote = 0;

    const auto emit = [&](const std::size_t end) {
        auto segment = line;
        // The scanner maintains start <= end <= line.size().
        segment.remove_prefix(start);
        segment.remove_suffix(line.size() - end);
        segment = support::trim_ascii(segment);
        if (segment.empty()) {
            return true;
        }
        if (segment.size() > kMaximumCommandSegmentLength || segments == kMaximumCommandSegments) {
            return false;
        }
        if (!visitor(segment)) {
            return false;
        }
        ++segments;
        return true;
    };

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (quote != 0) {
            if (character == '\\' && index + 1U < line.size()) {
                ++index;
            } else if (character == quote) {
                quote = 0;
            }
            continue;
        }
        if (character == '\\' && index + 1U < line.size() &&
            (line[index + 1U] == ';' || line[index + 1U] == '\\')) {
            ++index;
            continue;
        }
        if (character == '\'' || character == '\"') {
            quote = character;
            continue;
        }
        if (character == '{' || character == '[') {
            if (depth == brackets.size()) {
                return false;
            }
            brackets[depth++] = character == '{' ? '}' : ']';
            continue;
        }
        if (character == '}' || character == ']') {
            if (depth == 0 || brackets[depth - 1U] != character) {
                return false;
            }
            --depth;
            continue;
        }
        if (character == ';' && depth == 0) {
            if (!emit(index)) {
                return false;
            }
            start = index + 1U;
        }
    }
    if (quote != 0 || depth != 0 || !emit(line.size())) {
        return false;
    }
    if (count != nullptr) {
        *count = segments;
    }
    return true;
}

/** @brief Checks one command token and identifies explicit reboot operations. */
[[nodiscard]] bool validate_command_segment(const std::string_view segment, bool& reboot) {
    auto command = support::trim_ascii(segment);
    if (!command.empty() && command.front() == '_') {
        command.remove_prefix(1U);
        command = support::trim_ascii(command);
    }
    if (command.empty()) {
        return false;
    }
    const auto separator = command.find_first_of(" \t\r\n\f\v");
    auto name = command.substr(0, separator);
    if (name.empty() || name.size() >= kCommandNameCapacity) {
        return false;
    }
    std::size_t digit_start = name.size();
    while (digit_start > 0 && name[digit_start - 1U] >= '0' && name[digit_start - 1U] <= '9') {
        --digit_start;
    }
    if (digit_start == 0) {
        return false;
    }
    if (digit_start < name.size()) {
        std::uint32_t value = 0;
        auto digits = name;
        digits.remove_prefix(digit_start);
        if (!support::parse_unsigned_decimal(
                digits, std::numeric_limits<std::uint16_t>::max(), value)) {
            return false;
        }
        name = name.substr(0, digit_start);
    }
    const auto payload = separator == std::string_view::npos
                             ? std::string_view{}
                             : support::trim_ascii(std::string_view{command.data() + separator + 1U,
                                   command.size() - separator - 1U});
    reboot = (support::ascii_iequals(name, "restart") || support::ascii_iequals(name, "reset")) &&
             !payload.empty() && payload != "?";
    return true;
}

} // namespace

/** @brief Validates one complete semicolon-delimited command line. */
bool validate_command_line(const std::string_view line, std::size_t* const count) {
    std::size_t segment_count = 0;
    std::size_t reboot_index = 0;
    bool saw_reboot = false;
    bool reboot_before_final = false;
    std::size_t index = 0;
    const auto visitor = [&](const std::string_view segment) {
        bool reboot = false;
        if (!validate_command_segment(segment, reboot)) {
            return false;
        }
        if (reboot) {
            if (saw_reboot) {
                reboot_before_final = true;
            }
            saw_reboot = true;
            reboot_index = index;
        }
        ++index;
        return true;
    };
    if (!scan_command_line(line, visitor, &segment_count) || segment_count == 0 ||
        reboot_before_final || (saw_reboot && reboot_index + 1U != segment_count)) {
        return false;
    }
    if (count != nullptr) {
        *count = segment_count;
    }
    return true;
}

/** @brief Visits the raw segments in one already validated command line. */
bool for_each_command_segment(
    const std::string_view line, const CommandSegmentVisitor visitor, void* const context) {
    if (visitor == nullptr) {
        return false;
    }
    return scan_command_line(
        line,
        [visitor, context](const std::string_view segment) { return visitor(context, segment); },
        nullptr);
}

/** @brief Decodes the two command-line escape sequences outside quotes. */
bool decode_command_segment(const std::string_view segment, char* const output,
    const std::size_t capacity, std::size_t& size) {
    if (output == nullptr && !segment.empty()) {
        return false;
    }
    size = 0;
    char quote = 0;
    for (std::size_t index = 0; index < segment.size(); ++index) {
        const char character = segment[index];
        if (quote == 0 && character == '\\' && index + 1U < segment.size() &&
            (segment[index + 1U] == ';' || segment[index + 1U] == '\\')) {
            if (size == capacity) {
                return false;
            }
            output[size++] = segment[++index];
            continue;
        }
        if (size == capacity) {
            return false;
        }
        output[size++] = character;
        if (quote != 0) {
            if (character == '\\' && index + 1U < segment.size()) {
                if (size == capacity) {
                    return false;
                }
                output[size++] = segment[++index];
            } else if (character == quote) {
                quote = 0;
            }
        } else if (character == '\'' || character == '\"') {
            quote = character;
        }
    }
    return quote == 0;
}

namespace {

#if USE_WEB_CONSOLE
constexpr std::size_t kWebConsoleRecordCapacity = 192U;
std::uint32_t next_web_console_group{};

[[nodiscard]] std::uint32_t allocate_web_console_group() {
    ++next_web_console_group;
    if (next_web_console_group == 0U)
        ++next_web_console_group;
    return next_web_console_group;
}

/** @brief Mirrors visible command response text into bounded Web history. */
class WebConsoleOutput final {
  public:
    WebConsoleOutput(CommandOutput& destination, const std::uint32_t group)
        : destination_(destination), group_(group) {}

    [[nodiscard]] std::uint32_t group() const { return group_; }

    static bool write(void* const context, const std::string_view text) {
        return static_cast<WebConsoleOutput*>(context)->write(text);
    }

    void set_recording(const bool enabled) {
        recording_ = enabled;
        if (!enabled) {
            size_ = 0U;
            at_line_start_ = true;
        }
    }

    void finish_segment() {
        if (recording_) {
            flush();
        }
        size_ = 0U;
        recording_ = true;
        at_line_start_ = true;
    }

  private:
    bool write(const std::string_view text) {
        if (!destination_.write(text)) {
            return false;
        }
        if (!recording_) {
            return true;
        }

        for (std::size_t index = 0U; index < text.size();) {
            const auto character = static_cast<unsigned char>(text[index]);
            if (character == '\r') {
                ++index;
                continue;
            }
            if (character == '\n') {
                flush();
                at_line_start_ = true;
                ++index;
                continue;
            }
#if USE_TASMOTA_STATE
            // A long line continues in further records without another timestamp.
            if (at_line_start_) {
                size_ = WriteLogTimestamp(line_.data(), line_.size());
            }
#endif
            at_line_start_ = false;

            std::size_t width = 1U;
            if ((character & 0xE0U) == 0xC0U) {
                width = 2U;
            } else if ((character & 0xF0U) == 0xE0U) {
                width = 3U;
            } else if ((character & 0xF8U) == 0xF0U) {
                width = 4U;
            }
            if (index + width > text.size()) {
                width = 1U;
            }
            if (size_ + width > line_.size()) {
                flush();
            }
            for (std::size_t byte = 0U; byte < width; ++byte) {
                line_[size_++] = text[index++];
            }
        }
        return true;
    }

    void flush() {
        if (size_ != 0U) {
            console_history::record_web_output(
                {line_.data(), size_}, console_history::OutputKind::command_result, group_);
            size_ = 0U;
        }
    }

    CommandOutput& destination_;
    std::uint32_t group_{};
    std::array<char, kWebConsoleRecordCapacity> line_{};
    std::size_t size_{};
    bool recording_{true};
    bool at_line_start_{true};
};
#endif

#if USE_MQTT
/** @brief Copies one canonical result while preserving the caller's own output stream. */
class MqttResultOutput final {
  public:
    explicit MqttResultOutput(CommandOutput& destination) : destination_(destination) {}

    static bool write(void* context, std::string_view text) {
        auto& mirror = *static_cast<MqttResultOutput*>(context);
        if (!mirror.destination_.write(text))
            return false;
        if (mirror.recording_)
            mirror.append(text);
        return true;
    }

    void begin(bool enabled, bool chain) {
        enabled_ = enabled;
        chain_ = chain;
        if (enabled_ && chain_)
            append("{\"Results\":[");
    }

    void begin_segment(bool visible) {
        recording_ = enabled_ && visible;
        segment_start_ = size_;
#if USE_TASMOTA_STATE
        segment_overflow_ = overflow_;
#endif
        if (recording_ && count_ != 0U)
            append(",");
        value_start_ = size_;
    }

    void finish_segment() {
        recording_ = false;
        if (size_ == value_start_) {
            size_ = segment_start_;
            return;
        }
        const auto value =
            support::trim_ascii({payload_.data() + value_start_, size_ - value_start_});
        // Filesystem and raw Berry console text are local results, not protocol JSON.
        if (value.empty() || (value.front() != '{' && value.front() != '[')) {
            size_ = segment_start_;
            return;
        }
        ++count_;
    }

#if USE_TASMOTA_STATE
    /** @brief Drops a segment that reaches MQTT another way. */
    void discard_segment() {
        recording_ = false;
        size_ = segment_start_;
        overflow_ = segment_overflow_;
    }
#endif

    std::string_view finish() {
        recording_ = false;
        if (!enabled_)
            return {};
        if (overflow_)
            return "{\"Command\":\"Response exceeded MQTT limit\"}";
        if (count_ == 0U)
            return {};
        if (chain_)
            append("]}");
        if (overflow_)
            return "{\"Command\":\"Response exceeded MQTT limit\"}";
        return {payload_.data(), size_};
    }

  private:
    void append(std::string_view text) {
        if (overflow_)
            return;
        if (text.size() > payload_.size() - size_) {
            overflow_ = true;
            return;
        }
        std::memcpy(payload_.data() + size_, text.data(), text.size());
        size_ += text.size();
    }

    CommandOutput& destination_;
    std::array<char, kMaximumCommandResponseLength> payload_{};
    std::size_t size_{}, segment_start_{}, value_start_{}, count_{};
    bool enabled_{}, chain_{}, recording_{}, overflow_{}, segment_overflow_{};
};
#endif

/** @brief Holds one synchronous command-chain execution. */
struct CommandChainContext final {
    CommandProcessor* processor;
    CommandSource source;
    CommandOutput* output;
#if USE_MQTT
    MqttResultOutput* mqtt_output;
#endif
#if USE_WEB_CONSOLE
    WebConsoleOutput* web_output;
#endif
    std::size_t count;
    std::size_t index{};
    CommandResult result{CommandStatus::handled, true, false};
};

} // namespace

/**
 * @brief Determines whether this request asks for its current value.
 */
bool CommandRequest::is_query() const {
    return !has_payload || support::trim_ascii(payload) == "?";
}

/**
 * @brief Writes one response fragment and latches writer failures.
 */
bool CommandOutput::write(const std::string_view text) {
    if (failed_ || text.empty()) {
        return !failed_;
    }
    if (writer_ == nullptr || !writer_(context_, text)) {
        failed_ = true;
        return false;
    }
    size_ += text.size();
    return true;
}

/**
 * @brief Formats and writes one response fragment without dynamic allocation.
 */
bool CommandOutput::format(const char* const format, ...) {
    std::array<char, kFormatCapacity> buffer{};
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    if (length < 0 || static_cast<std::size_t>(length) >= buffer.size()) {
        failed_ = true;
        return false;
    }
    return write({buffer.data(), static_cast<std::size_t>(length)});
}

/**
 * @brief Writes a string with JSON quoting and control-character escaping.
 */
bool CommandOutput::json_string(const std::string_view text) {
    if (!write("\"")) {
        return false;
    }
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(text[index]);
        const char* escape = nullptr;
        switch (character) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (escape == nullptr && character >= 0x20) {
            continue;
        }
        if (index > start && !write({text.data() + start, index - start})) {
            return false;
        }
        if (escape != nullptr) {
            if (!write(escape)) {
                return false;
            }
        } else if (!format("\\u%04x", static_cast<unsigned int>(character))) {
            return false;
        }
        start = index + 1;
    }
    auto tail = text;
    tail.remove_prefix(start);
    return write(tail) && write("\"");
}

/**
 * @brief Parses Tasmota-style textual and numeric command states.
 */
CommandState parse_command_state(const std::string_view payload) {
    const auto value = support::trim_ascii(payload);
    if (value == "0" || support::ascii_iequals(value, "off") ||
        support::ascii_iequals(value, "false")) {
        return CommandState::off;
    }
    if (value == "1" || support::ascii_iequals(value, "on") ||
        support::ascii_iequals(value, "true")) {
        return CommandState::on;
    }
    if (value == "2" || support::ascii_iequals(value, "toggle")) {
        return CommandState::toggle;
    }
    return CommandState::invalid;
}

/**
 * @brief Routes a normalized request through the fixed subsystem order.
 */
CommandStatus CommandProcessor::dispatch(const CommandRequest& request, CommandOutput& output) {
#if USE_WIFI
    if (network_ != nullptr) {
        network_->note_command_activity(platform::monotonic_milliseconds());
    }
#endif
#if USE_STATE_TEXT
    const auto state_status = commands::dispatch_state_text(request, settings_, output);
    if (state_status != CommandStatus::unknown)
        return state_status;
#endif
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    const auto temperature_status =
        commands::dispatch_global_temperature(request, settings_, output);
    if (temperature_status != CommandStatus::unknown)
        return temperature_status;
#endif
#if USE_MODBUS_RTU
    const auto modbus_status = modbus::dispatch(request, output);
    if (modbus_status != CommandStatus::unknown)
        return modbus_status;
#endif
#if USE_ENERGY
    const auto energy_status = energy::dispatch(request, output);
    if (energy_status != CommandStatus::unknown)
        return energy_status;
#endif
#if USE_DISPLAY
    const auto display_status = commands::dispatch_display(request, output);
    if (display_status != CommandStatus::unknown)
        return display_status;
#endif
#if USE_MATTER
    const auto matter_status = berry_.matter_command(request, output);
    if (matter_status != CommandStatus::unknown)
        return matter_status;
#endif
#if USE_CAMERA
    const auto camera_status = berry_.camera_command(request, output);
    if (camera_status != CommandStatus::unknown)
        return camera_status;
#endif
#if USE_BLE
    const auto ble_status = commands::dispatch_ble(request, output);
    if (ble_status != CommandStatus::unknown)
        return ble_status;
#endif
#if USE_BLE_SENSORS
    const auto ble_sensors_status = commands::dispatch_ble_sensors(request, output);
    if (ble_sensors_status != CommandStatus::unknown)
        return ble_sensors_status;
#endif
#if USE_I2C
    const auto i2c_status = commands::dispatch_i2c(request, output);
    if (i2c_status != CommandStatus::unknown)
        return i2c_status;
#endif
#if USE_I2S
    const auto say_time_status = commands::dispatch_audio_say_time(request, time_, output);
    if (say_time_status != CommandStatus::unknown)
        return say_time_status;
    const auto audio_status = commands::dispatch_audio(request, output);
    if (audio_status != CommandStatus::unknown)
        return audio_status;
#endif
#if USE_PWM || USE_WS2812
    if (power_.light() != nullptr) {
        const auto status = commands::dispatch_light(request, *power_.light(), output);
        if (status != CommandStatus::unknown)
            return status;
    }
#endif
#if USE_MQTT
    if (support::ascii_iequals(request.name, "publish")) {
        if (request.is_query() ||
            (request.has_index && request.index != 1U && request.index != 2U) || !mqtt_publisher_)
            return CommandStatus::invalid;
        const auto separator = request.payload.find(' ');
        const auto topic = request.payload.substr(0U, separator);
        const auto payload = separator == std::string_view::npos
                                 ? std::string_view{}
                                 : std::string_view{request.payload.data() + separator + 1U,
                                       request.payload.size() - separator - 1U};
        std::array<char, 256> decoded{};
        if (topic.empty() || topic.size() >= decoded.size())
            return CommandStatus::invalid;
        for (std::size_t i = 0U; i < topic.size(); ++i)
            decoded[i] = topic[i] == '#' ? ' ' : topic[i];
        return mqtt_publisher_(mqtt_publish_context_, {decoded.data(), topic.size()}, payload,
                   request.index == 2U)
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
#endif
#if USE_STATUS_LEDS
    const auto led_status = commands::dispatch_leds(request, settings_, power_, output);
    if (led_status != CommandStatus::unknown) {
        return led_status;
    }
#endif
    const auto power_status = commands::dispatch_power(request, power_, output);
    if (power_status != CommandStatus::unknown) {
        return power_status;
    }
#if USE_HARDWARE_COMMANDS
    const auto hardware_status = commands::dispatch_hardware(request, settings_, restart_, output);
    if (hardware_status != CommandStatus::unknown) {
        return hardware_status;
    }
#endif
#if USE_BERRY
    if (support::ascii_iequals(request.name, "br")) {
        return berry_.command(request, output);
    }
#endif
#if USE_TUYA_MCU
    const auto tuya_status = tuya::dispatch(request, output, restart_);
    if (tuya_status != CommandStatus::unknown)
        return tuya_status;
#endif
    const auto settings_status =
        commands::dispatch_settings(request, settings_, restart_, power_, output);
    if (settings_status != CommandStatus::unknown) {
        return settings_status;
    }
#if USE_WIFI
    if (network_ != nullptr) {
        const auto wifi_status = commands::dispatch_wifi_commands(request, *network_, output);
        if (wifi_status != CommandStatus::unknown) {
            return wifi_status;
        }
    }
    const auto network_status = commands::dispatch_network_queries(request, output);
    if (network_status != CommandStatus::unknown) {
        return network_status;
    }
#endif
#if USE_TIMERS
    const auto timer_status = commands::dispatch_timers(request, settings_, power_, output);
    if (timer_status != CommandStatus::unknown)
        return timer_status;
#endif
#if USE_TIME
    const auto time_status = commands::dispatch_time(request, settings_, time_, output);
    if (time_status != CommandStatus::unknown) {
        return time_status;
    }
#endif
    const auto boot_status = boot_counter_ == nullptr ? BootStatus{} : boot_counter_->status();
    const auto status_status = commands::dispatch_status(request, settings_, power_,
#if USE_TIME
        &time_,
#else
        nullptr,
#endif
        &boot_status, output);
    if (status_status != CommandStatus::unknown) {
        return status_status;
    }
#if USE_OTA_PULL
    if (url_update_ != nullptr) {
        const auto update_status = commands::dispatch_url_update(request, *url_update_, output);
        if (update_status != CommandStatus::unknown) {
            return update_status;
        }
    }
#endif
#if USE_DEEPSLEEP
    const auto deep_sleep_status = deep_sleep::command(request, settings_, output);
    if (deep_sleep_status != CommandStatus::unknown)
        return deep_sleep_status;
#endif
#if USE_ETHERNET
    const auto ethernet_status = ethernet::command(request, output);
    if (ethernet_status != CommandStatus::unknown)
        return ethernet_status;
#endif
#if USE_ESP_HOSTED && defined(NEWMOTA_IMAGE_APPLICATION)
    const auto hosted_status = hosted::command(request, output);
    if (hosted_status != CommandStatus::unknown)
        return hosted_status;
#endif
#if USE_PING
    if (ping_ != nullptr && support::ascii_iequals(request.name, "Ping"))
        return ping_->command(request, output);
#endif
#if USE_WGET
    if (wget_ != nullptr && support::ascii_iequals(request.name, "wget")) {
        return wget_->command(request, output);
    }
    if (wget_ != nullptr && wget_->active() &&
        (support::ascii_iequals(request.name, "rm") ||
            support::ascii_iequals(request.name, "mv"))) {
        (void)output.write("{\"Filesystem\":\"Download active\"}");
        return CommandStatus::failed;
    }
#endif
#if USE_DEFER
    const auto defer_status = defer::command(request, output, restart_.pending());
    if (defer_status != CommandStatus::unknown) {
        return defer_status;
    }
#endif
    const auto reset_status =
        commands::dispatch_reset(request, settings_, restart_, boot_counter_, output);
    if (reset_status != CommandStatus::unknown) {
        return reset_status;
    }
    const auto restart_status = commands::dispatch_restart(request, restart_, output);
    const auto status = restart_status == CommandStatus::unknown
                            ? commands::dispatch_filesystem(request, output)
                            : restart_status;
#if USE_BERRY_DRIVERS
    // Berry commands come last, as in Tasmota: they cannot replace a native command.
    if (status == CommandStatus::unknown)
        return berry_.script_command(request, output);
#endif
    return status;
}

/**
 * @brief Executes one already validated command segment.
 */
bool CommandProcessor::execute_chain_segment(void* const context, const std::string_view segment) {
    auto& chain = *static_cast<CommandChainContext*>(context);
    std::array<char, kMaximumCommandSegmentLength> decoded{};
    std::size_t decoded_size = 0;
    if (!decode_command_segment(segment, decoded.data(), decoded.size(), decoded_size)) {
        chain.result = {CommandStatus::invalid, false, false};
        (void)write_status_error(*chain.output, CommandStatus::invalid);
#if USE_WEB_CONSOLE
        if (chain.web_output != nullptr) {
            chain.web_output->finish_segment();
        }
#endif
        return false;
    }

#if USE_MQTT
    if (chain.mqtt_output != nullptr) {
        const auto command = support::trim_ascii({decoded.data(), decoded_size});
        chain.mqtt_output->begin_segment(command.empty() || command.front() != '_');
    }
#endif
#if USE_WEB_CONSOLE
    if (chain.web_output != nullptr) {
        const auto command = support::trim_ascii({decoded.data(), decoded_size});
        const bool berry_suppressed =
            chain.source == CommandSource::berry && !command.empty() && command.front() == '_';
        chain.web_output->set_recording(!berry_suppressed);
    }
#endif
    const auto before = chain.output->size();
    const auto result = chain.processor->execute_impl(
        {decoded.data(), decoded_size}, chain.source, *chain.output, false);
#if USE_WEB_CONSOLE
    if (chain.web_output != nullptr) {
        chain.web_output->finish_segment();
    }
#endif
#if USE_MQTT
    if (chain.mqtt_output != nullptr) {
#if USE_TASMOTA_STATE
        if (commands::finish_status_split_request(!result.response_suppressed))
            chain.mqtt_output->discard_segment();
        else
#endif
            chain.mqtt_output->finish_segment();
    }
#endif
    const bool visible = chain.output->size() != before &&
                         (chain.source == CommandSource::berry || !result.response_suppressed);
    chain.result = result;
    if (visible) {
        chain.result.response_suppressed = false;
    }
    if (visible && chain.index + 1U < chain.count && !chain.output->write("\r\n")) {
        chain.result.output_failed = true;
    }
    ++chain.index;
    return result.status == CommandStatus::handled && !chain.result.output_failed;
}

/**
 * @brief Parses and synchronously executes one semicolon-delimited command line.
 */
CommandResult CommandProcessor::execute(std::string_view line, const CommandSource source,
    CommandOutput& output, std::uint32_t* const web_output_group) {
#if USE_MQTT
    MqttResultOutput mqtt_output{output};
    CommandOutput mqtt_destination{MqttResultOutput::write, &mqtt_output};
    auto& caller_output = mqtt_destination;
#else
    auto& caller_output = output;
#endif
#if USE_WEB_CONSOLE
    WebConsoleOutput web_output{caller_output, allocate_web_console_group()};
    if (web_output_group != nullptr)
        *web_output_group = web_output.group();
    CommandOutput mirrored_output{WebConsoleOutput::write, &web_output};
    CommandOutput& destination = mirrored_output;
#else
    if (web_output_group != nullptr)
        *web_output_group = 0U;
    CommandOutput& destination = caller_output;
#endif
    std::size_t count = 0;
    if (!validate_command_line(line, &count)) {
        (void)write_status_error(destination, CommandStatus::invalid);
#if USE_WEB_CONSOLE
        web_output.finish_segment();
#endif
        return {CommandStatus::invalid, false, destination.failed()};
    }

#if USE_MQTT
    bool mqtt_enabled = source != CommandSource::mqtt;
#if USE_DEFER
    mqtt_enabled = mqtt_enabled || deferred_;
#endif
    mqtt_output.begin(mqtt_result_publisher_ != nullptr && mqtt_enabled, count > 1U);
#endif
    CommandChainContext context{this, source, &destination,
#if USE_MQTT
        &mqtt_output,
#endif
#if USE_WEB_CONSOLE
        &web_output,
#endif
        count};
    (void)for_each_command_segment(line, &CommandProcessor::execute_chain_segment, &context);
#if USE_MQTT
    auto payload = mqtt_output.finish();
    if (!payload.empty() && context.result.output_failed)
        payload = "{\"Command\":\"Failed\"}";
    if (!payload.empty()) {
        auto command = support::trim_ascii(line);
        if (!command.empty() && command.front() == '_')
            command.remove_prefix(1U);
        command = support::trim_ascii(command);
        const auto end = command.find_first_of(" \t;");
        const auto name = command.substr(0U, end);
        (void)mqtt_result_publisher_(mqtt_result_context_, name, payload);
    }
#endif
    return context.result;
}

#if USE_DEFER
/** @brief Runs one deferred line with the MQTT result mirror forced on. */
CommandResult CommandProcessor::execute_deferred(
    const std::string_view line, const CommandSource source, CommandOutput& output) {
    deferred_ = true;
    const auto result = execute(line, source, output);
    deferred_ = false;
    return result;
}
#endif

/**
 * @brief Implements command parsing and optional response suppression.
 */
CommandResult CommandProcessor::execute_impl(std::string_view line, const CommandSource source,
    CommandOutput& output, const bool force_suppress) {
    line = support::trim_ascii(line);
    bool suppress_response = force_suppress;
    if (!line.empty() && line.front() == '_') {
        suppress_response = true;
        line.remove_prefix(1);
        line = support::trim_ascii(line);
    }

    const auto separator = line.find_first_of(" \t\r\n\f\v");
    auto name = line.substr(0, separator);
    auto payload = separator == std::string_view::npos
                       ? std::string_view{}
                       : support::trim_ascii(std::string_view{
                             line.data() + separator + 1U, line.size() - separator - 1U});
    const bool has_payload = separator != std::string_view::npos && !payload.empty();

    std::array<char, kCommandNameCapacity> normalized{};
    if (name.empty() || name.size() >= normalized.size()) {
        CommandOutput discarded{discard_output, nullptr};
        auto& destination = suppress_response ? discarded : output;
        (void)write_status_error(destination, CommandStatus::invalid);
        return {CommandStatus::invalid, suppress_response, destination.failed()};
    }

    std::size_t digit_start = name.size();
    while (digit_start > 0 && name[digit_start - 1] >= '0' && name[digit_start - 1] <= '9') {
        --digit_start;
    }
    const bool has_index = digit_start < name.size();
    std::uint16_t command_index = 0;
    if (has_index) {
        if (digit_start == 0) {
            CommandOutput discarded{discard_output, nullptr};
            auto& destination = suppress_response ? discarded : output;
            (void)write_status_error(destination, CommandStatus::invalid);
            return {CommandStatus::invalid, suppress_response, destination.failed()};
        }
        std::uint32_t value = 0;
        auto digits = name;
        digits.remove_prefix(digit_start);
        if (!support::parse_unsigned_decimal(
                digits, std::numeric_limits<std::uint16_t>::max(), value)) {
            CommandOutput discarded{discard_output, nullptr};
            auto& destination = suppress_response ? discarded : output;
            (void)write_status_error(destination, CommandStatus::invalid);
            return {CommandStatus::invalid, suppress_response, destination.failed()};
        }
        command_index = static_cast<std::uint16_t>(value);
        name = name.substr(0, digit_start);
    }

    for (std::size_t index = 0; index < name.size(); ++index) {
        normalized[index] = support::ascii_lower(name[index]);
    }

    CommandOutput discarded{discard_output, nullptr};
    const bool preserve_logical_result = source == CommandSource::berry;
    auto& destination = suppress_response && !preserve_logical_result ? discarded : output;
    const CommandRequest request{
        source,
        {normalized.data(), name.size()},
        payload,
        command_index,
        has_index,
        has_payload,
    };
    const auto size_before = destination.size();
    const auto status = dispatch(request, destination);
    if (status != CommandStatus::handled && destination.size() == size_before &&
        !destination.failed()) {
        (void)write_status_error(destination, status);
    }
    return {status, suppress_response, destination.failed()};
}

} // namespace newmota
