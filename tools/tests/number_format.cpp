/*
  number_format.cpp - Host checks for 64-bit integer text without printf's %ll

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file number_format.cpp
 * @brief Checks support::decimal() and support::digits() (text and size) against the %llu, %lld,
 * %0*llu and %0*llX output of the host C library, which they replace on newlib-nano (Beken).
 */
#include "support/number.h"
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
using namespace newmota::support;
int failures{};

void expect(const IntegerText& got_text, const char* want, const char* what) {
    const char* got = got_text.text;
    if (std::strcmp(got, want) != 0 || got_text.size != std::strlen(want)) {
        std::fprintf(stderr, "number format: %s: got \"%s\", want \"%s\"\n", what, got, want);
        ++failures;
    }
}
} // namespace

int main() {
    const std::uint64_t unsigned_values[]{
        0U, 7U, 4294967295U, 4294967296U, 1234567890123ULL, UINT64_MAX};
    const std::int64_t signed_values[]{0, -1, 42, INT32_MIN, -4294967296LL, INT64_MIN, INT64_MAX};
    char want[40];
    for (const auto value : unsigned_values) {
        std::snprintf(want, sizeof want, "%" PRIu64, value);
        expect(decimal(value), want, "decimal unsigned");
        for (const unsigned width : {0U, 1U, 6U, 16U, 20U}) {
            std::snprintf(want, sizeof want, "%0*" PRIu64, int(width), value);
            expect(digits(value, 10, width), want, "digits base 10");
            std::snprintf(want, sizeof want, "%0*" PRIX64, int(width), value);
            expect(digits(value, 16, width), want, "digits base 16");
        }
    }
    for (const auto value : signed_values) {
        std::snprintf(want, sizeof want, "%" PRId64, value);
        expect(decimal(value), want, "decimal signed");
    }
    // Widths above 20 are capped, so the text always fits.
    expect(digits(5U, 10, 30U), "00000000000000000005", "digits width cap");
    return failures == 0 ? 0 : 1;
}
