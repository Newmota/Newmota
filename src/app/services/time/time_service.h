/*
  time_service.h - civil and network time service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file time_service.h
 * @brief Civil time and explicit daylight-rule state, independent of the platform SDK.
 */

#pragma once

#include "newmota_features.h"
#include "support/utc_calendar.h"
#if USE_NTP_SERVER
#include "newmota/platform/ntp_server.h"
#endif
#if USE_NTP
#include "newmota/platform/network.h"
#endif

#include <array>
#include <cstdint>

namespace newmota {

class Settings;
struct TimeRule;

struct CivilTime final {
    std::uint16_t year{};
    std::uint8_t month{};
    std::uint8_t day{};
    std::uint8_t hour{};
    std::uint8_t minute{};
    std::uint8_t second{};
    std::uint8_t day_of_week{};
};

/** @brief Origin of the latest accepted time sample, independent of connectivity. */
enum class TimeSource : std::uint8_t { none, manual, ntp, rtc, gps };
/** @brief Stable source names shared by StatusTIM and Berry. */
[[nodiscard]] const char* time_source_name(TimeSource source);

struct TimeSnapshot final {
    std::uint32_t utc_seconds{};
    std::uint32_t local_seconds{};
    std::uint32_t daylight_start_seconds{};
    std::uint32_t standard_start_seconds{};
    std::int16_t offset_minutes{};
    bool valid{};
    TimeSource source{TimeSource::none};
    bool daylight{}; /**< Active automatic DST rule, even if its offset equals standard time. */
};

class TimeService final {
  public:
    /** @brief Shared earliest trusted Unix timestamp for all intake sources. */
    static constexpr std::uint32_t kFirstValidTime = utc_calendar::kFirstValidTime;
    /** @brief Receives accepted UTC seconds and their monotonic millisecond anchor. */
    using AutomaticSyncCallback = void (*)(void*, TimeSource, std::uint32_t, std::uint32_t);

    /** @brief Binds civil-time calculations to persistent timezone settings. */
    explicit constexpr TimeService(const Settings& settings) : settings_(settings) {}

    /** @brief Attempts and maintains UTC synchronization while Wi-Fi is online. */
    void poll(std::uint32_t now, bool network_connected);

    /** @brief Reports whether a valid manual, network, or RTC time has been accepted. */
    [[nodiscard]] bool synchronized() const { return synchronized_; }

    /** @brief Returns derived UTC; cached caller times up to 60 s behind the anchor clamp to it. */
    [[nodiscard]] std::uint32_t utc_seconds(std::uint32_t now) const;
    /** @brief Returns milliseconds within the current derived second. */
    [[nodiscard]] std::uint16_t millisecond(std::uint32_t now) const;
    /** @brief Returns UTC, local, timezone, and transition state at @p now. */
    [[nodiscard]] TimeSnapshot snapshot(std::uint32_t now) const;
#if USE_NTP_SERVER
    /** @brief Publishes UTC and true last-sample provenance for the optional server. */
    [[nodiscard]] platform::NtpServerClock ntp_server_clock(std::uint32_t now) const;
    /** @brief Returns accepted-sample age in seconds, saturated across monotonic wraps. */
    [[nodiscard]] std::uint32_t sample_age_seconds(std::uint32_t now) const;
#endif
    /** @brief Projects an arbitrary UTC instant through the configured timezone rules. */
    [[nodiscard]] std::uint32_t to_local(std::uint32_t utc) const;
    /** @brief Breaks Unix seconds into compact Gregorian calendar fields. */
    [[nodiscard]] static CivilTime civil_time(std::uint32_t seconds);
    /** @brief Validates Gregorian civil fields and converts within the uint32 epoch range. */
    [[nodiscard]] static bool epoch_seconds(const CivilTime& civil, std::uint32_t& seconds);
    /** @brief Sets a valid UTC epoch and suspends automatic NTP updates. */
    [[nodiscard]] bool set_utc_seconds(std::uint32_t seconds, std::uint32_t now);
    /** @brief Accepts an RTC bootstrap only while time is invalid and no manual hold exists. */
    [[nodiscard]] bool accept_rtc_seconds(std::uint32_t seconds, std::uint32_t now);

    /** @brief Accepts normalized GPS UTC unless a manual hold is active.
     * Fresh accepted GPS suppresses NTP for five seconds; fractions anchor the clock.
     */
    [[nodiscard]] bool accept_gps_seconds(
        std::uint32_t seconds, std::uint16_t millisecond, std::uint32_t now);
    /** @brief Invalidates GPS priority immediately on observed serial loss or invalid UTC. */
    void invalidate_gps(std::uint32_t now);

    /** @brief Reports fresh accepted GPS independently of the holdover source. */
    [[nodiscard]] bool gps_fresh(std::uint32_t now) const {
        return gps_active_ && static_cast<std::uint32_t>(now - gps_sample_) < 5000U;
    }

    /** @brief Binds one optional application-task observer of accepted automatic samples.
     * @param callback Receives context, accepted UTC seconds, and monotonic sample milliseconds.
     * A null callback detaches the observer.
     * @param context Application-lifetime observer state; never owned by the time service.
     */
    void bind_automatic_sync(AutomaticSyncCallback callback, void* context) {
        automatic_sync_ = callback;
        automatic_sync_context_ = context;
    }

    /** @brief Clears the manual-time hold and requests network synchronization. */
    void enable_network_time(std::uint32_t now);
    /** @brief Retries configured servers without releasing a manual Time hold. */
    void retry_network_time(std::uint32_t now);

  private:
#if USE_NTP_SERVER
    /** @brief Retains the original sample reference independently of running-clock rebasing. */
    void server_reference(std::uint32_t seconds, std::uint16_t fraction, std::uint32_t now);
    std::uint64_t sample_age_ms_{};
    std::uint32_t sample_age_at_{}, reference_seconds_{};
    std::uint16_t reference_millisecond_{};
    std::uint8_t upstream_stratum_{16}, upstream_leap_{3};
    std::array<std::uint8_t, 4> upstream_address_{};
    std::uint32_t upstream_delay_{}, upstream_dispersion_{};
#if USE_NTP
    std::uint32_t request_started_at_{};
#endif
#endif
    /** @brief Converts one configured transition rule to local Unix seconds. */
    [[nodiscard]] static std::uint32_t transition_seconds(const TimeRule& rule, std::uint16_t year);
    /** @brief Selects the configured fixed or daylight-aware UTC offset.
     * @param daylight Optional output reporting the active automatic daylight rule.
     */
    [[nodiscard]] std::int16_t timezone_offset(std::uint32_t utc_seconds,
        std::uint32_t daylight_start, std::uint32_t standard_start, bool* daylight = nullptr) const;
#if USE_NTP
    /** @brief Starts a correlated NTP request without waiting for DNS. */
    [[nodiscard]] platform::NetworkResult start(std::uint32_t now);
    /** @brief Validates the selected peer's reply and updates UTC. */
    [[nodiscard]] bool accept(const std::uint8_t* response, std::uint32_t now);
#endif

    const Settings& settings_;
    std::uint32_t sync_utc_seconds_{};
    std::uint32_t sync_milliseconds_{};
#if USE_NTP
    std::uint32_t next_attempt_{};
    std::uint32_t deadline_{};
    std::uint32_t sequence_{};
    std::array<std::uint8_t, 8> token_{};
    std::uint8_t server_{};
    bool was_connected_{};
    bool pending_{};
#endif
    std::uint32_t gps_sample_{};
    bool gps_active_{};
    bool synchronized_{};
    bool manual_time_{};
    TimeSource source_{TimeSource::none};
    AutomaticSyncCallback automatic_sync_{};
    void* automatic_sync_context_{};
};

} // namespace newmota
