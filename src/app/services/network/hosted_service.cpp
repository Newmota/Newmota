/*
  hosted_service.cpp - ESP-Hosted companion pins and commands for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/network/hosted_service.h"

#include "gpio_ids.h"
#include "newmota/platform/hosted.h"
#include "newmota_features.h"

#if defined(NEWMOTA_IMAGE_APPLICATION)
#include "core/storage/filesystem.h"
#include "core/system/restart.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "services/settings/settings.h"
#include "support/text.h"
#if USE_WGET
#include "services/network/wget_service.h"
#endif

#include <array>
#include <cstdio>
#include <cstring>
#endif

namespace newmota::hosted {
namespace {
PinId assigned(const Hardware& hardware, const std::uint16_t base) {
    const auto function = generated::gpio::encode(base, 1U);
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function && binding->active())
            return binding->pin;
    }
    return PinId{};
}
} // namespace

void initialize(const Hardware& hardware) {
    platform::hosted::Pins pins{};
    pins.clk = assigned(hardware, generated::gpio::kGpioHsdioClkBase);
    pins.cmd = assigned(hardware, generated::gpio::kGpioHsdioCmdBase);
    pins.d0 = assigned(hardware, generated::gpio::kGpioHsdioD0Base);
    pins.d1 = assigned(hardware, generated::gpio::kGpioHsdioD1Base);
    pins.d2 = assigned(hardware, generated::gpio::kGpioHsdioD2Base);
    pins.d3 = assigned(hardware, generated::gpio::kGpioHsdioD3Base);
    pins.reset = assigned(hardware, generated::gpio::kGpioHsdioRstBase);
    platform::hosted::set_pins(pins);
}

#if defined(NEWMOTA_IMAGE_APPLICATION)
namespace {
constexpr std::size_t kChunk = 1024;
constexpr std::uint32_t kSliceMs = 40;
constexpr std::size_t kProgressBytes = 102400;

enum class State : std::uint8_t { idle, downloading, loading };

struct Update final {
    const Settings* settings{};
    WgetService* wget{};
    RestartController* restart{};
    State state{State::idle};
    std::array<char, 200> source{};
    filesystem::FileReader reader{};
    std::uint32_t written{};
    bool remove_after{};
    const char* result{"idle"};
};

Update update{};

void format_version(const platform::hosted::Version version, char* text, const std::size_t size) {
    std::snprintf(text, size, "%u.%u.%u", unsigned(version >> 16), unsigned((version >> 8) & 0xFF),
        unsigned(version & 0xFF));
}

/** @brief Tasmota's names: a long argument is the source itself, a short one a version. */
bool resolve_source(const CommandRequest& request, const std::string_view prefix) {
    auto& source = update.source;
    source.fill('\0');
    if (request.payload.size() > 15U) {
        if (request.payload.size() >= source.size())
            return false;
        std::memcpy(source.data(), request.payload.data(), request.payload.size());
        return true;
    }
    char chip[30]{};
    platform::hosted::companion_chip(chip, sizeof(chip));
    const auto length =
        std::snprintf(source.data(), source.size(), "%.*s/coprocessor%s%.*s/network_adapter_%s.bin",
            int(prefix.size()), prefix.data(), request.payload.empty() ? "" : "/",
            int(request.payload.size()), request.payload.data(), chip);
    return length > 0 && static_cast<std::size_t>(length) < source.size();
}

void finish(const char* result, const bool success) {
    if (update.reader.native != nullptr)
        (void)filesystem::close(update.reader);
    if (update.remove_after)
        (void)filesystem::remove_if_exists(update.source.data());
    update.state = State::idle;
    update.result = result;
    if (!success) {
        AddLog(LogLevel::error, "HST: update failed: %s", result);
        return;
    }
    AddLog(LogLevel::info, "HST: Hosted MCU updated (%lu bytes), restarting",
        static_cast<unsigned long>(update.written));
    if (update.restart != nullptr)
        (void)update.restart->schedule(RestartTarget::current, platform::monotonic_milliseconds());
}

void begin_load(const char* path, const bool remove_after) {
    if (path != update.source.data()) {
        update.source.fill('\0');
        std::strncpy(update.source.data(), path, update.source.size() - 1U);
    }
    update.remove_after = remove_after;
    update.written = 0;
    update.state = State::loading;
    if (filesystem::open_reader(update.source.data(), update.reader) !=
        filesystem::ReadResult::succeeded) {
        finish("File not found", false);
        return;
    }
    // Blocks while the companion erases its update partition.
    if (platform::hosted::update_begin() != platform::hosted::Result::success)
        finish("Begin failed", false);
}

void load_slice() {
    std::uint8_t buffer[kChunk];
    const auto started = platform::monotonic_milliseconds();
    while (platform::monotonic_milliseconds() - started < kSliceMs) {
        std::size_t size{};
        if (filesystem::read(update.reader, buffer, sizeof(buffer), size) !=
            filesystem::ReadResult::succeeded) {
            finish("File read failed", false);
            return;
        }
        if (size == 0U) {
            if (update.written == 0U) {
                finish("Empty file", false);
            } else if (platform::hosted::update_end() != platform::hosted::Result::success) {
                finish("End failed", false);
            } else if (platform::hosted::update_activate() != platform::hosted::Result::success) {
                finish("Activate failed", false);
            } else {
                finish("Successful", true);
            }
            return;
        }
        if (platform::hosted::update_write(buffer, size) != platform::hosted::Result::success) {
            finish("Write failed", false);
            return;
        }
        update.written += static_cast<std::uint32_t>(size);
        if (update.written % kProgressBytes == 0U)
            AddLog(LogLevel::debug, "HST: Progress %lu KB",
                static_cast<unsigned long>(update.written / 1024U));
    }
}

CommandStatus status(CommandOutput& output) {
    char host[16]{}, companion[16]{}, chip[30]{};
    format_version(platform::hosted::host_version(), host, sizeof(host));
    format_version(platform::hosted::companion_version(), companion, sizeof(companion));
    platform::hosted::companion_chip(chip, sizeof(chip));
    return output.format("{\"Hosted\":{\"Host\":\"%s\",\"Hosted\":\"%s\",\"MCU\":\"%s\","
                         "\"Update\":\"%s\"}}",
               host, companion, chip,
               update.state == State::downloading ? "Downloading"
               : update.state == State::loading   ? "Loading"
                                                  : update.result)
               ? CommandStatus::handled
               : CommandStatus::failed;
}

CommandStatus started(const CommandRequest& request, CommandOutput& output) {
    char companion[16]{};
    format_version(platform::hosted::companion_version(), companion, sizeof(companion));
    return output.format("{\"%.*s\":\"Version %s from ", int(request.name.size()),
               request.name.data(), companion) &&
                   output.write(update.source.data()) && output.write("\"}")
               ? CommandStatus::handled
               : CommandStatus::failed;
}

CommandStatus refuse(const CommandRequest& request, CommandOutput& output, const char* why) {
    (void)output.format("{\"%.*s\":\"%s\"}", int(request.name.size()), request.name.data(), why);
    return CommandStatus::failed;
}
} // namespace

void bind(const Settings& settings, WgetService* wget, RestartController& restart) {
    update.settings = &settings;
    update.wget = wget;
    update.restart = &restart;
}

CommandStatus command(const CommandRequest& request, CommandOutput& output) {
    const bool load = support::ascii_iequals(request.name, "HostedLoad");
    const bool ota = support::ascii_iequals(request.name, "HostedOta");
    if (!load && !ota && !support::ascii_iequals(request.name, "Hosted"))
        return CommandStatus::unknown;
    if (request.has_index)
        return CommandStatus::invalid;
    if (!load && !ota)
        return status(output);
    if (busy())
        return refuse(request, output, "Busy");
    if (!platform::hosted::running())
        return refuse(request, output, "No Hosted MCU");
    if (load) {
        // Expects /coprocessor/v2.0.17/network_adapter_esp32c6.bin for "HostedLoad v2.0.17".
        if (!resolve_source(request, ""))
            return refuse(request, output, "Invalid file");
        const auto status = started(request, output);
        begin_load(update.source.data(), false);
        return status;
    }
#if USE_WGET
    // OtaUrl http://host/path/newmota.bin gives http://host/path/coprocessor/network_adapter_x.bin.
    std::string_view prefix{};
    if (update.settings != nullptr)
        prefix = update.settings->ota_url();
    prefix = prefix.substr(
        0, prefix.find_last_of('/') == std::string_view::npos ? 0U : prefix.find_last_of('/'));
    if (update.wget == nullptr || !resolve_source(request, prefix))
        return refuse(request, output, "Invalid URL");
    switch (update.wget->start(update.source.data())) {
    case WgetService::StartResult::started:
        break;
    case WgetService::StartResult::busy:
        return refuse(request, output, "Busy");
    case WgetService::StartResult::invalid_url:
        return refuse(request, output, "Invalid HTTP URL");
    case WgetService::StartResult::unavailable:
        return refuse(request, output, "Unavailable");
    case WgetService::StartResult::no_memory:
        return refuse(request, output, "Out of memory");
    }
    update.state = State::downloading;
    return started(request, output);
#else
    return refuse(request, output, "Unavailable");
#endif
}

void poll(std::uint32_t) {
    if (update.state == State::loading) {
        load_slice();
        return;
    }
#if USE_WGET
    if (update.state != State::downloading || update.wget == nullptr || update.wget->active())
        return;
    if (!update.wget->completed()) {
        update.state = State::idle;
        update.result = "Download failed";
        AddLog(LogLevel::error, "HST: download failed: %s", update.wget->error());
        return;
    }
    begin_load(update.wget->destination(), true);
#endif
}

bool busy() {
    return update.state != State::idle;
}
#endif

} // namespace newmota::hosted
