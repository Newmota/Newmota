/*
  ping_service.cpp - Single cooperative IPv4 ping campaign

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/network/ping_service.h"
#include "newmota/platform/ping.h"
#include "newmota/platform/platform.h"
#if USE_WEB_CONSOLE
#include "newmota/console_history.h"
#include "newmota/log.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace newmota {
namespace {
constexpr std::uint32_t kDnsTimeout = 5000, kProbeTimeout = 1000;

/** @brief Rejects URL syntax, whitespace, IPv6, and ambiguous numeric IPv4 spellings. */
bool destination_valid(std::string_view host) {
    if (host.empty() || host.size() > 127)
        return false;
    bool numeric = true;
    for (const char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '.' || c == '-'))
            return false;
        if ((c < '0' || c > '9') && c != '.')
            numeric = false;
    }
    unsigned int labels = 0;
    while (!host.empty()) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        ++labels;
        if (numeric) {
            if (label.size() > 3 || (label.size() > 1 && label.front() == '0'))
                return false;
            unsigned int value = 0;
            for (const char c : label)
                value = value * 10U + static_cast<unsigned int>(c - '0');
            if (value > 255)
                return false;
        }
        if (dot == std::string_view::npos)
            break;
        host.remove_prefix(dot + 1);
        if (host.empty() && numeric)
            return false;
    }
    return !numeric || labels == 4;
}
} // namespace

const char* PingService::state_name() const {
    switch (state_) {
    case State::idle:
        return has_result_ ? "completed" : "idle";
    case State::resolving:
        return "resolving";
    case State::probing:
        return "probing";
    case State::interval:
        return "probing";
    case State::draining:
        return "draining";
    case State::delivering:
        return "delivering";
    }
    return "failed";
}

CommandStatus PingService::command(const CommandRequest& request, CommandOutput& output) {
    if (request.has_index && request.index > 8)
        return CommandStatus::invalid;
    if (request.is_query()) {
        return output.format("{\"Ping\":{\"State\":\"%s\",\"Destination\":", state_name()) &&
                       output.json_string(host_.data()) &&
                       output.format(",\"Sent\":%u,\"Success\":%u,\"Timeout\":%u,\"Error\":", sent_,
                           success_, timeout_) &&
                       output.json_string(error_) && output.write("}}")
                   ? CommandStatus::handled
                   : CommandStatus::failed;
    }
    if (!destination_valid(request.payload))
        return CommandStatus::invalid;
    if (active()) {
        (void)output.write("{\"Ping\":\"Busy\"}");
        return CommandStatus::failed;
    }
    const auto started = platform::ping_start(request.payload);
    if (started != platform::NetworkResult::success) {
        (void)output.write(started == platform::NetworkResult::busy ? "{\"Ping\":\"Busy\"}"
                                                                    : "{\"Ping\":\"Unavailable\"}");
        return CommandStatus::failed;
    }
    host_.fill('\0');
    std::memcpy(host_.data(), request.payload.data(), request.payload.size());
    address_.fill('\0');
    started_at_ = platform::monotonic_milliseconds();
    // A changing campaign token also distinguishes replies across sequence reuse and clock wrap.
    token_ += 0x9e3779b9U;
    token_ ^= started_at_;
    fast_ = request.has_index && request.index == 0;
    limit_ = fast_ ? 4 : static_cast<std::uint8_t>(request.has_index ? request.index : 1);
    sent_ = success_ = timeout_ = 0;
    minimum_ = maximum_ = total_ = 0;
    sending_ = console_pending_ = message_pending_ = has_result_ = false;
    error_ = "";
    state_ = State::resolving;
    return output.write("{\"Ping\":\"Done\"}") ? CommandStatus::handled : CommandStatus::failed;
}

bool PingService::serialize_result(CommandOutput& output) const {
    return output.write("{\"Ping\":{") && output.json_string(host_.data()) &&
           output.format(":{\"Reachable\":%s,\"IP\":\"%s\",\"Success\":%u,\"Timeout\":%u,"
                         "\"MinTime\":%lu,\"MaxTime\":%lu,\"AvgTime\":%lu,\"Error\":",
               success_ ? "true" : "false", address_.data(), success_, timeout_,
               static_cast<unsigned long>(minimum_), static_cast<unsigned long>(maximum_),
               static_cast<unsigned long>(success_ ? total_ / success_ : 0)) &&
           output.json_string(error_) && output.write("}}}");
}

void PingService::finish(const char* error) {
    error_ = error;
    platform::ping_cancel();
    state_ = State::draining;
    result_size_ = 0;
    CommandOutput output{
        [](void* context, std::string_view fragment) {
            auto& self = *static_cast<PingService*>(context);
            if (fragment.size() > self.result_.size() - self.result_size_ - 2U)
                return false;
            std::memcpy(self.result_.data() + self.result_size_, fragment.data(), fragment.size());
            self.result_size_ += fragment.size();
            return true;
        },
        this};
    if (serialize_result(output)) {
#if USE_WEB_CONSOLE
        // Browser polling reads history, independently of serial and MQTT delivery.
        // Record once, before retries, and split instead of truncating a long result.
        std::array<char, console_history::kWebLineCapacity> line{};
        std::size_t stamp = 0U;
#if USE_TASMOTA_STATE
        stamp = WriteLogTimestamp(line.data(), line.size());
#endif
        for (std::size_t offset = 0; offset < result_size_;) {
            const auto size = std::min(line.size() - stamp, result_size_ - offset);
            std::memcpy(line.data() + stamp, result_.data() + offset, size);
            console_history::record_web_output(
                {line.data(), stamp + size}, console_history::OutputKind::command_result);
            offset += size;
            stamp = 0U;
        }
#endif
        result_[result_size_++] = '\r';
        result_[result_size_++] = '\n';
        console_pending_ = message_pending_ = has_result_ = true;
    }
}

void PingService::poll(std::uint32_t now, bool network_ready, bool restart_pending) {
    if (state_ == State::idle || state_ == State::delivering)
        return;
    const auto native = platform::ping_poll();
    if (state_ == State::draining) {
        if (native.state == platform::PingState::idle)
            state_ = State::delivering;
        return;
    }
    if (!network_ready || restart_pending) {
        finish(restart_pending ? "Restart pending" : "Network unavailable");
        return;
    }
    if (native.address[0] != '\0')
        std::memcpy(address_.data(), native.address, address_.size());
    if (native.state == platform::PingState::failed) {
        finish(state_ == State::resolving ? "Resolution or ICMP setup failed" : "ICMP send failed");
        return;
    }
    if (state_ == State::resolving) {
        if (now - started_at_ >= kDnsTimeout) {
            finish("DNS timeout");
            return;
        }
        if (native.state != platform::PingState::ready)
            return;
        state_ = State::interval;
    }
    if (state_ == State::probing) {
        if (sending_) {
            if (native.state != platform::PingState::waiting &&
                native.state != platform::PingState::replied) {
                if (now - started_at_ >= 15000U)
                    finish("ICMP core timeout");
                return;
            }
            sending_ = false;
            sent_at_ = native.sent_at;
            ++sent_;
        }
        if (native.state == platform::PingState::replied) {
            ++success_;
            total_ += native.elapsed_ms;
            minimum_ = success_ == 1 ? native.elapsed_ms : std::min(minimum_, native.elapsed_ms);
            maximum_ = std::max(maximum_, native.elapsed_ms);
        } else if (now - sent_at_ < kProbeTimeout) {
            return;
        } else {
            ++timeout_;
        }
        if (sent_ == limit_ || (fast_ && success_)) {
            finish("");
            return;
        }
        state_ = State::interval;
    }
    if (state_ == State::interval && (sent_ == 0 || now - sent_at_ >= kProbeTimeout)) {
        if (platform::ping_send(token_, static_cast<std::uint16_t>(sent_ + 1U)) ==
            platform::NetworkResult::success) {
            state_ = State::probing;
            sending_ = true;
        }
    }
}

void PingService::deliver(CommandOutput::Writer console, void* console_context,
    CommandOutput::Writer message, void* message_context) {
    if (!has_result_)
        return;
    if (console_pending_ && console != nullptr &&
        console(console_context, {result_.data(), result_size_}))
        console_pending_ = false;
    if (message_pending_ && message != nullptr &&
        message(message_context, {result_.data(), result_size_ - 2U}))
        message_pending_ = false;
    if (state_ == State::delivering && !console_pending_ && !message_pending_)
        state_ = State::idle;
}
} // namespace newmota
