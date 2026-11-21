/*
  pin_topology.h - generated-header fixture for Newmota resolver tests

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota::generated::hardware {

inline constexpr char kArchitecture[] = "test-arch";
inline constexpr std::uint8_t kNoTemplateSlot = 0xffU;
inline constexpr std::uint8_t kPinAvailable = 0x01U;
inline constexpr std::uint16_t kInput = 0x0001U;
inline constexpr std::uint16_t kOutput = 0x0002U;
inline constexpr std::uint16_t kPullUp = 0x0004U;
inline constexpr std::uint16_t kPullDown = 0x0008U;
inline constexpr std::uint16_t kOpenDrain = 0x0010U;
inline constexpr std::uint16_t kInterrupt = 0x0020U;
inline constexpr std::uint16_t kAnalog = 0x0040U;
inline constexpr std::array<std::uint16_t, 6> kTemplateToPhysical{{
    0U,
    1U,
    3U,
    4U,
    5U,
    6U,
}};

struct PinTopology {
    std::uint16_t id;
    std::uint16_t capabilities;
    std::uint8_t template_slot;
    std::uint8_t flags;
};

inline constexpr std::array<PinTopology, 7> kPins{{
    {0U, 0x000fU, 0U, kPinAvailable},
    {1U, 0x000fU, 1U, 0U},
    {2U, 0x000fU, kNoTemplateSlot, kPinAvailable},
    {3U, 0x0001U, 2U, kPinAvailable},
    {4U, 0x000fU, 3U, kPinAvailable},
    {5U, 0x000fU, 4U, kPinAvailable},
    {6U, 0x000fU, 5U, kPinAvailable},
}};

inline constexpr std::size_t kPinCount = kPins.size();
inline constexpr std::size_t kTemplateSlotCount = kTemplateToPhysical.size();

} // namespace newmota::generated::hardware
