/*
  rtc_service.h - Optional external RTC owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtc_service.h
 * @brief One leased RTC bootstrap source and observer of accepted network time.
 */
#pragma once
#include "drivers/i2c/i2c_device_claims.h"
#include "drivers/rtc/rtc_driver.h"

namespace newmota {
class TimeService;
enum class TimeSource : std::uint8_t;

namespace rtc {
enum class State : std::uint8_t { unavailable, conflict, invalid, io_error, ready };

/** @brief Last observed chip health, separate from the result of its latest write attempt. */
struct Snapshot final {
    const char* chip{"none"};
    std::uint8_t bus{}; /**< One-based bus; zero means no selected chip. */
    State state{State::unavailable};
    bool valid{};
    WriteResult write{WriteResult::never};
};

/** @brief Returns the stable diagnostic name for the latest chip observation. */
[[nodiscard]] const char* state_name(State state);
/** @brief Returns the stable diagnostic name for the latest write attempt. */
[[nodiscard]] const char* write_result_name(WriteResult result);

/** @brief Cooperative static RTC owner; no polling, hotplug, or manual-time writes. */
class Service final {
  public:
    /** @brief Releases ownership before any hardware or I2C reinitialization. */
    void reset();
    /** @brief Probes selected chips on configured buses and offers one trusted bootstrap. */
    void initialize(TimeService& time, std::uint32_t now);

    /** @brief Returns retained diagnostics without issuing an I2C transaction. */
    [[nodiscard]] Snapshot snapshot() const { return snapshot_; }

  private:
    void automatic_sync(TimeSource source, std::uint32_t epoch, std::uint32_t sample_milliseconds);
    void observe(ReadResult result);
    std::uint32_t gps_write_at_{};
    bool prepared_{};
    bool gps_write_seen_{};
    bool gps_write_succeeded_{};
    TimeService* time_{};
    const Driver* driver_{};
    i2c_devices::Lease lease_{};
    Snapshot snapshot_{};
};

/** @brief Returns the application-lifetime owner of the selected external RTC. */
[[nodiscard]] Service& service();
} // namespace rtc
} // namespace newmota
