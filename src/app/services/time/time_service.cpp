/*
  time_service.cpp - civil and network time service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file time_service.cpp
 * @brief Civil snapshots and network synchronization for portable consumers.
 */

#include "services/time/time_service.h"
#include "services/time/clock_elapsed.h"

#include "newmota/log.h"
#include "services/settings/settings.h"
#if USE_NTP
#include "newmota/platform/network.h"
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>

namespace newmota {
namespace {

constexpr std::uint32_t kSecondsPerDay = 86'400U;
#if USE_NTP
constexpr std::uint16_t kNtpPort = 123;
constexpr std::uint32_t kNtpTimeoutMilliseconds = 5'000;
constexpr std::uint32_t kRetryMilliseconds = 60'000;
constexpr std::uint32_t kRefreshMilliseconds = 60 * 60'000;
constexpr std::uint32_t kNtpToUnixSeconds = 2'208'988'800U;
constexpr std::uint32_t kNtpEraToUnixSeconds =
    std::numeric_limits<std::uint32_t>::max() - kNtpToUnixSeconds + 1U;
#endif

#if USE_NTP
/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}
#endif

/** @brief Reports whether @p year is a Gregorian leap year. */
[[nodiscard]] bool is_leap_year(const std::uint16_t year) {
    return (year % 4U == 0 && year % 100U != 0) || year % 400U == 0;
}

/** @brief Returns the number of days in one Gregorian month. */
[[nodiscard]] std::uint8_t days_in_month(const std::uint16_t year, const std::uint8_t month) {
    constexpr std::array<std::uint8_t, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap_year(year) ? 29 : kDays[month - 1];
}

/** @brief Converts a valid calendar date to Unix seconds without libc state. */
[[nodiscard]] std::uint32_t make_time(const std::uint16_t year, const std::uint8_t month,
    const std::uint8_t day, const std::uint8_t hour) {
    std::uint32_t days = 0;
    for (std::uint16_t current = 1970; current < year; ++current) {
        days += is_leap_year(current) ? 366U : 365U;
    }
    for (std::uint8_t current = 1; current < month; ++current) {
        days += days_in_month(year, current);
    }
    days += day - 1U;
    const auto hour_seconds = static_cast<std::uint32_t>(hour) * 3600U;
    if (days > (std::numeric_limits<std::uint32_t>::max() - hour_seconds) / kSecondsPerDay) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return days * kSecondsPerDay + hour_seconds;
}

/** @brief Applies a bounded signed second offset to a Unix timestamp. */
[[nodiscard]] std::uint32_t add_offset(const std::uint32_t seconds, const std::int32_t offset) {
    if (offset < 0) {
        const auto magnitude = static_cast<std::uint32_t>(-offset);
        return seconds < magnitude ? 0 : seconds - magnitude;
    }
    const auto magnitude = static_cast<std::uint32_t>(offset);
    if (seconds > std::numeric_limits<std::uint32_t>::max() - magnitude) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return seconds + magnitude;
}

#if USE_NTP
/** @brief Decodes a network-order 32-bit integer. */
[[nodiscard]] std::uint32_t read_be32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24) |
           (static_cast<std::uint32_t>(input[1]) << 16) |
           (static_cast<std::uint32_t>(input[2]) << 8) | static_cast<std::uint32_t>(input[3]);
}
#endif

} // namespace

const char* time_source_name(const TimeSource source) {
    switch (source) {
    case TimeSource::manual:
        return "manual";
    case TimeSource::ntp:
        return "ntp";
    case TimeSource::gps:
        return "gps";
    case TimeSource::rtc:
        return "rtc";
    case TimeSource::none:
        return "none";
    }
    return "none";
}

/** @brief Attempts and maintains UTC synchronization while Wi-Fi is online. */
void TimeService::poll(const std::uint32_t now, const bool network_connected) {
#if USE_NTP_SERVER
    if (synchronized_) {
        const auto elapsed = time_clock::elapsed(now, sample_age_at_);
        sample_age_ms_ = std::min<std::uint64_t>(
            sample_age_ms_ + elapsed, static_cast<std::uint64_t>(UINT32_MAX) * 1000U);
        if (elapsed != 0)
            sample_age_at_ = now;
    } else {
        sample_age_at_ = now;
    }
#endif
    // Rebase regularly so UTC survives repeated millisecond-counter rollovers.
    if (synchronized_) {
        const auto elapsed = time_clock::elapsed(now, sync_milliseconds_);
        const auto increment = elapsed / 1000U;
        sync_utc_seconds_ =
            sync_utc_seconds_ > std::numeric_limits<std::uint32_t>::max() - increment
                ? std::numeric_limits<std::uint32_t>::max()
                : sync_utc_seconds_ + increment;
        sync_milliseconds_ += (elapsed / 1000U) * 1000U;
    }
#if USE_NTP
    if (gps_active_ && !gps_fresh(now)) {
        gps_active_ = false;
        next_attempt_ = now;
    }
    if (manual_time_ || gps_fresh(now)) {
        if (pending_)
            platform::udp_cancel();
        pending_ = false;
        return;
    }
    if (!network_connected) {
        if (pending_)
            platform::udp_cancel();
        pending_ = false;
        was_connected_ = false;
        return;
    }
    if (!was_connected_) {
        was_connected_ = true;
        next_attempt_ = now;
    }
    if (!pending_) {
        if (!deadline_reached(now, next_attempt_))
            return;
        std::uint8_t skipped = 0;
        while (settings_.ntp_server(server_).empty()) {
            server_ = static_cast<std::uint8_t>((server_ + 1) % Settings::kNtpServerCount);
            if (++skipped == Settings::kNtpServerCount)
                return;
        }
        const auto result = start(now);
        // A cancelled DNS request owns the native slot until its callback drains.
        if (result == platform::NetworkResult::busy)
            return;
        pending_ = result == platform::NetworkResult::success;
        if (pending_) {
            deadline_ = now + kNtpTimeoutMilliseconds;
            return;
        }
    } else {
        std::array<std::uint8_t, 48> response{};
        std::size_t size = 0;
#if USE_NTP_SERVER
        std::array<std::uint8_t, 4> peer{};
        const auto result = platform::udp_poll(response.data(), response.size(), size, &peer);
#else
        const auto result = platform::udp_poll(response.data(), response.size(), size);
#endif
        if (result == platform::NetworkResult::busy && !deadline_reached(now, deadline_)) {
            return;
        }
        platform::udp_cancel();
        pending_ = false;
        if (!deadline_reached(now, deadline_) && result == platform::NetworkResult::success &&
            size == response.size() && accept(response.data(), now)) {
#if USE_NTP_SERVER
            upstream_address_ = peer;
#endif
            next_attempt_ = now + kRefreshMilliseconds;
            return;
        }
    }
    const auto address = settings_.ntp_server(server_);
    AddLog(LogLevel::info, "NTP: no valid reply from %.*s", static_cast<int>(address.size()),
        address.data());
    next_attempt_ = now + kRetryMilliseconds;
    server_ = static_cast<std::uint8_t>((server_ + 1) % Settings::kNtpServerCount);
#else
    (void)network_connected;
#endif
}

/** @brief Returns current UTC seconds derived from the last accepted time. */
std::uint32_t TimeService::utc_seconds(const std::uint32_t now) const {
    if (!synchronized_) {
        return 0;
    }
    const auto increment = time_clock::elapsed(now, sync_milliseconds_) / 1000U;
    return sync_utc_seconds_ > std::numeric_limits<std::uint32_t>::max() - increment
               ? std::numeric_limits<std::uint32_t>::max()
               : sync_utc_seconds_ + increment;
}

/** @brief Returns milliseconds within the current derived second. */
std::uint16_t TimeService::millisecond(const std::uint32_t now) const {
    return synchronized_
               ? static_cast<std::uint16_t>(time_clock::elapsed(now, sync_milliseconds_) % 1000U)
               : 0;
}

/** @brief Returns UTC, local, timezone, and transition state at @p now. */
TimeSnapshot TimeService::snapshot(const std::uint32_t now) const {
    TimeSnapshot result{};
    result.utc_seconds = utc_seconds(now);
    result.source = source_;
    result.valid = result.utc_seconds >= kFirstValidTime;
    if (!result.valid) {
        return result;
    }

    const auto year = civil_time(result.utc_seconds).year;
    result.standard_start_seconds =
        transition_seconds(settings_.time_rule(Settings::kStandardTimeRule), year);
    result.daylight_start_seconds =
        transition_seconds(settings_.time_rule(Settings::kDaylightTimeRule), year);
    result.offset_minutes = timezone_offset(result.utc_seconds, result.daylight_start_seconds,
        result.standard_start_seconds, &result.daylight);
    result.local_seconds = add_offset(result.utc_seconds, result.offset_minutes * 60);
    return result;
}

/** @brief Projects a UTC solar occurrence using the offset at that occurrence. */
std::uint32_t TimeService::to_local(const std::uint32_t utc) const {
    const auto year = civil_time(utc).year;
    return add_offset(
        utc, timezone_offset(utc,
                 transition_seconds(settings_.time_rule(Settings::kDaylightTimeRule), year),
                 transition_seconds(settings_.time_rule(Settings::kStandardTimeRule), year)) *
                 60);
}

/** @brief Breaks Unix seconds into compact Gregorian calendar fields. */
CivilTime TimeService::civil_time(std::uint32_t seconds) {
    CivilTime result{};
    result.second = static_cast<std::uint8_t>(seconds % 60U);
    seconds /= 60U;
    result.minute = static_cast<std::uint8_t>(seconds % 60U);
    seconds /= 60U;
    result.hour = static_cast<std::uint8_t>(seconds % 24U);
    std::uint32_t days = seconds / 24U;
    result.day_of_week = static_cast<std::uint8_t>(((days + 4U) % 7U) + 1U);

    std::uint16_t year = 1970;
    while (true) {
        const std::uint16_t year_days = is_leap_year(year) ? 366 : 365;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        ++year;
    }
    result.year = year;

    std::uint8_t month = 1;
    while (month < 12) {
        const auto month_days = days_in_month(year, month);
        if (days < month_days) {
            break;
        }
        days -= month_days;
        ++month;
    }
    result.month = month;
    result.day = static_cast<std::uint8_t>(days + 1U);
    return result;
}

bool TimeService::epoch_seconds(const CivilTime& civil, std::uint32_t& seconds) {
    return utc_calendar::epoch(
        civil.year, civil.month, civil.day, civil.hour, civil.minute, civil.second, seconds);
}

/** @brief Sets a valid UTC epoch and suspends automatic NTP updates. */
bool TimeService::set_utc_seconds(const std::uint32_t seconds, const std::uint32_t now) {
    if (seconds < kFirstValidTime) {
        return false;
    }
#if USE_NTP
    if (pending_)
        platform::udp_cancel();
    pending_ = false;
#endif
    sync_utc_seconds_ = seconds;
    sync_milliseconds_ = now;
    synchronized_ = true;
    manual_time_ = true;
    source_ = TimeSource::manual;
#if USE_NTP_SERVER
    server_reference(seconds, 0, now);
#endif
    AddLog(LogLevel::info, "RTC: synchronized from Time command");
    return true;
}

bool TimeService::accept_rtc_seconds(const std::uint32_t seconds, const std::uint32_t now) {
    if (manual_time_ || snapshot(now).valid || seconds < kFirstValidTime)
        return false;
    sync_utc_seconds_ = seconds;
    sync_milliseconds_ = now;
    synchronized_ = true;
    source_ = TimeSource::rtc;
#if USE_NTP_SERVER
    server_reference(seconds, 0, now);
#endif
    return true;
}

bool TimeService::accept_gps_seconds(
    std::uint32_t seconds, std::uint16_t fraction, std::uint32_t now) {
    if (manual_time_ || seconds < kFirstValidTime || fraction >= 1000)
        return false;
#if USE_NTP
    if (pending_)
        platform::udp_cancel();
    pending_ = false;
#endif
    sync_utc_seconds_ = seconds;
    sync_milliseconds_ = now - fraction;
    synchronized_ = true;
    gps_active_ = true;
    gps_sample_ = now;
    source_ = TimeSource::gps;
#if USE_NTP_SERVER
    server_reference(seconds, fraction, now);
#endif
    if (automatic_sync_)
        automatic_sync_(automatic_sync_context_, TimeSource::gps, seconds, now - fraction);
    return true;
}

void TimeService::invalidate_gps(std::uint32_t now) {
    const bool active = gps_active_;
    gps_active_ = false;
#if USE_NTP
    if (active)
        next_attempt_ = now;
#else
    (void)active;
    (void)now;
#endif
}

/** @brief Clears the manual-time hold and requests network synchronization. */
void TimeService::enable_network_time(const std::uint32_t now) {
    manual_time_ = false;
    retry_network_time(now);
}

/** @brief Cancels an old exchange and retries the configured list, preserving manual time. */
void TimeService::retry_network_time(const std::uint32_t now) {
#if USE_NTP
    if (pending_)
        platform::udp_cancel();
    pending_ = false;
    server_ = 0;
    next_attempt_ = now;
#else
    (void)now;
#endif
}

/** @brief Converts one configured transition rule to local Unix seconds. */
std::uint32_t TimeService::transition_seconds(const TimeRule& rule, const std::uint16_t year) {
    std::uint8_t day = 1;
    if (rule.week == 0) {
        const auto last = days_in_month(year, rule.month);
        const auto last_fields = civil_time(make_time(year, rule.month, last, rule.hour));
        day = static_cast<std::uint8_t>(
            last - ((last_fields.day_of_week + 7U - rule.day_of_week) % 7U));
    } else {
        const auto first = civil_time(make_time(year, rule.month, 1, rule.hour));
        day = static_cast<std::uint8_t>(
            1U + ((rule.day_of_week + 7U - first.day_of_week) % 7U) + 7U * (rule.week - 1U));
    }
    return make_time(year, rule.month, day, rule.hour);
}

/** @brief Selects the configured offset and independently reports the active daylight rule. */
std::int16_t TimeService::timezone_offset(const std::uint32_t utc,
    const std::uint32_t daylight_start, const std::uint32_t standard_start, bool* active) const {
    if (active)
        *active = false;
    if (!settings_.timezone_automatic()) {
        return settings_.timezone_offset_minutes();
    }

    const auto standard = settings_.time_rule(Settings::kStandardTimeRule).offset_minutes;
    const auto daylight = settings_.time_rule(Settings::kDaylightTimeRule).offset_minutes;
    const auto daylight_utc = add_offset(daylight_start, -standard * 60);
    const auto standard_utc = add_offset(standard_start, -daylight * 60);
    const bool southern = settings_.time_rule(Settings::kDaylightTimeRule).hemisphere != 0;
    const bool in_daylight = southern ? !(utc >= standard_utc && utc < daylight_utc)
                                      : utc >= daylight_utc && utc < standard_utc;
    if (active)
        *active = in_daylight;
    return in_daylight ? daylight : standard;
}

#if USE_NTP
/** @brief Starts a Tasmota-style request with an echoed transaction token. */
platform::NetworkResult TimeService::start(const std::uint32_t now) {
#if USE_NTP_SERVER
    request_started_at_ = now;
#endif
    std::array<std::uint8_t, 48> request{};
    request[0] = 0x23;
    request[2] = 6;
    request[3] = 0xec;
    request[12] = 49;
    request[13] = 0x4e;
    request[14] = 49;
    request[15] = 52;
    ++sequence_;
    for (unsigned index = 0; index < 4; ++index) {
        token_[index] = static_cast<std::uint8_t>(sequence_ >> (24U - 8U * index));
        token_[index + 4] = static_cast<std::uint8_t>(now >> (24U - 8U * index));
    }
    std::memcpy(request.data() + 40, token_.data(), token_.size());
    return platform::udp_start(
        settings_.ntp_server(server_), kNtpPort, request.data(), request.size());
}

/** @brief Checks unicast server mode, stratum, version and request token. */
bool TimeService::accept(const std::uint8_t* response, const std::uint32_t now) {
    const auto leap = response[0] >> 6;
    const auto version = (response[0] >> 3) & 7;
    const auto mode = response[0] & 7;
    const auto seconds = read_be32(response + 40);
    if (leap == 3 || mode != 4 || (version != 3 && version != 4) || response[1] == 0 ||
        response[1] > 15 || std::memcmp(response + 24, token_.data(), token_.size()) != 0 ||
        (seconds == 0 && read_be32(response + 44) == 0)) {
        return false;
    }
    const auto unix_seconds =
        seconds < kNtpToUnixSeconds ? seconds + kNtpEraToUnixSeconds : seconds - kNtpToUnixSeconds;
    if (unix_seconds < kFirstValidTime) {
        return false;
    }
    sync_utc_seconds_ = unix_seconds;
    sync_milliseconds_ = now;
    synchronized_ = true;
    manual_time_ = false;
    const auto address = settings_.ntp_server(server_);
    source_ = TimeSource::ntp;
#if USE_NTP_SERVER
    server_reference(unix_seconds, 0, now);
    upstream_stratum_ = response[1];
    upstream_leap_ = static_cast<std::uint8_t>(leap);
    // Include our bounded exchange (including DNS/scheduling) in the total root path.
    const auto upstream_delay = static_cast<std::int32_t>(read_be32(response + 4));
    const auto local_delay = static_cast<std::int64_t>(now - request_started_at_) * 65536 / 1000;
    upstream_delay_ = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(upstream_delay + local_delay, 0, INT32_MAX));
    upstream_dispersion_ = read_be32(response + 8);
#endif
    if (automatic_sync_)
        automatic_sync_(automatic_sync_context_, TimeSource::ntp, unix_seconds, now);
    AddLog(LogLevel::info, "NTP: synchronized from %.*s", static_cast<int>(address.size()),
        address.data());
    return true;
}
#endif

#if USE_NTP_SERVER
void TimeService::server_reference(
    std::uint32_t seconds, std::uint16_t fraction, std::uint32_t now) {
    reference_seconds_ = seconds;
    reference_millisecond_ = fraction;
    sample_age_ms_ = 0;
    sample_age_at_ = now;
}

std::uint32_t TimeService::sample_age_seconds(std::uint32_t now) const {
    if (!synchronized_)
        return UINT32_MAX;
    const auto age = sample_age_ms_ + time_clock::elapsed(now, sample_age_at_);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(age / 1000U, UINT32_MAX));
}

platform::NtpServerClock TimeService::ntp_server_clock(std::uint32_t now) const {
    platform::NtpServerClock result{};
    result.monotonic_ms = now;
    if (!synchronized_)
        return result;
    result.utc_seconds = utc_seconds(now);
    result.millisecond = millisecond(now);
    result.reference_seconds = reference_seconds_;
    result.reference_millisecond = reference_millisecond_;
    result.age_ms =
        std::min<std::uint64_t>(sample_age_ms_ + time_clock::elapsed(now, sample_age_at_),
            static_cast<std::uint64_t>(UINT32_MAX) * 1000U);
    result.root_dispersion = 65536U; // 1 s policy allowance, not measured clock accuracy.
    result.leap = 0;
    switch (source_) {
    case TimeSource::gps:
        result.quality = platform::NtpServerQuality::gps;
        result.stratum = gps_fresh(now) ? 1 : 16;
        result.maximum_age_ms = 5000;
        result.reference_id = {'G', 'P', 'S', 0};
        break;
    case TimeSource::ntp:
        result.quality = platform::NtpServerQuality::ntp;
        result.stratum = static_cast<std::uint8_t>(upstream_stratum_ + 1U);
        result.leap = upstream_leap_;
        result.maximum_age_ms = 3'605'000;
        result.reference_id = upstream_address_;
        result.root_delay = upstream_delay_;
        result.root_dispersion = upstream_dispersion_ > UINT32_MAX - result.root_dispersion
                                     ? UINT32_MAX
                                     : upstream_dispersion_ + result.root_dispersion;
        break;
    case TimeSource::manual:
    case TimeSource::rtc:
        result.quality = platform::NtpServerQuality::local;
        result.stratum = 10;
        result.maximum_age_ms = UINT32_MAX;
        result.reference_id = {127, 127, 1, 0};
        break;
    case TimeSource::none:
        break;
    }
    return result;
}
#endif

} // namespace newmota
