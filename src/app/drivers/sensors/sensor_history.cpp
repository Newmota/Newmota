/*
  sensor_history.cpp - Recent sensor history for the dashboard

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sensor_history.cpp
 * @brief Samples the sensor visitor every 10 s into fixed 24-hour rings of int16 averages.
 */
#include "drivers/sensors/sensor_history.h"
#include "drivers/sensors/sensor_service.h"
#include "core/command/command.h"
#include "newmota/log.h"
#include <algorithm>
#include <climits>
#include <new>

namespace newmota {
namespace {
constexpr std::uint32_t kSamplePeriod = 10000;
constexpr std::int32_t kPresentAge = 60000; // milli-seconds
constexpr std::int16_t kNoData = INT16_MIN;

/** @brief Milli-units per stored step, by SensorQuantity; 0 is not recorded. */
constexpr std::int32_t kStep[] = {
    4000, // illuminance: 4 lx, up to 131 klx
    10,   // temperature: 0.01 °C
    10,   // humidity: 0.01 %
    100,  // pressure: 0.1 hPa
    0,    // dew point: derived from temperature and humidity
    1000, // battery: %
    1000, // moisture: %
    1000, // conductivity: µS/cm
    10,   // weight: 0.01 kg
    1000, // impedance: ohm
    1000, // signal strength: dBm
    1000, // age: presence 0/1
    1000, // state: 0/1
    10,   // voltage: 0.01 V
    1,    // current: mA
    1000, // power: W
    10,   // energy: 0.01 kWh
    1000, // carbon dioxide: ppm
    1000, // volatile organic compounds: µg/m³
    1000, // particulates: µg/m³
    1,    // distance: mm
    1000, // count
    100,  // UV index: 0.1
    0,    // apparent power
    0,    // reactive power
    0,    // power factor
    0,    // frequency
    0,    // other: a unit of the reading's own
};
static_assert(sizeof(kStep) / sizeof(kStep[0]) == std::size_t(SensorQuantity::other) + 1);

bool presence(std::uint8_t quantity) {
    return quantity == std::uint8_t(SensorQuantity::age);
}

SensorHistory history;
} // namespace

SensorHistory& sensor_history() {
    return history;
}

bool SensorHistory::initialize(bool exact) {
    if (values_ != nullptr)
        return slots_ != 24;
    if (exact) {
        slots_ = 288;
        step_ms_ = 5U * 60U * 1000U;
        values_ = new (std::nothrow) std::int16_t[kSeries * slots_];
        if (values_ == nullptr)
            AddLog(LogLevel::error, "SNS: exact history needs %u bytes, using hourly",
                unsigned(kSeries * slots_ * sizeof(std::int16_t)));
    }
    if (values_ == nullptr) {
        slots_ = 24;
        step_ms_ = 60U * 60U * 1000U;
        values_ = new (std::nothrow) std::int16_t[kSeries * slots_];
    }
    if (values_ == nullptr) {
        slots_ = 0;
        AddLog(LogLevel::error, "SNS: history unavailable");
        return false;
    }
    std::fill(values_, values_ + kSeries * slots_, kNoData);
    return slots_ != 24;
}

SensorHistory::Series* SensorHistory::find(std::uint32_t key, std::uint8_t quantity) {
    Series* free = nullptr;
    Series* oldest = nullptr;
    for (auto& series : series_) {
        if (series.used && series.key == key)
            return &series;
        if (!series.used && free == nullptr)
            free = &series;
        if (series.used &&
            (oldest == nullptr || bucket_ - series.last_bucket > bucket_ - oldest->last_bucket))
            oldest = &series;
    }
    // A full pool reuses a series that has had no sample for a whole day.
    if (free == nullptr && oldest != nullptr && bucket_ - oldest->last_bucket >= slots_)
        free = oldest;
    if (free == nullptr)
        return nullptr;
    *free = {key, bucket_, 0, 0, quantity, true};
    auto* row = values_ + (free - series_) * slots_;
    std::fill(row, row + slots_, kNoData);
    return free;
}

/** @brief Stores the averages of the finished bucket and clears skipped ones. */
void SensorHistory::close_bucket(std::uint32_t bucket) {
    const std::uint32_t elapsed = bucket - bucket_;
    for (std::size_t i = 0; i < kSeries; ++i) {
        auto& series = series_[i];
        if (!series.used)
            continue;
        auto* row = values_ + i * slots_;
        row[bucket_ % slots_] =
            series.count == 0
                ? kNoData
                : static_cast<std::int16_t>(
                      presence(series.quantity) ? series.sum : series.sum / series.count);
        for (std::uint32_t skipped = 1; skipped < std::min<std::uint32_t>(elapsed, slots_);
            ++skipped)
            row[(bucket_ + skipped) % slots_] = kNoData;
        row[bucket % slots_] = kNoData;
        series.sum = 0;
        series.count = 0;
    }
    bucket_ = bucket;
}

void SensorHistory::record(std::uint32_t key, std::uint8_t quantity, std::int32_t value) {
    auto* series = find(key, quantity);
    if (series == nullptr)
        return;
    series->last_bucket = bucket_;
    if (presence(quantity)) {
        series->sum = std::max<std::int32_t>(series->sum, value < kPresentAge ? 1 : 0);
        series->count = 1;
        return;
    }
    const auto step = kStep[quantity];
    const auto stored = std::clamp<std::int32_t>(value / step, INT16_MIN + 1, INT16_MAX);
    // Sixteen-bit sums of up to 360 samples per hour stay well inside int32.
    series->sum += stored;
    ++series->count;
}

void SensorHistory::poll(std::uint32_t now) {
    if (values_ == nullptr || (started_ && now - last_sample_ < kSamplePeriod))
        return;
    if (started_) {
        bucket_elapsed_ += now - last_sample_; // wrap-safe difference
        if (bucket_elapsed_ >= step_ms_) {
            const std::uint32_t advance = bucket_elapsed_ / step_ms_;
            bucket_elapsed_ %= step_ms_;
            close_bucket(bucket_ + advance);
        }
    }
    started_ = true;
    last_sample_ = now;
    (void)sensor_service().visit_readings(
        [](void* context, const SensorReading& reading) {
            const auto quantity = static_cast<std::uint8_t>(reading.quantity);
            if (!reading.no_history && quantity < sizeof(kStep) / sizeof(kStep[0]) &&
                kStep[quantity] != 0)
                static_cast<SensorHistory*>(context)->record(
                    sensor_series_key(reading.sensor, reading.field), quantity, reading.value);
            return true;
        },
        this);
}

bool SensorHistory::write_series(std::uint32_t key, CommandOutput& output) const {
    const Series* series = nullptr;
    for (const auto& candidate : series_)
        if (candidate.used && candidate.key == key)
            series = &candidate;
    if (series == nullptr || values_ == nullptr)
        return false;
    const auto* row = values_ + (series - series_) * slots_;
    const auto quantity = static_cast<SensorQuantity>(series->quantity);
    if (!output.format("{\"step\":%lu,\"values\":[", static_cast<unsigned long>(step_ms_ / 1000)))
        return false;
    // Oldest first: the slot after the current bucket, ending with the bucket in progress.
    for (std::size_t i = 1; i <= slots_; ++i) {
        std::int32_t value = kNoData;
        if (i < slots_)
            value = row[(bucket_ + i) % slots_];
        else if (series->count != 0)
            value = presence(series->quantity) ? series->sum : series->sum / series->count;
        bool okay = i == 1 || output.write(",");
        if (value == kNoData)
            okay = okay && output.write("null");
        else if (presence(series->quantity))
            okay = okay && output.format("%d", int(value));
        else // stored steps back to milli-units, then the cards' units and offsets
            okay = okay &&
                   sensor_service().write_value(output, quantity, value * kStep[series->quantity]);
        if (!okay)
            return false;
    }
    return output.write("]}");
}
} // namespace newmota
