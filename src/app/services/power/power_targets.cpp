/*
  power_targets.cpp - Explicit logical output selections for portable policy consumers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  FORCE_OFF/RestorePower/ExecuteCommandPower reference: Tasmota support_tasmota.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file power_targets.cpp
 * @brief Keeps selected forced OFF, explicit scoped restore and ordinary retry operations distinct.
 */
#include "services/power/power_service.h"
#if USE_PWM || USE_WS2812
#include "services/light/light_service.h"
#endif
#if USE_ZERO_CROSS
#include "drivers/gpio/zero_cross.h"
#endif

namespace newmota {
PowerService::TargetUpdate::TargetUpdate(PowerService* owner, bool enabled)
    : owner_(enabled ? owner : nullptr) {
    if (owner_)
        ++owner_->target_update_depth_;
}

PowerService::TargetUpdate::~TargetUpdate() {
    if (owner_ && !--owner_->target_update_depth_)
        owner_->observe_targets();
}

std::uint32_t PowerService::target_intent_mask() const {
    auto on = state_mask_;
#if USE_PWM || USE_WS2812
    if (light_)
        on |= light_->intent_mask();
#endif
    return on;
}

void PowerService::remember_targets(std::uint32_t on) {
    if (on)
        last_nonzero_targets_ = on;
}

void PowerService::observe_targets() {
    if (!target_update_depth_)
        remember_targets(target_intent_mask());
}

bool PowerService::last_nonzero_targets(std::uint32_t selection, std::uint32_t& on) const {
    std::uint32_t current{};
    if (!target_state(selection, current))
        return false;
    on = last_nonzero_targets_ & selection;
    return true;
}

PowerChangeResult PowerService::restore_last_nonzero(
    std::uint32_t selection, PowerOperationSource source) {
    std::uint32_t on{};
    if (!last_nonzero_targets(selection, on))
        return PowerChangeResult::unavailable;
    return restore_selected(selection, on, source);
}

bool PowerService::target_state(std::uint32_t selection, std::uint32_t& on) const {
    if (!selection)
        return false;
    std::uint32_t available{};
    for (unsigned i = 0; i < 32; ++i) {
        if (!(selection & (std::uint32_t{1} << i)))
            continue;
        bool enabled{};
        if (state(static_cast<std::uint8_t>(i + 1), enabled)) {
            available |= std::uint32_t{1} << i;
        }
    }
    if (available != selection)
        return false;
    on = target_intent_mask() & selection;
    return true;
}

PowerChangeResult PowerService::force_off_selected(
    std::uint32_t selection, PowerOperationSource source) {
    std::uint32_t on{};
    if (!target_state(selection, on))
        return PowerChangeResult::unavailable;
    return write_direct(selection, 0, true, true, source);
}

PowerChangeResult PowerService::force_off_group(
    std::uint32_t selection, PowerOperationSource source) {
    std::uint32_t on{};
    if (!target_state(selection, on))
        return PowerChangeResult::unavailable;
    return write_direct(selection, 0, false, true, source);
}

PowerChangeResult PowerService::restore_selected(
    std::uint32_t selection, std::uint32_t on, PowerOperationSource source) {
    std::uint32_t current{};
    if ((on & ~selection) || !target_state(selection, current))
        return PowerChangeResult::unavailable;
    /** RestorePower compares accepted logical intent, not a fading Light's applied sample. */
    if ((target_intent_mask() & selection) == on)
        return PowerChangeResult::unchanged;
    return write_direct(selection, on, false, false, source);
}

PowerChangeResult PowerService::set_selected(
    std::uint32_t selection, PowerAction action, PowerOperationSource source) {
    std::uint32_t current{};
    if (!target_state(selection, current))
        return PowerChangeResult::unavailable;
#if USE_ZERO_CROSS
    zero_cross::WriteGroup timing_group;
#endif
    TargetUpdate update(this);
    bool failed{}, changed{};
    for (unsigned i = 0; i < 32; ++i) {
        if (!(selection & (std::uint32_t{1} << i)))
            continue;
        const auto result = set(static_cast<std::uint8_t>(i + 1), action, source);
        failed = failed || result == PowerChangeResult::failed ||
                 result == PowerChangeResult::unavailable;
        changed = changed || result == PowerChangeResult::changed;
    }
    return failed    ? PowerChangeResult::failed
           : changed ? PowerChangeResult::changed
                     : PowerChangeResult::unchanged;
}
} // namespace newmota
