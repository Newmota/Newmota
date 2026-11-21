/*
  float_math.h - compact transcendental functions for Berry and solar timers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file float_math.h
 * @brief Transcendental functions without newlib's tables and huge-argument reduction.
 *
 * newlib links its float and double sets separately, about 43 KB on BK7231N for Berry's math
 * module and the sunrise formula. These compute in double and round once, so the float
 * functions stay within about half an ulp, no worse than newlib's float set. sqrt, floor, ceil,
 * round, fabs and fmod stay with the C library: they are small and exact there.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

float newmota_sinf(float x);
float newmota_cosf(float x);
float newmota_tanf(float x);
float newmota_asinf(float x);
float newmota_acosf(float x);
float newmota_atanf(float x);
float newmota_atan2f(float y, float x);
float newmota_sinhf(float x);
float newmota_coshf(float x);
float newmota_tanhf(float x);
float newmota_expf(float x);
float newmota_logf(float x);
float newmota_log10f(float x);
float newmota_powf(float x, float y);

#ifdef __cplusplus
}

namespace newmota::math {

// Double versions for small angles such as the sunrise formula: |x| must stay below about
// 1.6e6, beyond which sin, cos and tan return NaN instead of an unreduced result.
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);

} // namespace newmota::math
#endif
