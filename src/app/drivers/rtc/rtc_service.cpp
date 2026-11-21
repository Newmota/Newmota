/*
  rtc_service.cpp - Optional external RTC owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtc_service.cpp
 * @brief Boot-only RTC selection with bounded writes following accepted network samples.
 */
#include "drivers/rtc/rtc_service.h"
#include "drivers/rtc/rtc_chips.h"
#include "newmota_features.h"
#if USE_DS3231
#include "drivers/rtc/ds3231.h"
#endif
#include "newmota/platform/i2c.h"
#include "newmota/platform/platform.h"
#include "services/time/time_service.h"
#include <array>
#include <limits>

namespace newmota::rtc {
namespace {
constexpr std::array<const Driver*, 0
#if USE_DS3231
                                        + 1
#endif
#if USE_DS1307
                                        + 1
#endif
#if USE_BM8563
                                        + 1
#endif
#if USE_PCF85063
                                        + 1
#endif
#if USE_PCF85363
                                        + 1
#endif
#if USE_RV3028
                                        + 1
#endif
#if USE_RX8010
                                        + 1
#endif
#if USE_RX8025
                                        + 1
#endif
#if USE_RX8130
                                        + 1
#endif
    >
    kDrivers{
#if USE_DS3231
        &ds3231_driver,
#endif
#if USE_DS1307
        &ds1307_driver,
#endif
#if USE_BM8563
        &bm8563_driver,
#endif
#if USE_PCF85063
        &pcf85063_driver,
#endif
#if USE_PCF85363
        &pcf85363_driver,
#endif
#if USE_RV3028
        &rv3028_driver,
#endif
#if USE_RX8010
        &rx8010_driver,
#endif
#if USE_RX8025
        &rx8025_driver,
#endif
#if USE_RX8130
        &rx8130_driver,
#endif
    };
} // namespace

const char* state_name(const State state) {
    switch (state) {
    case State::unavailable:
        return "unavailable";
    case State::conflict:
        return "conflict";
    case State::invalid:
        return "invalid";
    case State::io_error:
        return "io_error";
    case State::ready:
        return "ready";
    }
    return "unavailable";
}

const char* write_result_name(const WriteResult result) {
    switch (result) {
    case WriteResult::never:
        return "never";
    case WriteResult::unchanged:
        return "unchanged";
    case WriteResult::success:
        return "success";
    case WriteResult::range_error:
        return "range_error";
    case WriteResult::io_error:
        return "io_error";
    case WriteResult::verify_error:
        return "verify_error";
    }
    return "never";
}

void Service::reset() {
    if (time_)
        time_->bind_automatic_sync(nullptr, nullptr);
    i2c_devices::release(lease_);
    lease_ = {};
    driver_ = nullptr;
    prepared_ = false;
    time_ = nullptr;
    snapshot_ = {};
    gps_write_seen_ = false;
    gps_write_succeeded_ = false;
    gps_write_at_ = 0;
}

void Service::observe(const ReadResult result) {
    snapshot_.valid = result == ReadResult::valid;
    switch (result) {
    case ReadResult::valid:
        snapshot_.state = State::ready;
        break;
    case ReadResult::invalid:
        snapshot_.state = State::invalid;
        break;
    case ReadResult::unavailable:
        snapshot_.state = State::unavailable;
        break;
    case ReadResult::io_error:
        snapshot_.state = State::io_error;
        break;
    }
}

void Service::initialize(TimeService& time, const std::uint32_t now) {
    reset();
    time_ = &time;
    time.bind_automatic_sync(
        [](void* context, TimeSource source, std::uint32_t epoch, std::uint32_t milliseconds) {
            static_cast<Service*>(context)->automatic_sync(source, epoch, milliseconds);
        },
        this);
    for (std::uint8_t bus = 0; bus < platform::i2c::kBusCount; ++bus) {
        if (!platform::i2c::available(bus))
            continue;
        for (const auto* driver : kDrivers) {
            i2c_devices::Lease candidate{};
            if (!i2c_devices::claim(bus, driver->address, candidate)) {
                if (snapshot_.state == State::unavailable)
                    snapshot_.state = State::conflict;
                continue;
            }
            std::uint32_t epoch{};
            const auto result = driver->read(bus, epoch);
            if (result == ReadResult::unavailable || result == ReadResult::io_error) {
                i2c_devices::release(candidate);
                if (result == ReadResult::io_error)
                    snapshot_.state = State::io_error;
                continue;
            }
            driver_ = driver;
            lease_ = candidate;
            snapshot_.chip = driver->name;
            snapshot_.bus = static_cast<std::uint8_t>(bus + 1);
            prepared_ = !driver_->prepare || driver_->prepare(bus);
            observe(prepared_ ? result : ReadResult::io_error);
            if (snapshot_.valid)
                (void)time.accept_rtc_seconds(epoch, now);
            return;
        }
    }
}

void Service::automatic_sync(
    const TimeSource source, const std::uint32_t epoch, const std::uint32_t sample_milliseconds) {
    if (!driver_)
        return;
    if (source == TimeSource::gps) {
        const std::uint32_t interval = gps_write_succeeded_ ? 3600000U : 60000U;
        if (gps_write_seen_ &&
            static_cast<std::uint32_t>(sample_milliseconds - gps_write_at_) < interval)
            return;
        gps_write_seen_ = true;
        gps_write_at_ = sample_milliseconds;
        // A failed attempt waits one minute; only a healthy result earns the hourly interval.
        gps_write_succeeded_ = false;
    }
    const auto bus = static_cast<std::uint8_t>(snapshot_.bus - 1);
    if (!prepared_) {
        prepared_ = !driver_->prepare || driver_->prepare(bus);
        if (!prepared_) {
            observe(ReadResult::io_error);
            snapshot_.write = WriteResult::io_error;
            return;
        }
    }
    std::uint32_t observed{};
    const auto result = driver_->read(bus, observed);
    observe(result);
    const auto elapsed =
        static_cast<std::uint32_t>(platform::monotonic_milliseconds() - sample_milliseconds) /
        1000U;
    if (epoch > std::numeric_limits<std::uint32_t>::max() - elapsed) {
        snapshot_.write = WriteResult::range_error;
        return;
    }
    const auto current = epoch + elapsed;
    const auto difference = observed > current ? observed - current : current - observed;
    if (snapshot_.valid && difference <= 2U) {
        snapshot_.write = WriteResult::unchanged;
        if (source == TimeSource::gps)
            gps_write_succeeded_ = true;
        return;
    }
    snapshot_.write = driver_->write(bus, current);
    if (source == TimeSource::gps)
        gps_write_succeeded_ = snapshot_.write == WriteResult::success;
    // Successful drivers already verify programming. Failed writes need a fresh observation.
    observe(
        snapshot_.write == WriteResult::success ? ReadResult::valid : driver_->read(bus, observed));
}

Service& service() {
    static Service owner;
    return owner;
}
} // namespace newmota::rtc
