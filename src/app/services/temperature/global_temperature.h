/*
  global_temperature.h - Canonical shared temperature observations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Global-value reference: Tasmota support.ino, Theo Arends, GPL version 3 or later.
*/

/** @file global_temperature.h
 * @brief Retains canonical global Celsius independently of display units and energy metrology.
 */
#pragma once
#include "newmota_features.h"
#include <cstdint>

namespace newmota {
/** @brief Cooperative default global-value owner; no sensor or hardware resources are acquired.
 * Automatic source observations replace the last temperature. Humidity/pressure refresh
 * the shared source validity clock without fabricating a temperature. Optional commands
 * add a volatile manual override and retained configured source selection. Raw automatic
 * observations never overwrite either mode; selected publication fields use a separate lane.
 */
class GlobalTemperature {
  public:
    /** @brief Clears volatile values at application initialization. */
    void reset();
    /** @brief Records an accepted automatic temperature before Fahrenheit/offset conversion.
     * @param celsius Canonical signed milli-Celsius, not an energy power/voltage reading.
     * @param now Monotonic application milliseconds.
     */
    void observe(std::int32_t celsius, std::uint32_t now);
    /** @brief Refreshes shared global validity from accepted humidity/pressure observations.
     * @param now Monotonic milliseconds; expired temperature is cleared before refreshing.
     */
    void refresh(std::uint32_t now);

    /** @brief Expires shared state even while no protection consumer or output is active.
     * @param now Monotonic milliseconds; called once per observed application second.
     */
    void poll(std::uint32_t now) { expire(now); }

    /** @brief Reads the last temperature with source shared 300-second validity.
     * @param now Monotonic milliseconds.
     * @param celsius Receives canonical milli-Celsius only when available.
     * @return false before a nonzero uptime observation or after shared expiry/no temperature.
     */
    bool read(std::uint32_t now, std::int32_t& celsius);

#if USE_GLOBAL_TEMPERATURE_COMMANDS
    /** @brief Installs a volatile source manual override without changing physical samples.
     * @param celsius Canonical milli-Celsius within the source manual range [-50000,100000].
     * @param now Monotonic milliseconds.
     * @return false on an out-of-range value, leaving the previous value/mode untouched.
     * Automatic temperature updates stay suppressed even after shared expiry.
     */
    bool set_manual(std::int32_t celsius, std::uint32_t now);

    /** @brief Returns to default automatic selection, retaining the last value until replaced. */
    void use_automatic() { configure_source(0); }

    /** @brief Installs a successfully saved selector and releases manual precedence.
     * @param source Retained ordinal0..250; a query must not call this method.
     */
    void configure_source(std::uint8_t source) {
        source_ = source;
        manual_ = false;
    }

    /** @brief Reports whether configured field enumeration is due at this observed boundary.
     * @param now Monotonic milliseconds.
     * @param publication Forces a collection for an actual SENSOR preparation.
     * @return true only in nonzero selection mode without a manual override.
     */
    bool selection_due(std::uint32_t now, bool publication);

    /** @brief Returns the active runtime ordinal; zero leaves automatic observations enabled. */
    std::uint8_t source() const { return source_; }

    /** @brief Installs a typed formatted-field result without clearing the retained selector.
     * @param celsius Canonical milli-Celsius after the presentation round trip.
     * @param now Monotonic observation milliseconds.
     */
    void selected(std::int32_t celsius, std::uint32_t now);

    /** @brief Reports a retained command value even during the first zero-uptime second.
     * @param now Monotonic milliseconds; shared expiry still applies.
     * @param celsius Receives canonical milli-Celsius only when present.
     * @return true for an available value, independently of the protection uptime gate.
     */
    bool value(std::uint32_t now, std::int32_t& celsius);
#endif

  private:
    /** @brief Clears an expired value before another global quantity can refresh its clock. */
    void expire(std::uint32_t now);
    /** @brief Clamps an older captured app timestamp after a command used a newer clock read. */
    std::uint32_t normalize(std::uint32_t now);
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    bool manual_{};
    std::uint8_t source_{};
    std::uint32_t selection_bucket_{};
#endif
    std::int32_t celsius_{};
    std::uint32_t updated_at_{}, now_{};
    bool present_{}, uptime_seen_{}, update_valid_{}, clock_seen_{};
};

/** @brief Returns the shared application-task global-value owner. */
GlobalTemperature& global_temperature();
} // namespace newmota
