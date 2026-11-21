/*
  math.h - Floating-point math compatibility helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once

#include <cmath>
#include <type_traits>

namespace newmota::math {

// The pinned Bouffalo libstdc++ omits these C99 names from std even though
// newlib supplies the float and double functions. Keep this gap in one place.
// Extend this API when another diagnosed target math gap needs compatibility.
// Each call must use the same float or double type; mixed types and long double
// are deliberately rejected to avoid silently changing precision.

/**
 * @brief Returns the smaller same-type float or double, preferring the number over one NaN.
 * @tparam T Must be float or double.
 * @param a First operand.
 * @param b Second operand.
 * @return The platform math library's floating-point minimum result.
 */
template <typename T> [[nodiscard]] inline T fmin(T a, T b) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
        "newmota::math::fmin supports only same-type float or double operands");
#if defined(BOUFFALO_SDK)
    if constexpr (std::is_same_v<T, float>)
        return ::fminf(a, b);
    else
        return ::fmin(a, b);
#else
    return std::fmin(a, b);
#endif
}

/**
 * @brief Returns the larger same-type float or double, preferring the number over one NaN.
 * @tparam T Must be float or double.
 * @param a First operand.
 * @param b Second operand.
 * @return The platform math library's floating-point maximum result.
 */
template <typename T> [[nodiscard]] inline T fmax(T a, T b) {
    static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
        "newmota::math::fmax supports only same-type float or double operands");
#if defined(BOUFFALO_SDK)
    if constexpr (std::is_same_v<T, float>)
        return ::fmaxf(a, b);
    else
        return ::fmax(a, b);
#else
    return std::fmax(a, b);
#endif
}

} // namespace newmota::math
