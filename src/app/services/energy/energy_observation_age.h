/*
  energy_observation_age.h - Source energy observation age independent of transport traffic

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Watchdog reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_observation_age.h
 * @brief Source one-second measurement aging; transport traffic renews only when the driver says
 * so.
 */
#pragma once
#include <cstdint>

namespace newmota::energy {
/** @brief Models ENERGY_WATCHDOG4 with the source's boot grace and fifth-tick expiry. */
class ObservationAge {
  public:
    /** @brief Advances at most one observed second, then renews for a real source observation.
     * @param now Monotonic application milliseconds, wrapping at uint32.
     * @param fresh A newly captured input edge or complete source frame, not repeated polling.
     * @return true on the one-second driver-accounting boundary; missed ticks are not synthesized.
     */
    bool poll(std::uint32_t now, bool fresh) {
        expired_now_ = false;
        if (now >= 5000)
            armed_ = true;
        const bool tick = now - at_ >= 1000;
        if (tick) {
            at_ = now;
            if (armed_ && age_ <= 4) {
                ++age_;
                expired_now_ = age_ > 4;
            }
        }
        if (fresh) {
            age_ = 0;
            expired_now_ = false;
        }
        return tick;
    }

    /** @brief Copies the raw source byte without ticking, renewing or resetting observations.
     * @return Age 0..5; fifth-tick expiry saturates until an explicit fresh source event.
     */
    std::uint8_t value() const { return age_; }

    /** @brief Returns whether the source measurement has exceeded its observation-age limit. */
    bool stale() const { return age_ > 4; }

    /** @brief Returns true only on this poll's transition into the watchdog-reset state. */
    bool expired_now() const { return expired_now_; }

  private:
    std::uint32_t at_{};
    std::uint8_t age_{};
    bool armed_{}, expired_now_{};
};
} // namespace newmota::energy
