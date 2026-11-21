/*
  power_safety.cpp - Forced logical output shutdown

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  POWER_OFF_FORCE reference: Tasmota support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_safety.cpp
 * @brief Forces configured outputs off with the source always-on and existing timer policy.
 */
#include "services/power/power_service.h"
#if USE_POWER_PROVENANCE
#include "newmota/log.h"
#endif
#if USE_PWM || USE_WS2812
#include "services/light/light_service.h"
#endif
#if USE_ZERO_CROSS
#include "drivers/gpio/zero_cross.h"
#endif

namespace newmota {
PowerChangeResult PowerService::force_off(PowerOperationSource source) {
    return write_direct(UINT32_MAX, 0, false, true, source);
}

PowerChangeResult PowerService::write_direct(std::uint32_t selection, std::uint32_t on,
    bool cancel_selected, bool forced, PowerOperationSource source) {
#if USE_POWER_PROVENANCE
    if (forced)
        last_force_source_ = source;
    record_source(source);
#else
    (void)source;
    (void)forced;
#endif
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
#if USE_POWER_TARGETS
    TargetUpdate target_update(this);
#endif
    bool available = false, changed = false, failed = false;
#if USE_POWER_TARGETS
    const auto previous_intent = target_intent_mask();
#else
    const auto previous_intent = state_mask();
#endif
    std::uint32_t target =
        always_on_ ? UINT32_MAX : (previous_intent & ~selection) | (on & selection);
    if (cancel_selected) {
        /** ExecuteCommandPower(FORCE_OFF) clears selected pulse/blink without restoring blink
         * output. SetAllPower(FORCE_OFF) and direct RestorePower deliberately retain these
         * schedules.
         */
        blink_mask_ &= ~selection;
        pulse_active_mask_ &= ~selection;
    }
    if (settings_->interlock_enabled)
        for (std::size_t i = 0; i < kMaximumInterlockGroups; ++i) {
            const auto group = interlock_group(i);
            const auto on = target & group;
            if (on && (on & (on - 1)))
                target &= ~group;
        }
#if USE_ENERGY_POWER_DELTA || USE_POWER_TARGETS
    ++operation_revision_;
#endif
    for (std::size_t i = 0; i < kMaximumPowerOutputs; ++i) {
        const auto instance = static_cast<std::uint8_t>(i + 1);
        bool previous{};
        if (!state(instance, previous))
            continue;
        available = true;
        /** Source SetDevicePower reasserts ON in PowerOnState4, even for FORCE_OFF.
         * Native direct writes bypass ordinary lock policy; only selected FORCE_OFF
         * cancels its pulse/blink schedules before this complete-mask application.
         * Reapply the physical level even when retained logical state is already equal.
         */
        const bool next = (target & (std::uint32_t{1} << i)) != 0;
        if (io_.has_relay(instance)) {
            if (io_.write_relay(instance, next) != platform::GpioResult::success) {
                failed = true;
                continue;
            }
            const auto mask = std::uint32_t{1} << i;
            state_mask_ = next ? state_mask_ | mask : state_mask_ & ~mask;
            if (previous != next) {
                changed = true;
                if (!++state_revision_)
                    state_revision_ = 1;
            }
        } else {
            const auto action = next ? PowerAction::on : PowerAction::off;
            auto result = PowerChangeResult::unavailable;
#if USE_PWM || USE_WS2812
            if (light_ != nullptr) {
                const auto endpoint = light_->endpoints().get(instance);
                if (endpoint.owner == PowerOwner::light)
                    result = light_->set(endpoint.instance, action, source);
            }
#else
            (void)action;
#endif
            failed = failed || result == PowerChangeResult::failed ||
                     result == PowerChangeResult::unavailable;
            changed = changed || result == PowerChangeResult::changed;
        }
    }
    if (cancel_selected) {
        /** ExecuteCommandPower rearms PulseTime from resulting state, including OFF→ON mode5. */
        for (unsigned i = 0; i < kMaximumPowerOutputs; ++i)
            if ((selection & (std::uint32_t{1} << i)) &&
                io_.has_relay(static_cast<std::uint8_t>(i + 1)))
                arm_pulse(static_cast<std::uint8_t>(i), now_);
    }
    return failed       ? PowerChangeResult::failed
           : !available ? PowerChangeResult::unavailable
           : changed    ? PowerChangeResult::changed
                        : PowerChangeResult::unchanged;
}
} // namespace newmota
