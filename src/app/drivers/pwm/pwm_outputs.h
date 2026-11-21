/*
  pwm_outputs.h - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once
#include "core/hardware/hardware.h"
#include "newmota/platform/pwm.h"
#include "services/light/light_settings.h"
#include <array>

namespace newmota {
/** @brief Retains resolved bindings for the portable PWM and Light service.
 *
 * Duty values use electrical linear scale. Pin ownership remains with the
 * resolved hardware binding and the platform PWM controller.
 */
class PwmOutputs final {
  public:
    /** @brief Reports whether changed settings require a device restart. */
    [[nodiscard]] bool requires_restart() const;
    /** @brief Resolves configured output pins and initializes retained state. */
    void initialize(const Hardware& hardware, const LightSettings& settings);

    /** @brief Reports whether an output index has a resolved configuration. */
    [[nodiscard]] bool configured(std::size_t index) const {
        return index < outputs_.size() && outputs_[index].configured;
    }

    /** @brief Returns the current platform status for one output index. */
    [[nodiscard]] platform::PwmResult status(std::size_t index) const;

    /** @brief Returns the last write or configuration result for an output. */
    [[nodiscard]] platform::PwmResult last_operation(std::size_t index) const {
        return outputs_[index].last_operation;
    }

    /** @brief Applies selected electrical-linear duties to the retained outputs. */
    [[nodiscard]] platform::PwmResult write(
        const std::array<std::uint16_t, 16>& duties, std::uint16_t selected);
    /** @brief Stages settings changes and optionally activates them immediately. */
    [[nodiscard]] platform::PwmResult reconfigure(
        const LightSettings& settings, bool activate = true, std::uint16_t selected = 0xFFFFU);
    /** @brief Activates a previously staged output configuration. */
    [[nodiscard]] platform::PwmResult activate_reconfiguration();
    /** @brief Returns the last duty value confirmed by the platform. */
    [[nodiscard]] std::uint16_t applied(std::size_t index) const;

    /** @brief Returns the mask of outputs confirmed active by the platform. */
    [[nodiscard]] std::uint16_t confirmed_mask() const { return confirmed_mask_; }

    /** @brief Returns the mask of outputs selected by the current configuration. */
    [[nodiscard]] std::uint16_t mask() const { return mask_; }

  private:
    struct Output {
        platform::PwmRequest request{};
        platform::PwmHandle handle{};
        platform::PwmResult result{platform::PwmResult::unsupported_route};
        platform::PwmResult last_operation{platform::PwmResult::success};
        bool configured{};
    };

    std::array<Output, 16> outputs_{};
    std::uint16_t mask_{};
    std::uint16_t confirmed_mask_{};
    platform::PwmPlanToken staged_{};
    std::size_t staged_count_{};
};
} // namespace newmota
