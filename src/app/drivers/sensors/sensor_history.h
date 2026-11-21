/*
  sensor_history.h - Recent sensor history for the dashboard

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file sensor_history.h
 * @brief Keeps the last 24 hours of every sensor reading for the dashboard, in RAM only.
 *
 * Coarse mode keeps hourly averages; exact mode (SetOption177) keeps 5-minute averages and
 * falls back to coarse when its memory is not available at boot. Readings of the `age`
 * quantity are kept as presence: 1 when the reported age was below one minute in that period.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace newmota {
class CommandOutput;

class SensorHistory {
  public:
    static constexpr std::size_t kSeries = 32;

    /** @brief Allocates the pool once. @return Whether exact mode is in use. */
    bool initialize(bool exact);
    /** @brief Samples every reading on its own schedule; call once per second. */
    void poll(std::uint32_t now);
    /** @brief Writes `{"step":<s>,"values":[...]}`, oldest first, null for gaps, in the
     *  units and with the offsets the sensor cards use at the time of the request.
     *  @param key The series id reported by the readings API.
     *  @return False when no series has that id or the output is full.
     */
    [[nodiscard]] bool write_series(std::uint32_t key, CommandOutput& output) const;

  private:
    struct Series {
        std::uint32_t key;
        std::uint32_t last_bucket; // Bucket of the latest sample, to retire unused series.
        std::int32_t sum;          // Current bucket: sum (values) or maximum (presence).
        std::uint16_t count;
        std::uint8_t quantity;
        bool used;
    };

    [[nodiscard]] Series* find(std::uint32_t key, std::uint8_t quantity);
    void close_bucket(std::uint32_t bucket);
    void record(std::uint32_t key, std::uint8_t quantity, std::int32_t value);

    Series series_[kSeries]{};
    std::int16_t* values_{}; // kSeries rows of slots_ values; INT16_MIN marks no data.
    std::size_t slots_{};
    // Buckets count elapsed time, not the clock, so the 32-bit millisecond wrap is harmless.
    std::uint32_t step_ms_{}, bucket_{}, bucket_elapsed_{}, last_sample_{};
    bool started_{};
};

/** @brief Returns the singleton history owner. */
SensorHistory& sensor_history();
} // namespace newmota
