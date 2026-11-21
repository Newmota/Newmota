/*
  basic_io.h - basic digital device driver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/hardware/hardware.h"
#include "newmota/platform/gpio.h"
#include "newmota_features.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {

struct BasicIoInitializationResult final {
    std::uint8_t relay_count{};
    std::uint8_t button_count{};
    std::uint8_t switch_count{};
    std::uint8_t led_count{};
    std::uint8_t failure_count{};
};

#ifndef NEWMOTA_IMAGE_SAFEBOOT
struct ButtonEdge final {
    std::uint8_t instance{};
    bool pressed{};
};
#endif

class BasicIoDriver final {
  public:
    static constexpr std::size_t kMaximumRelays = 32;
    static constexpr std::size_t kMaximumButtons = 32;
    static constexpr std::size_t kMaximumSwitches = 32;
    static constexpr std::size_t kMaximumLeds = 4;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    using ButtonEdgeHandler = void (*)(void* context, const ButtonEdge& edge);
#endif
#if USE_TUYA_MCU
    /** @brief Writes one relay without a pin, such as a TuyaMCU relay; true on success. */
    using ExternalRelayWriter = bool (*)(void* context, std::uint8_t instance, bool on);

    /** @brief Adds relays without a pin; they take the place of a GPIO relay of the same index,
     * as Tasmota's FUNC_SET_DEVICE_POWER drivers do. Kept across initialize().
     * @param mask One bit per one-based relay instance.
     */
    void attach_external_relays(std::uint32_t mask, ExternalRelayWriter writer, void* context) {
        external_relays_ = mask;
        external_writer_ = writer;
        external_context_ = context;
    }

    /** @brief Returns the relays without a pin, one bit per one-based instance. */
    [[nodiscard]] constexpr std::uint32_t external_relays() const { return external_relays_; }
#endif

    /** @brief Acquires resolved relay, button, switch, and LED pins in safe states. */
    [[nodiscard]] BasicIoInitializationResult initialize(const Hardware& hardware);
    /** @brief Reports whether a one-based relay instance is configured. */
    [[nodiscard]] bool has_relay(std::uint8_t instance) const;
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Reports whether a one-based button instance is configured. */
    [[nodiscard]] bool has_button(std::uint8_t instance) const;
#endif

    /** @brief Returns the number of successfully initialized relays, with relays without a pin. */
#if USE_TUYA_MCU
    [[nodiscard]] std::uint8_t relay_count() const;
#else
    [[nodiscard]] constexpr std::uint8_t relay_count() const { return relay_count_; }
#endif

    /** @brief Returns the number of successfully initialized buttons. */
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    [[nodiscard]] constexpr std::uint8_t button_count() const { return buttons_.count; }
#else
    [[nodiscard]] constexpr std::uint8_t button_count() const { return 0U; }
#endif

    /** @brief Returns the number of successfully initialized switches, with switches without a
     * pin that may still appear.
     */
#if USE_SWITCHES && USE_TUYA_MCU
    [[nodiscard]] std::uint8_t switch_count() const;
#elif USE_SWITCHES
    [[nodiscard]] constexpr std::uint8_t switch_count() const { return switches_.count; }
#else
    [[nodiscard]] constexpr std::uint8_t switch_count() const { return 0U; }
#endif

    /** @brief Returns the number of successfully initialized LEDs. */
    [[nodiscard]] constexpr std::uint8_t led_count() const { return led_count_; }

    /** @brief Writes one logical relay state through its inversion mapping. */
    [[nodiscard]] platform::GpioResult write_relay(std::uint8_t instance, bool on);
    /** @brief Writes one logical LED state through its inversion mapping. */
    [[nodiscard]] platform::GpioResult write_led(std::uint8_t instance, bool on);
    /** @brief Reports whether a one-based LED instance is configured. */
    [[nodiscard]] bool has_led(std::uint8_t instance) const;

    /** @brief Returns a one-based LED's pin, or an invalid pin. */
    [[nodiscard]] PinId led_pin(std::uint8_t instance) const {
        return has_led(instance) ? led_pins_[instance - 1U] : PinId{};
    }

    /** @brief Reports whether a one-based LED is active low. */
    [[nodiscard]] bool led_inverted(std::uint8_t instance) const {
        return has_led(instance) && (led_inverted_ & (1U << (instance - 1U))) != 0U;
    }

    /** @brief Reports whether a dedicated network link LED is configured. */
    [[nodiscard]] bool has_link_led() const { return link_led_pin_.valid(); }

    /** @brief Writes the link LED through its inversion mapping. */
    [[nodiscard]] platform::GpioResult write_link_led(bool on);
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Advances the Tasmota logical HIGH/LOW integrator once per 10 ms, without catch-up.
     * @param now Rollover-safe monotonic milliseconds.
     * @param debounce Validated ButtonDebounce value, 40..1000 excluding AC mode (units digit 9).
     * Changes discard partial filtering and seed counters from the last delivered states.
     */
    void probe_buttons(std::uint32_t now, std::uint16_t debounce);
    /** @brief Emits changes in filtered button states at the selected handler cadence. */
    void poll_buttons(ButtonEdgeHandler handler, void* context);
    /** @brief Returns the last delivered filtered logical state of one button. */
    [[nodiscard]] bool button_pressed(std::uint8_t instance) const;
#endif
#if USE_SWITCHES
    /** @brief Reports whether a one-based switch instance is configured. */
    [[nodiscard]] bool has_switch(std::uint8_t instance) const;
    /** @brief Returns the configured switches, one bit per instance, with switches without a pin
     * that have not reported yet.
     */
    [[nodiscard]] std::uint32_t configured_switches() const;
    /** @brief Advances the switch filter like probe_buttons, with SwitchDebounce. */
    void probe_switches(std::uint32_t now, std::uint16_t debounce);

    /** @brief Returns the filtered switch levels, one bit per switch, set while HIGH. */
#if USE_TUYA_MCU
    [[nodiscard]] std::uint32_t switch_levels() const {
        return (~switches_.filtered_pressed & ~external_switches_) |
               (external_levels_ & external_switches_);
    }

    /** @brief Adds switches without a pin, such as TuyaMCU switches, before initialize(). As
     * Tasmota's SwitchSetState, one exists from its first report on, starts HIGH (not pressed)
     * and takes the reported level without filtering. Kept across initialize().
     * @param mask One bit per one-based switch instance.
     */
    void attach_external_switches(std::uint32_t mask) {
        external_switches_ = mask;
        external_seen_ = 0U;
        external_levels_ = mask;
    }

    /** @brief Sets the level of a switch without a pin; ignored for one not attached. */
    void set_external_switch(std::uint8_t instance, bool high);
#else
    [[nodiscard]] std::uint32_t switch_levels() const { return ~switches_.filtered_pressed; }
#endif
#endif

  private:
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    /** @brief Pins and filter state of one group of polled inputs; pressed means logical LOW. */
    struct InputBank final {
        std::array<PinId, kMaximumButtons> pins{};
        std::uint32_t inverted{};
        std::uint32_t pressed{};
        std::uint32_t filtered_pressed{};
        std::uint32_t probe_at{};
        std::array<std::uint8_t, kMaximumButtons> filters{};
        std::uint16_t debounce{};
        std::uint8_t count{};
    };

    /** @brief Configures one input pin and records its initial logical level. */
    [[nodiscard]] static platform::GpioResult add_input(
        InputBank& bank, const PinBinding& binding, std::size_t channel);
    /** @brief Runs Tasmota's integrating filter over one bank. */
    static void probe(InputBank& bank, std::uint32_t now, std::uint16_t debounce);
#endif
    std::array<PinId, kMaximumRelays> relay_pins_{};
    std::array<PinId, kMaximumLeds> led_pins_{};
    PinId link_led_pin_{};
    bool link_led_inverted_{};
    std::uint32_t relay_inverted_{};
#ifndef NEWMOTA_IMAGE_SAFEBOOT
    InputBank buttons_{};
#endif
#if USE_SWITCHES
    InputBank switches_{};
#endif
#if USE_TUYA_MCU && USE_SWITCHES
    std::uint32_t external_switches_{};
    std::uint32_t external_seen_{};
    std::uint32_t external_levels_{};
#endif
#if USE_TUYA_MCU
    std::uint32_t external_relays_{};
    ExternalRelayWriter external_writer_{};
    void* external_context_{};
#endif
    std::uint8_t led_inverted_{};
    std::uint8_t relay_count_{};
    std::uint8_t led_count_{};
};

} // namespace newmota
