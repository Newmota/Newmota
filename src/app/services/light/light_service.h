/*
  light_service.h - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LightPreparePower source reference: Tasmota xdrv_04_light.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file light_service.h
 * @brief Portable PWM and Light behavior for Newmota.
 */

#pragma once
#include "newmota_features.h"
#include "drivers/pwm/pwm_outputs.h"
#include "drivers/led_strip/led_strip.h"
#if USE_LED_DRIVER
#include "drivers/led_driver/led_driver.h"
#endif
#include "services/power/power_endpoints.h"
#include "services/power/power_service.h"
#include "services/settings/settings.h"

namespace newmota {
class CommandOutput;
class TimeService;

/** @brief Owns the shared Light state, PWM transitions, and pixel output. */
class LightService final {
  public:
    /** @brief Binds hardware, settings, power endpoints, and the initial time. */
    void initialize(
        const Hardware& hardware, Settings& settings, PowerService& power, std::uint32_t now);
    /** @brief Advances transitions, effects, and addressable output without blocking. */
    void poll(std::uint32_t now);

    /** @brief Attaches the time source used by Light effects. */
    void attach_time(const TimeService& time) { time_ = &time; }

    /** @brief Returns a persisted Light option by Tasmota option index. */
    [[nodiscard]] std::uint8_t option(std::uint16_t index) const;
    /** @brief Changes the addressable pixel type when the configured driver allows it. */
    [[nodiscard]] platform::PwmResult pixel_type(std::uint8_t type);

#if USE_LED_DRIVER
    /** @brief Returns the LED driver chip that owns the Light, or nullptr. */
    [[nodiscard]] const led_driver::Driver* led_driver() const {
        return chip_ ? &led_driver_ : nullptr;
    }
#endif

    /** @brief Reports whether a usable addressable strip is configured. */
    [[nodiscard]] bool addressable() const { return addressable_; }

    /** @brief Returns the active strip's pixel count, or zero when unavailable. */
    [[nodiscard]] std::size_t pixel_count() const {
#if USE_WS2812
        return addressable_ ? intent().strip_pixels : 0U;
#else
        return 0U;
#endif
    }

    /** @brief Reads one pixel from the current addressable frame. */
    [[nodiscard]] platform::PwmResult pixel(std::size_t index, led_strip::Pixel&) const;
    /** @brief Writes a bounded run of pixels, copying colors before returning. */
    [[nodiscard]] platform::PwmResult pixels(
        std::size_t index, const led_strip::Pixel*, std::size_t count);
    /** @brief Starts the selected Light effect at the supplied color. */
    void start_effect_at(std::uint16_t color);
    /** @brief Appends the current Light state fields to a command response. */
    [[nodiscard]] bool write_state_fields(CommandOutput& output, bool comma = true) const;
    /** @brief Applies new Light settings and begins the requested transition. */
    [[nodiscard]] platform::PwmResult update(const LightSettings& intent, bool turn_on,
        std::uint8_t channel = 0U, std::uint16_t effect_start = 256U);
    /** @brief Writes one raw electrical-linear PWM value. */
    [[nodiscard]] platform::PwmResult raw_write(std::size_t index, std::uint16_t value);
    /** @brief Applies timing-related settings to the selected PWM outputs. */
    [[nodiscard]] platform::PwmResult timing(
        const LightSettings& intent, std::uint16_t selected = 0xFFFFU);
    /** @brief Reads whether one logical Light endpoint is on. */
    [[nodiscard]] bool state(std::uint8_t instance, bool& on) const;
    /** @brief Applies a power action to one logical Light endpoint.
     * @param instance One-based Light endpoint.
     * @param action Requested logical state.
     * @param source Explicit Power caller origin, or Light for direct Light API calls.
     * @return Operation result; recorded origin is not physical-output confirmation.
     */
    [[nodiscard]] PowerChangeResult set(std::uint8_t instance, PowerAction action,
        PowerOperationSource source = PowerOperationSource::light);
    /** @brief Returns the bit mask of currently-on logical Light endpoints. */
    [[nodiscard]] std::uint32_t state_mask() const;

#if USE_POWER_TARGETS
    /** @brief Projects accepted logical intent to global Power bits before fades complete. */
    [[nodiscard]] std::uint32_t intent_mask() const;
#endif
    /** @brief Returns the revision incremented after applied state changes. */
    [[nodiscard]] std::uint32_t revision() const { return revision_; }

    /** @brief Returns the logical power endpoints owned by this service. */
    [[nodiscard]] const PowerEndpoints& endpoints() const { return endpoints_; }

    /** @brief Reports whether the resolved Light mapping can drive an output. */
    [[nodiscard]] bool available() const { return channel_count_ != 0U && mapping_valid_; }

    /** @brief Reports whether a PWM timing change is waiting for activation. */
    [[nodiscard]] bool timing_pending() const { return timing_pending_; }

    /** @brief Reports whether each configured channel has an independent endpoint. */
    [[nodiscard]] bool independent() const { return independent_; }

    /** @brief Reports whether warm and cold white channels are separate. */
    [[nodiscard]] bool separate() const { return separate_; }

    /** @brief Reports whether the Light switches between white and colour, as a TuyaMCU light
     * with a mode data point; Home then offers Tasmota's light mode button.
     */
    [[nodiscard]] bool mode_switch() const { return mode_switch_; }

    /** @brief Reports whether the Light shows colour rather than white: colour channels win,
     * and a light without white, or with white at 0, shows colour.
     */
    [[nodiscard]] bool shows_color() const;

    /** @brief Returns the number of logical power endpoints exposed by Light. */
    [[nodiscard]] std::uint8_t endpoint_count() const {
        return channel_count_ == 0U ? 0U : independent_ ? channel_count_ : separate_ ? 2U : 1U;
    }

    /** @brief Maps one channel index to its logical power endpoint. */
    [[nodiscard]] std::uint8_t endpoint_for_channel(std::size_t i) const {
        return independent_ ? static_cast<std::uint8_t>(i) : separate_ && i >= 3U ? 1U : 0U;
    }

    /** @brief Reports whether Light currently owns a PWM output index. */
    [[nodiscard]] bool owns(std::size_t index) const;

    /** @brief Reports whether any output is partway through a transition. */
    [[nodiscard]] bool transitioning() const { return transition_mask_ != 0U; }

    /** @brief Returns the number of resolved Light channels. */
    [[nodiscard]] std::uint8_t channel_count() const { return channel_count_; }

    /** @brief Returns the current persisted Light settings after initialization. */
    [[nodiscard]] const LightSettings& intent() const { return settings_->light_settings(); }

    /** @brief Returns the configured PWM output index for one Light channel. */
    [[nodiscard]] std::uint8_t channel_index(std::size_t index) const {
        return channel_indices_[index];
    }

    /** @brief Returns retained PWM configuration and status for Light outputs. */
    [[nodiscard]] const PwmOutputs& outputs() const { return outputs_; }

    /** @brief Returns the result of the most recent Light operation. */
    [[nodiscard]] platform::PwmResult last_result() const { return last_result_; }

  private:
    [[nodiscard]] platform::PwmResult retarget(std::uint32_t now, std::uint16_t selected);
    [[nodiscard]] platform::PwmResult sample(std::uint32_t now, std::uint16_t selected);
    [[nodiscard]] std::array<std::uint16_t, 16> target() const;
    [[nodiscard]] std::uint16_t endpoint_channels(std::uint8_t instance) const;
    void effect_step(std::uint32_t now);
    void correct(std::array<std::uint16_t, 16>& electrical) const;
    [[nodiscard]] bool blending() const;
    void balance(std::array<std::uint16_t, 16>& electrical) const;
#if USE_WS2812
    [[nodiscard]] led_strip::Config strip_config(const LightSettings&) const;
    [[nodiscard]] platform::PwmResult strip_sample(
        std::uint32_t now, const std::array<std::uint16_t, 16>& perceptual);
    [[nodiscard]] std::uint8_t manual_power(
        const LightSettings&, const led_strip::Pixel*, std::size_t) const;
    [[nodiscard]] platform::PwmResult write_pixels(
        std::size_t index, const led_strip::Pixel*, std::size_t count, bool repeat);
    static led_strip::Result strip_edit(void*, std::size_t, led_strip::Pixel, std::size_t);
    static void strip_fault(void*);
    bool strip_steps_complete_{};
    led_strip::Handle strip_{};
    PinId strip_pin_{};
    std::array<led_strip::Pixel, platform::led_strip::kMaximumPixels> manual_pixels_{};
    std::array<led_strip::Pixel, platform::led_strip::kMaximumPixels> strip_frame_{};
#endif
#if USE_LED_DRIVER
    led_driver::Driver led_driver_{};
#endif
    const TimeService* time_{};
    bool addressable_{};
    bool chip_{};
    bool tuya_{};        /**< A TuyaMCU light owns the channels. */
    bool mode_switch_{}; /**< The TuyaMCU light has a mode data point. */
    std::uint16_t channel_mask_{};
#if USE_POWER_PROVENANCE || USE_POWER_TARGETS
    PowerService*
        power_{}; /**< Non-owning provenance sink; initialized before Light accepts intent. */
#endif
    Settings* settings_{};
    std::array<std::uint8_t, 5> effect_channels_{};
    std::uint32_t next_effect_{};
    std::uint32_t effect_started_{};
    std::uint16_t effect_level_{65535U};
    std::uint32_t effect_random_{0x13579BDFU};
    std::uint16_t effect_wheel_{};
    std::uint8_t active_scheme_{};
    bool effect_initial_{};
    PwmOutputs outputs_{};
    PowerEndpoints endpoints_{};
    std::array<std::uint8_t, 5> channel_indices_{};
    std::array<std::uint16_t, 16> from_{};
    std::array<std::uint16_t, 16> current_{};
    std::array<std::uint16_t, 16> target_{};
    std::array<std::uint32_t, 16> started_{};
    std::array<std::uint32_t, 16> duration_{};
    std::uint16_t transition_mask_{};
    std::uint32_t revision_{};
    std::uint32_t next_sample_{};
    std::uint16_t owned_mask_{};
    std::uint8_t desired_on_{};
    std::uint8_t applied_on_{};
    std::uint8_t channel_count_{};
    bool timing_pending_{};
    bool independent_{};
    bool separate_{};
    bool mapping_valid_{};

    platform::PwmResult last_result_{platform::PwmResult::success};
};
} // namespace newmota
