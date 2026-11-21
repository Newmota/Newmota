/*
  float_math.cpp - compact transcendental functions for Berry and solar timers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file float_math.cpp
 * @brief Double-precision kernels behind the float functions in newmota/float_math.h.
 *
 * Kernel coefficients and the medium argument reduction follow fdlibm (Sun Microsystems,
 * freely redistributable). Every float function evaluates in double and rounds once.
 */
#include "newmota/float_math.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979311600e+00;
constexpr double kPio2 = 1.57079632679489655800e+00;
constexpr double kPio4 = 7.85398163397448278999e-01;
constexpr double kInvPio2 = 6.36619772367581382433e-01;
constexpr double kLn2Hi = 6.93147180369123816490e-01; // Trailing zero bits keep k * kLn2Hi exact.
constexpr double kLn2Lo = 1.90821492927058770002e-10;
constexpr double kInvLn2 = 1.44269504088896338700e+00;
constexpr double kInvLn10 = 4.34294481903251827651e-01;
constexpr double kSqrt2 = 1.41421356237309504880e+00;
// Medium reduction is exact while n * kPio2Part1 fits 53 bits, so for |n| below 2^20.
constexpr double kMediumLimit = 1.6e6;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double from_bits(const std::uint64_t value) {
    double result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

std::uint64_t to_bits(const double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

double magnitude(const double value) {
    return from_bits(to_bits(value) & ~(1ULL << 63));
}

double with_sign(const double value, const double sign) {
    return from_bits((to_bits(value) & ~(1ULL << 63)) | (to_bits(sign) & (1ULL << 63)));
}

bool negative(const double value) {
    return (to_bits(value) >> 63) != 0U;
}

double nearest(const double value) {
    return static_cast<double>(static_cast<std::int64_t>(value + (value < 0 ? -0.5 : 0.5)));
}

/** @brief sin on |x| <= pi/4. */
double kernel_sin(const double x) {
    const double z = x * x;
    return x + x * z *
                   (-1.66666666666666324348e-01 +
                       z * (8.33333333332248946124e-03 +
                               z * (-1.98412698298579493134e-04 +
                                       z * (2.75573137070700676789e-06 +
                                               z * (-2.50507602534068634195e-08 +
                                                       z * 1.58969099521155010221e-10)))));
}

/** @brief cos on |x| <= pi/4. */
double kernel_cos(const double x) {
    const double z = x * x;
    return 1.0 - 0.5 * z +
           z * z *
               (4.16666666666666019037e-02 +
                   z * (-1.38888888888741095749e-03 +
                           z * (2.48015872894767294178e-05 +
                                   z * (-2.75573143513906633035e-07 +
                                           z * (2.08757232129817482790e-09 +
                                                   z * -1.13596475577881948265e-11)))));
}

/** @brief Reduces |x| < kMediumLimit to r in [-pi/4, pi/4] in three Cody-Waite steps. */
double reduce_medium(const double x, int& quadrant) {
    const double n = nearest(x * kInvPio2);
    quadrant = static_cast<int>(n);
    double r = x - n * 1.57079632673412561417e+00;
    double t = r;
    double w = n * 6.07710050630396597660e-11;
    r = t - w;
    t = r;
    w = n * 2.02226624871116645580e-21;
    r = t - w;
    w = n * 8.47842766036889956997e-32 - ((t - r) - w);
    return r - w;
}

// 2/pi in 32-bit words, most significant first: enough bits for the largest float.
constexpr std::uint32_t kTwoOverPi[] = {0xA2F9836EU, 0x4E441529U, 0xFC2757D1U, 0xF534DDC0U,
    0xDB629599U, 0x3C439041U, 0xFE5163ABU, 0xDEBBC561U, 0xB7246E3AU};

/** @brief Payne-Hanek reduction of a float at least kMediumLimit, exact to about 2^-64. */
double reduce_large(const float x, int& quadrant) {
    std::uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const std::uint64_t mantissa = (bits & 0x7fffffU) | 0x800000U;
    const int scale = static_cast<int>((bits >> 23) & 0xffU) - 150; // |x| = mantissa * 2^scale
    // Bits of 2/pi weighing 4 or more in mantissa * 2^scale * 2/pi only add multiples of 4.
    const int first = scale - 1 < 1 ? 1 : scale - 1;
    const int position = first - 1;
    const int word = position >> 5, shift = position & 31;
    std::uint64_t window[3];
    for (int index = 0; index < 3; ++index) {
        const std::uint32_t high = kTwoOverPi[word + index] << shift;
        const std::uint32_t low = shift == 0 ? 0U : kTwoOverPi[word + index + 1] >> (32 - shift);
        window[index] = high | low;
    }
    // product = mantissa * window, 120 bits; its binary point sits fraction_bits from the right.
    const std::uint64_t p2 = mantissa * window[2], p1 = mantissa * window[1];
    const std::uint64_t p0 = mantissa * window[0];
    const std::uint64_t low = p2 + (p1 << 32);
    const std::uint64_t high = p0 + (p1 >> 32) + (low < p2 ? 1U : 0U);
    const int fraction_shift = 95 + first - scale - 64;
    std::uint64_t fraction = (low >> fraction_shift) | (high << (64 - fraction_shift));
    quadrant = static_cast<int>((high >> fraction_shift) & 3U);
    double turn;
    if ((fraction >> 63) != 0U) {
        // Above one half: step to the next quadrant without losing the small remainder.
        fraction = ~fraction + 1U;
        turn = -static_cast<double>(fraction) * 0x1p-64;
        quadrant = (quadrant + 1) & 3;
    } else {
        turn = static_cast<double>(fraction) * 0x1p-64;
    }
    return turn * kPio2;
}

/** @brief Reduces a finite float; large magnitudes take the exact path. */
double reduce_float(const float x, int& quadrant) {
    const double value = x;
    if (magnitude(value) <= kPio4) {
        quadrant = 0;
        return value;
    }
    if (magnitude(value) < kMediumLimit)
        return reduce_medium(value, quadrant);
    const double r = reduce_large(x < 0 ? -x : x, quadrant);
    if (x >= 0)
        return r;
    quadrant = -quadrant;
    return -r;
}

double sin_reduced(const double r, const int quadrant) {
    switch (quadrant & 3) {
    case 0:
        return kernel_sin(r);
    case 1:
        return kernel_cos(r);
    case 2:
        return -kernel_sin(r);
    default:
        return -kernel_cos(r);
    }
}

double cos_reduced(const double r, const int quadrant) {
    return sin_reduced(r, quadrant + 1);
}

double tan_reduced(const double r, const int quadrant) {
    return (quadrant & 1) == 0 ? kernel_sin(r) / kernel_cos(r) : -kernel_cos(r) / kernel_sin(r);
}

double atan_d(const double value) {
    constexpr double kHigh[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
        9.82793723247329054082e-01, 1.57079632679489655800e+00};
    constexpr double kLow[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
        1.39033110312309984516e-17, 6.12323399573676603587e-17};
    if (value != value)
        return value;
    double x = magnitude(value);
    if (x >= 0x1p66)
        return with_sign(kPio2, value);
    int id = -1;
    if (x < 0.4375) {
        if (x < 0x1p-29)
            return value; // Also keeps the sign of zero.
        x = value;
    } else if (x < 1.1875) {
        if (x < 0.6875) {
            id = 0;
            x = (2.0 * x - 1.0) / (2.0 + x);
        } else {
            id = 1;
            x = (x - 1.0) / (x + 1.0);
        }
    } else if (x < 2.4375) {
        id = 2;
        x = (x - 1.5) / (1.0 + 1.5 * x);
    } else {
        id = 3;
        x = -1.0 / x;
    }
    const double z = x * x, w = z * z;
    const double s1 = z * (3.33333333333329318027e-01 +
                              w * (1.42857142725034663711e-01 +
                                      w * (9.09088713343650656196e-02 +
                                              w * (6.66107313738753120669e-02 +
                                                      w * (4.97687799461593236017e-02 +
                                                              w * 1.62858201153657823623e-02)))));
    const double s2 =
        w *
        (-1.99999999998764832476e-01 +
            w * (-1.11111104054623557880e-01 +
                    w * (-7.69187620504482999495e-02 +
                            w * (-5.83357013379057348645e-02 + w * -3.65315727442169155270e-02))));
    if (id < 0)
        return x - x * (s1 + s2);
    return with_sign(kHigh[id] - ((x * (s1 + s2) - kLow[id]) - x), value);
}

double atan2_d(const double y, const double x) {
    if (x != x || y != y)
        return x + y;
    if (y == 0)
        return negative(x) ? with_sign(kPi, y) : y;
    if (x == 0)
        return with_sign(kPio2, y);
    if (x == kInfinity || x == -kInfinity) {
        if (y == kInfinity || y == -kInfinity)
            return with_sign(x > 0 ? kPio4 : 3.0 * kPio4, y);
        return with_sign(x > 0 ? 0.0 : kPi, y);
    }
    if (y == kInfinity || y == -kInfinity)
        return with_sign(kPio2, y);
    const double angle = atan_d(magnitude(y / x));
    return with_sign(x < 0 ? kPi - angle : angle, y);
}

/** @brief Square root of v >= 0 from the exact float root and two Newton steps. */
double sqrt_d(const double v) {
    if (v == 0)
        return v;
    if (v < 1e-30)
        return sqrt_d(v * 0x1p100) * 0x1p-50;
    double g = std::sqrt(static_cast<float>(v));
    g = 0.5 * (g + v / g);
    return 0.5 * (g + v / g);
}

/** @brief exp for any double; overflows to infinity and underflows to zero. */
double exp_d(const double x) {
    if (x != x)
        return x;
    if (x > 709.8)
        return kInfinity;
    if (x < -745.2)
        return 0.0;
    const double k = nearest(x * kInvLn2);
    const double r = (x - k * kLn2Hi) - k * kLn2Lo;
    // Taylor series to r^13 / 13!: below 1e-17 relative for |r| <= ln2 / 2.
    double p = 1.0 / 6227020800.0;
    for (const double coefficient :
        {1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0, 1.0 / 362880.0, 1.0 / 40320.0,
            1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0, 1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0, 1.0})
        p = p * r + coefficient;
    auto exponent = static_cast<int>(k);
    // Scale in two steps at both ends of the exponent range.
    if (exponent > 1023) {
        p *= 2.0;
        --exponent;
    } else if (exponent < -1000) {
        p *= 0x1p-200;
        exponent += 200;
    }
    return p * from_bits(static_cast<std::uint64_t>(exponent + 1023) << 52);
}

/** @brief Natural logarithm of a positive, finite, normal double. */
double log_d(const double x) {
    const auto bits = to_bits(x);
    int exponent = static_cast<int>((bits >> 52) & 0x7ffU) - 1023;
    double m = from_bits((bits & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL);
    if (m > kSqrt2) {
        m *= 0.5;
        ++exponent;
    }
    // log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.172; the series ends below 1e-17.
    const double s = (m - 1.0) / (m + 1.0), z = s * s;
    double series = 1.0 / 23.0;
    for (const double coefficient : {1.0 / 21.0, 1.0 / 19.0, 1.0 / 17.0, 1.0 / 15.0, 1.0 / 13.0,
             1.0 / 11.0, 1.0 / 9.0, 1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0})
        series = series * z + coefficient;
    return exponent * kLn2Hi + (exponent * kLn2Lo + 2.0 * s * series);
}

/** @brief log for a float, with the C library's results for zero, negatives and specials. */
double log_float(const float x) {
    if (x != x || x < 0)
        return kNaN;
    if (x == 0)
        return -kInfinity;
    if (x == std::numeric_limits<float>::infinity())
        return kInfinity;
    return log_d(x);
}

/** @brief Classifies a finite float as an integer and as odd. */
void integer_kind(const float y, bool& integer, bool& odd) {
    const double value = magnitude(y);
    if (value >= 0x1p24) {
        integer = true;
        odd = false;
        return;
    }
    const auto whole = static_cast<std::int64_t>(value);
    integer = static_cast<double>(whole) == value;
    odd = integer && (whole & 1) != 0;
}

} // namespace

extern "C" {

float newmota_sinf(const float x) {
    if (x != x || x == std::numeric_limits<float>::infinity() ||
        x == -std::numeric_limits<float>::infinity())
        return x - x;
    int quadrant;
    const double r = reduce_float(x, quadrant);
    return static_cast<float>(sin_reduced(r, quadrant));
}

float newmota_cosf(const float x) {
    if (x != x || x == std::numeric_limits<float>::infinity() ||
        x == -std::numeric_limits<float>::infinity())
        return x - x;
    int quadrant;
    const double r = reduce_float(x, quadrant);
    return static_cast<float>(cos_reduced(r, quadrant));
}

float newmota_tanf(const float x) {
    if (x != x || x == std::numeric_limits<float>::infinity() ||
        x == -std::numeric_limits<float>::infinity())
        return x - x;
    int quadrant;
    const double r = reduce_float(x, quadrant);
    return static_cast<float>(tan_reduced(r, quadrant));
}

float newmota_asinf(const float x) {
    const double value = x;
    if (!(magnitude(value) <= 1.0))
        return static_cast<float>(kNaN);
    return static_cast<float>(atan2_d(value, sqrt_d((1.0 - value) * (1.0 + value))));
}

float newmota_acosf(const float x) {
    const double value = x;
    if (!(magnitude(value) <= 1.0))
        return static_cast<float>(kNaN);
    return static_cast<float>(atan2_d(sqrt_d((1.0 - value) * (1.0 + value)), value));
}

float newmota_atanf(const float x) {
    return static_cast<float>(atan_d(x));
}

float newmota_atan2f(const float y, const float x) {
    return static_cast<float>(atan2_d(y, x));
}

float newmota_sinhf(const float x) {
    const double value = x, a = magnitude(value);
    if (a < 0.125) {
        const double z = value * value;
        return static_cast<float>(
            value +
            value * z * (1.0 / 6.0 + z * (1.0 / 120.0 + z * (1.0 / 5040.0 + z / 362880.0))));
    }
    const double e = exp_d(a);
    return static_cast<float>(with_sign(0.5 * (e - 1.0 / e), value));
}

float newmota_coshf(const float x) {
    const double e = exp_d(magnitude(x));
    return static_cast<float>(0.5 * (e + 1.0 / e));
}

float newmota_tanhf(const float x) {
    const double value = x, a = magnitude(value);
    if (value != value)
        return x;
    if (a < 0.125) {
        const double z = value * value;
        return static_cast<float>(
            value -
            value * z *
                (1.0 / 3.0 -
                    z * (2.0 / 15.0 -
                            z * (17.0 / 315.0 - z * (62.0 / 2835.0 - z * (1382.0 / 155925.0))))));
    }
    if (a > 20.0)
        return static_cast<float>(with_sign(1.0, value));
    const double e = exp_d(2.0 * a);
    return static_cast<float>(with_sign((e - 1.0) / (e + 1.0), value));
}

float newmota_expf(const float x) {
    return static_cast<float>(exp_d(x));
}

float newmota_logf(const float x) {
    return static_cast<float>(log_float(x));
}

float newmota_log10f(const float x) {
    return static_cast<float>(log_float(x) * kInvLn10);
}

float newmota_powf(const float x, const float y) {
    constexpr float kFloatInfinity = std::numeric_limits<float>::infinity();
    if (y == 0 || x == 1)
        return 1.0F;
    if (x != x || y != y)
        return x + y;
    if (y == kFloatInfinity || y == -kFloatInfinity) {
        const double a = magnitude(x);
        if (a == 1.0)
            return 1.0F;
        return (a > 1.0) == (y > 0) ? kFloatInfinity : 0.0F;
    }
    bool integer, odd;
    integer_kind(y, integer, odd);
    if (x == 0) {
        if (y < 0)
            return odd ? static_cast<float>(with_sign(kInfinity, x)) : kFloatInfinity;
        return odd ? x : 0.0F;
    }
    if (x == kFloatInfinity)
        return y < 0 ? 0.0F : kFloatInfinity;
    if (x == -kFloatInfinity) {
        if (y < 0)
            return odd ? -0.0F : 0.0F;
        return odd ? -kFloatInfinity : kFloatInfinity;
    }
    if (x < 0 && !integer)
        return static_cast<float>(kNaN);
    const double result = exp_d(static_cast<double>(y) * log_d(magnitude(x)));
    return static_cast<float>(x < 0 && odd ? -result : result);
}

} // extern "C"

namespace newmota::math {

double sin(const double x) {
    if (!(magnitude(x) < kMediumLimit))
        return kNaN;
    int quadrant = 0;
    const double r = magnitude(x) <= kPio4 ? x : reduce_medium(x, quadrant);
    return sin_reduced(r, quadrant);
}

double cos(const double x) {
    if (!(magnitude(x) < kMediumLimit))
        return kNaN;
    int quadrant = 0;
    const double r = magnitude(x) <= kPio4 ? x : reduce_medium(x, quadrant);
    return cos_reduced(r, quadrant);
}

double tan(const double x) {
    if (!(magnitude(x) < kMediumLimit))
        return kNaN;
    int quadrant = 0;
    const double r = magnitude(x) <= kPio4 ? x : reduce_medium(x, quadrant);
    return tan_reduced(r, quadrant);
}

double asin(const double x) {
    if (!(magnitude(x) <= 1.0))
        return kNaN;
    return atan2_d(x, sqrt_d((1.0 - x) * (1.0 + x)));
}

double acos(const double x) {
    if (!(magnitude(x) <= 1.0))
        return kNaN;
    return atan2_d(sqrt_d((1.0 - x) * (1.0 + x)), x);
}

double atan(const double x) {
    return atan_d(x);
}

} // namespace newmota::math
