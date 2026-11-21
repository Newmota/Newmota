/*
  hardware_names.h - generated-name fixture for Newmota command tests

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

inline constexpr char kFunctionNames[] =
    "None\0Button\0Button_n\0Button_i\0Button_in\0Relay\0Relay_i\0"
    "Led\0Led_i\0Output Hi\0Output Low\0Input\0Button_pd\0Button_ipd";
inline constexpr std::array<std::uint16_t, 14> kFunctionNameOffsets{{
    0U,
    5U,
    12U,
    21U,
    30U,
    40U,
    46U,
    54U,
    58U,
    64U,
    74U,
    85U,
    91U,
    101U,
}};
inline constexpr std::array<char, 23> kModuleNames{{
    'T',
    'e',
    's',
    't',
    ' ',
    'B',
    'o',
    'a',
    'r',
    'd',
    '\0',
    'V',
    'a',
    'l',
    'i',
    'd',
    ' ',
    'B',
    'o',
    'a',
    'r',
    'd',
    '\0',
}};
inline constexpr std::array<std::uint16_t, 2> kModuleNameOffsets{{0U, 11U}};

} // namespace newmota::generated::gpio
