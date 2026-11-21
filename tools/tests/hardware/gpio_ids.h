/*
  gpio_ids.h - generated-header fixture for Newmota resolver tests

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include <array>
#include <cstdint>

namespace newmota::generated::gpio {

inline constexpr std::uint8_t kEncodingShift = 5U;
inline constexpr std::uint16_t kSensorEnd = 389U;
inline constexpr std::uint16_t kTemplateUserValue = 1U;
inline constexpr std::uint8_t kRoleNone = 0U;
inline constexpr std::uint8_t kRoleInput = 1U;
inline constexpr std::uint8_t kRoleFixedOutput = 2U;
inline constexpr std::uint8_t kRoleButton = 3U;
inline constexpr std::uint8_t kRoleRelay = 4U;
inline constexpr std::uint8_t kRoleStatusLed = 5U;
inline constexpr std::uint8_t kRoleDht11 = 10U;
inline constexpr std::uint8_t kRoleDht22 = 11U;
inline constexpr std::uint8_t kRoleSi7021 = 12U;
inline constexpr std::uint8_t kRoleLinkLed = 18U;
inline constexpr std::uint8_t kRoleSwitch = 32U;
inline constexpr std::uint8_t kRoleBuzzer = 33U;
inline constexpr std::uint8_t kModifierInverted = 0x01U;
inline constexpr std::uint8_t kModifierNoPull = 0x02U;
inline constexpr std::uint8_t kModifierPullDown = 0x04U;
inline constexpr std::uint8_t kModifierBistable = 0x08U;
inline constexpr std::uint8_t kModifierFixed = 0x10U;
inline constexpr std::uint8_t kModifierInitialHigh = 0x20U;
inline constexpr std::uint8_t kModifierInitialLow = 0x40U;

struct FunctionDescriptor {
    std::uint16_t first;
    std::uint16_t requirements;
    std::uint8_t instances;
    std::uint8_t role;
    std::uint8_t modifiers;
};

struct ModuleAssignment {
    std::uint16_t function;
    std::uint8_t slot;
};

struct ModuleDescriptor {
    std::uint32_t flag;
    std::uint16_t assignment_offset;
    std::uint8_t number;
    std::uint8_t assignment_count;
    std::uint8_t base;
};

inline constexpr std::uint16_t kOutputHigh = 3840U;
inline constexpr std::uint16_t kOutputLow = 3872U;
inline constexpr std::uint16_t kInput = 7648U;
inline constexpr std::uint16_t kButton = 32U;
inline constexpr std::uint16_t kButtonNoPull = 64U;
inline constexpr std::uint16_t kButtonInverted = 96U;
inline constexpr std::uint16_t kButtonInvertedNoPull = 128U;
inline constexpr std::uint16_t kRelay = 224U;
inline constexpr std::uint16_t kRelayInverted = 256U;
inline constexpr std::uint16_t kLed = 288U;
inline constexpr std::uint16_t kLedInverted = 320U;
inline constexpr std::uint16_t kButtonPullDown = 7680U;
inline constexpr std::uint16_t kButtonInvertedPullDown = 7712U;

#if USE_PWM
inline constexpr std::array<FunctionDescriptor, 16> kFunctions {
    {
#else
inline constexpr std::array<FunctionDescriptor, 14> kFunctions{{
#endif
        {0U, 0U, 1U, 0U, 0U}, {kButton, 0x0005U, 32U, 3U, 0U},
            {kButtonNoPull, 0x0001U, 32U, 3U, 0x02U}, {kButtonInverted, 0x0005U, 32U, 3U, 0x01U},
            {kButtonInvertedNoPull, 0x0001U, 32U, 3U, 0x03U}, {kRelay, 0x0002U, 32U, 4U, 0U},
            {kRelayInverted, 0x0002U, 32U, 4U, 0x01U}, {kLed, 0x0002U, 4U, 5U, 0U},
            {kLedInverted, 0x0002U, 4U, 5U, 0x01U},
#if USE_PWM
            {416U, 0x0002U, 16U, 6U, 0U}, {448U, 0x0002U, 16U, 6U, 1U},
#endif
            {kOutputHigh, 0x0002U, 1U, 2U, 0x30U}, {kOutputLow, 0x0002U, 1U, 2U, 0x50U},
            {kInput, 0x0001U, 32U, 1U, 0U}, {kButtonPullDown, 0x0009U, 32U, 3U, 0x04U},
            {kButtonInvertedPullDown, 0x0009U, 32U, 3U, 0x05U},
    }
};

inline constexpr std::array<ModuleAssignment, 4> kModuleAssignments{{
    {kTemplateUserValue, 0U},
    {kInput, 1U},
    {kTemplateUserValue, 0U},
    {kInput, 2U},
}};

inline constexpr std::array<ModuleDescriptor, 2> kModules{{
    {0U, 0U, 1U, 2U, 0U},
    {0U, 2U, 2U, 2U, 0U},
}};

} // namespace newmota::generated::gpio
