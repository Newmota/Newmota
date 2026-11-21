/*
  button_event.h - portable application button events

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file button_event.h
 * @brief Typed button gestures, switch keys and their MQTT routing, independent of Rules.
 */
#pragma once
#include <cstdint>

namespace newmota {
enum class ButtonAction : std::uint8_t {
    single = 1,
    double_press,
    triple,
    quad,
    penta,
    hold,
    clear
};

/** @brief One completed gesture; key instance/state retain Tasmota SendKey identities. */
struct ButtonEvent final {
    std::uint8_t instance{};
    std::uint8_t key_instance{};
    std::uint8_t state{};
    ButtonAction action{ButtonAction::single};
    bool decoupled{};
    bool send_key{true};
    bool observe{true}; /**< False only for a second ButtonTopic key route of the same gesture. */
    bool immediate{};   /**< SetOption13 press: no multi-press sequence, as in Tasmota. */
};

/** @brief One Tasmota switch SendKey or SetOption114 Action.
 * state is Tasmota's POWER_* code: 0 OFF, 1 ON, 2 TOGGLE, 3 HOLD, 4 INCREMENT, 5 INV, 6 CLEAR,
 * 7 RELEASE, 8 100, 10 DELAYED.
 */
enum class SwitchEventKind : std::uint8_t {
    key,    /**< SendKey: SwitchTopic or local Power. */
    action, /**< SetOption114 Action publication. */
    report, /**< SwitchMode 15/16 change: publish SENSOR now. */
};

struct SwitchEvent final {
    std::uint8_t instance{};
    std::uint8_t state{};
    SwitchEventKind kind{SwitchEventKind::key};
};

/** @brief Returns the Tasmota Action word for a typed gesture. */
[[nodiscard]] constexpr const char* button_action_name(ButtonAction action) {
    switch (action) {
    case ButtonAction::single:
        return "SINGLE";
    case ButtonAction::double_press:
        return "DOUBLE";
    case ButtonAction::triple:
        return "TRIPLE";
    case ButtonAction::quad:
        return "QUAD";
    case ButtonAction::penta:
        return "PENTA";
    case ButtonAction::hold:
        return "HOLD";
    case ButtonAction::clear:
        return "CLEAR";
    }
    return "CLEAR";
}
} // namespace newmota
