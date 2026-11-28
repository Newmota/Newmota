/*
  deep_sleep_service.cpp - Tasmota's DeepSleepTime for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Adapted from Tasmota xdrv_29_deepsleep.ino,
  Copyright (C) 2023 Stefan Bode, GPL version 3 or later.
*/

#include "core/system/deep_sleep_service.h"

#include "core/storage/driver_settings_file.h"
#include "core/system/restart.h"
#include "core/system/uptime.h"
#include "gpio_ids.h"
#include "newmota/log.h"
#include "newmota/platform/deep_sleep.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include "services/settings/settings.h"
#include "services/time/time_format.h"
#include "services/time/time_service.h"
#include "support/number.h"
#include "support/text.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace newmota::deep_sleep {
namespace {
namespace retained = platform::deep_sleep;

constexpr std::uint32_t kMaximum = 10U * 366U * 24U * 60U * 60U; // Allow max 10 years sleep
constexpr std::uint32_t kMaximumCycle = 60U * 60U; // Longest sleep between retained wakes
constexpr std::uint32_t kMinimumTime = 15U;        // Allow 15 seconds skew
constexpr std::uint8_t kStartCountdown = 4U;       // Allow 4 seconds to update web console
constexpr std::uint8_t kBootCountdown = 60U;       // Extra awake time after a normal boot
constexpr std::uint32_t kNetworkTimeout = 15U;     // Sleep when no network after this uptime
constexpr std::uint32_t kExactSlip = 10000U;
constexpr std::uint32_t kTelePeriodIdle = 9999U; // Tasmota: waiting for the network

constexpr std::size_t kPayloadSize = 4U;

constexpr DriverSettingsSpec kSpec{{"/.newmota/deepsleep.a", "/.newmota/deepsleep.b"},
    {'N', 'M', 'D', 'S'}, 1, kPayloadSize, nullptr};

std::uint32_t deepsleep{}; // Settings->deepsleep
retained::Retained rtc{};  // RtcSettings.nextwakeup, deepsleep_slip, ultradeepsleep
PinId pin{};               // GPIO_DEEPSLEEP
std::uint32_t tele_period{kTelePeriodIdle}; // TasmotaGlobal.tele_period
std::uint8_t flag{};                        // deepsleep_flag
Publisher publisher{};
void* publisher_context{};

/** @brief Reads one little-endian 32-bit value. */
[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* const source) {
    return static_cast<std::uint32_t>(source[0]) | static_cast<std::uint32_t>(source[1]) << 8U |
           static_cast<std::uint32_t>(source[2]) << 16U |
           static_cast<std::uint32_t>(source[3]) << 24U;
}

/** @brief Persists DeepSleepTime; the value applies even when the write fails. */
void save(const std::uint32_t value) {
    const std::array<std::uint8_t, kPayloadSize> payload{static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 24U)};
    if (!save_driver_settings(kSpec, payload.data()))
        AddLog(LogLevel::error, "DSL: settings save failed");
}

/** @brief The pad of the active GPIO_DEEPSLEEP function, or none. */
[[nodiscard]] PinId assigned(const Hardware& hardware) {
    const auto function = generated::gpio::encode(generated::gpio::kGpioDeepsleepBase, 1U);
    for (std::size_t index = 0; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->function == function && binding->active())
            return binding->pin;
    }
    return PinId{};
}

[[nodiscard]] bool woke_from_deep_sleep() {
    return platform::restart_reason() == platform::RestartReason::deep_sleep;
}

/** @brief The longest sleep before a retained wake; without retention one sleep covers all. */
[[nodiscard]] std::uint32_t maximum_cycle() {
    return retained::retained() ? std::min(kMaximumCycle, retained::maximum_seconds())
                                : retained::maximum_seconds();
}

/** @brief Tasmota's LocalTime: the local clock, or seconds counted since boot without one. */
[[nodiscard]] std::uint32_t local_time(const TimeService& time, const std::uint32_t now) {
    const auto snapshot = time.snapshot(now);
    return snapshot.valid ? snapshot.local_seconds : uptime_seconds();
}

/** @brief Timer delay in microseconds for @p seconds, scaled by the learned slip. */
[[nodiscard]] std::uint64_t sleep_microseconds(const std::uint32_t seconds) {
    return 100U * static_cast<std::uint64_t>(rtc.slip) * seconds;
}

/** @brief Builds Tasmota's {"DeepSleep":{"Time":…,"DeepSleep":…,"Wakeup":…}}. */
[[nodiscard]] bool write_event(const Settings& settings, const TimeService& time,
    const std::uint32_t now, const std::uint32_t local, std::array<char, 160>& payload,
    std::size_t& size) {
    std::array<char, 48> date{};
    std::size_t date_size{};
    if (format_time("%Y-%m-%dT%H:%M:%S", rtc.next_wakeup, date.data(), date.size(), date_size) !=
        TimeFormatStatus::ok)
        return false;
    std::uint8_t append_timezone = 0U; // SetOption52 - Append timezone to JSON time
    (void)settings.option(52, append_timezone);
    const auto snapshot = time.snapshot(now);
    if (append_timezone != 0U && snapshot.valid) {
        const int minutes = snapshot.offset_minutes;
        const unsigned magnitude = static_cast<unsigned>(std::abs(minutes));
        const int written = std::snprintf(date.data() + date_size, date.size() - date_size,
            "%c%02u:%02u", minutes < 0 ? '-' : '+', magnitude / 60U, magnitude % 60U);
        if (written < 0 || static_cast<std::size_t>(written) >= date.size() - date_size)
            return false;
        date_size += static_cast<std::size_t>(written);
    }
    const int written = std::snprintf(payload.data(), payload.size(),
        "{\"DeepSleep\":{\"Time\":\"%.*s\",\"DeepSleep\":%lu,\"Wakeup\":%lu}}",
        static_cast<int>(date_size), date.data(), static_cast<unsigned long>(local),
        static_cast<unsigned long>(rtc.next_wakeup));
    if (written < 0 || static_cast<std::size_t>(written) >= payload.size())
        return false;
    size = static_cast<std::size_t>(written);
    return true;
}

/** @brief Tasmota's DeepSleepStart: plans the next wake, reports it and schedules sleep. */
void start(const Settings& settings, const TimeService& time, RestartController& restart,
    const std::uint32_t now) {
    auto local = local_time(time, now);
    // The slip is ideally 10000 == 100%; a device typically slips up to 4%. Anything else is a
    // wrong setting, so values above 110% or below 90% start over at 100%.
    if (rtc.next_wakeup == 0U || rtc.slip < 9000U || rtc.slip > 11000U ||
        rtc.next_wakeup > local + deepsleep) {
        if (retained::retained())
            AddLog(LogLevel::error, "DSL: Reset wrong settings wakeup: %lu, slip %lu",
                static_cast<unsigned long>(rtc.next_wakeup), static_cast<unsigned long>(rtc.slip));
        rtc.next_wakeup = 0U;
        rtc.slip = kExactSlip;
    }

    const auto awake = platform::monotonic_milliseconds() / 1000U;
    // Time slip in 0.1 seconds between the real and the calculated wake. Within 10% of the
    // sleep it counts as a timed wake; anything else was probably manual.
    auto timeslip = static_cast<std::int32_t>(rtc.next_wakeup + awake - local) * 10;
    timeslip = timeslip < -static_cast<std::int32_t>(deepsleep)  ? 0
               : timeslip > static_cast<std::int32_t>(deepsleep) ? 0
                                                                 : 1;
    rtc.next_wakeup += deepsleep; // DeepSleepCalculate without the "Wakeup" rule
    if (timeslip != 0) {
        const std::uint32_t planned = std::max<std::uint32_t>(deepsleep - awake, 5U);
        const auto slip = static_cast<std::uint64_t>(rtc.next_wakeup - local) * rtc.slip / planned;
        rtc.slip = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            std::max<std::uint64_t>(slip, 9000U), 11000U)); // Again maximum 10% deviation
    }
    AddLog(LogLevel::debug, "DSL: Time %ld, next %lu, slip %lu", static_cast<long>(timeslip),
        static_cast<unsigned long>(rtc.next_wakeup), static_cast<unsigned long>(rtc.slip));

    // The wake may be due in less than the allowed skew; move it on by whole periods then.
    if (rtc.next_wakeup <= local + kMinimumTime && deepsleep > 0U) {
        rtc.next_wakeup += ((local + kMinimumTime - rtc.next_wakeup) / deepsleep + 1U) * deepsleep;
    }

    local = local_time(time, now);
    const std::uint32_t sleeptime = std::min<std::uint32_t>(
        maximum_cycle(), rtc.next_wakeup > 0U ? rtc.next_wakeup - local : 0U);

    std::array<char, 160> payload{};
    std::size_t size{};
    if (!write_event(settings, time, now, local, payload, size))
        AddLog(LogLevel::error, "DSL: event too long");
    else if (publisher != nullptr && !publisher(publisher_context, {payload.data(), size}))
        AddLog(LogLevel::info, "DSL: event not published");

    rtc.remaining = rtc.next_wakeup - local;
    retained::store(rtc);
    if (restart.schedule_deep_sleep(sleep_microseconds(sleeptime), now) !=
        RestartRequestResult::scheduled) {
        AddLog(LogLevel::error, "DSL: restart pending, deep sleep skipped");
        flag = 0U;
    }
}

} // namespace

void initialize(const Hardware& hardware) {
    std::array<std::uint8_t, kPayloadSize> payload{};
    (void)load_driver_settings(kSpec, payload.data());
    deepsleep = read_u32(payload.data());
    pin = assigned(hardware);
    if (!retained::load(rtc))
        rtc = {};

    // Tasmota's DeepSleepReInit: go back to sleep while the requested sleep is not over.
    if (woke_from_deep_sleep() && enabled() && rtc.remaining > maximum_cycle() &&
        rtc.remaining < 1700000000U) {
        rtc.remaining -= maximum_cycle();
        AddLog(LogLevel::error, "DSL: Remain DeepSleep %lu",
            static_cast<unsigned long>(rtc.remaining));
        retained::store(rtc);
        platform::deep_sleep::enter(
            sleep_microseconds(std::min(maximum_cycle(), rtc.remaining)), false);
    }
    // Stay awake
    rtc.remaining = 0U;
    retained::store(rtc);
}

bool enabled() {
    if (deepsleep < 10U || deepsleep > kMaximum) {
        deepsleep = 0U; // Issue #6961
        return false;
    }
    if (pin.valid()) {
        // The user disables deep sleep by holding GPIO_DEEPSLEEP low.
        bool high = true;
        if (platform::gpio_configure_input(pin, platform::GpioPull::up) ==
                platform::GpioResult::success &&
            platform::gpio_read(pin, high) == platform::GpioResult::success)
            return high;
    }
    return true;
}

void bind_publisher(const Publisher callback, void* const context) {
    publisher = callback;
    publisher_context = context;
}

void poll_second(Settings& settings, const TimeService& time, RestartController& restart,
    const bool network_up, const std::uint32_t now) {
    // Tasmota's teleperiod counter, which starts once the network is up.
    const auto period = settings.tele_period_seconds();
    if (period != 0U) {
        if (tele_period >= kTelePeriodIdle) {
            if (network_up)
                tele_period = 0U;
        } else if (++tele_period >= period) {
            tele_period = 0U;
            // FUNC_AFTER_TELEPERIOD: the initial start stays awake about 40 seconds for changes.
            if (enabled() && flag == 0U && (period == 10U || period == 300U || now > 20000U))
                flag = kStartCountdown + (woke_from_deep_sleep() ? 0U : kBootCountdown);
        }
    }

    if (restart.pending())
        return;
    if (kNetworkTimeout != 0U && uptime_seconds() > kNetworkTimeout && !network_up && flag == 0U &&
        enabled()) {
        AddLog(LogLevel::error, "Error Wifi no connect %lu [s]. Deepsleep",
            static_cast<unsigned long>(kNetworkTimeout));
        flag = kStartCountdown; // Start deepsleep in 4 seconds
    }
    if (flag == 0U)
        return;

    --flag;
    AddLog(LogLevel::error, "DSL: Countdown: %u", static_cast<unsigned>(flag));
    if (!enabled()) {
        flag = 0U;
        return;
    }
    if (flag == 1U)
        start(settings, time, restart, now);
}

CommandStatus command(const CommandRequest& request, Settings& settings, CommandOutput& output) {
    if (!support::ascii_iequals(request.name, "DeepSleepTime"))
        return CommandStatus::unknown;
    if (request.has_index)
        return CommandStatus::invalid;
    std::uint32_t value{};
    if (!request.is_query() &&
        support::parse_unsigned_decimal(request.payload, UINT32_MAX, value) &&
        (value == 0U || (value > 10U && value < kMaximum))) {
        deepsleep = value;
        save(value);
        rtc.next_wakeup = 0U;
        retained::store(rtc);
        flag = value == 0U ? 0U : kStartCountdown + (woke_from_deep_sleep() ? 0U : kBootCountdown);
        // A TelePeriod is needed to go back to sleep.
        if (flag != 0U && settings.tele_period_seconds() == 0U &&
            settings.set_tele_period_seconds(Settings::kDefaultTelePeriodSeconds) ==
                SettingsUpdateResult::failed)
            AddLog(LogLevel::error, "DSL: TelePeriod save failed");
    }
    return output.format("{\"DeepSleepTime\":%lu}", static_cast<unsigned long>(deepsleep))
               ? CommandStatus::handled
               : CommandStatus::failed;
}

} // namespace newmota::deep_sleep
