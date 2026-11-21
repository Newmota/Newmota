/*
  cron.cpp - bounded portable civil-time cron patterns

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file cron.cpp
 * @brief Bounded portable civil-time cron patterns.
 */

#include "services/berry/modules/newmota/cron.h"

#include "services/time/time_service.h"

namespace newmota {
namespace {
constexpr unsigned mins[] = {0, 0, 0, 1, 1, 0}, maxs[] = {59, 59, 23, 31, 12, 7};

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool value(std::string_view s, std::size_t& pos, unsigned field, unsigned& n) {
    n = 0;
    auto start = pos;
    if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            n = n * 10 + static_cast<unsigned>(s[pos++] - '0');
            if (n > maxs[field])
                return false;
        }
        return n >= mins[field];
    }
    if ((field != 4 && field != 5) || s.size() - pos < 3)
        return false;
    constexpr const char *month = "janfebmaraprmayjunjulaugsepoctnovdec",
                         *day = "sunmontuewedthufrisat";
    auto names = field == 4 ? month : day;
    unsigned count = field == 4 ? 12 : 7;
    for (unsigned i = 0; i < count; ++i)
        if (lower(s[pos]) == names[i * 3] && lower(s[pos + 1]) == names[i * 3 + 1] &&
            lower(s[pos + 2]) == names[i * 3 + 2]) {
            pos += 3;
            n = i + (field == 4);
            return true;
        }
    pos = start;
    return false;
}

bool field(std::string_view s, unsigned f, std::uint64_t& bits) {
    if (s.empty())
        return false;
    std::size_t p = 0;
    bits = 0;
    while (p < s.size()) {
        unsigned lo, hi, step = 1;
        bool range = false;
        bool wildcard = s[p] == '*' || s[p] == '?';
        if (s[p] == '?' && f != 3 && f != 5)
            return false;
        if (wildcard) {
            ++p;
            lo = mins[f];
            hi = maxs[f];
        } else {
            if (!value(s, p, f, lo))
                return false;
            hi = lo;
        }
        if (p < s.size() && s[p] == '-') {
            range = true;
            if (wildcard)
                return false;
            ++p;
            if (!value(s, p, f, hi) || hi < lo)
                return false;
        }
        if (p < s.size() && s[p] == '/') {
            ++p;
            auto start = p;
            step = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
                step = step * 10 + static_cast<unsigned>(s[p++] - '0');
                if (step > maxs[f])
                    return false;
            }
            if (p == start || !step)
                return false;
            if (!wildcard && !range)
                hi = maxs[f];
        }
        for (unsigned i = lo; i <= hi; i += step)
            bits |= 1ULL << (f == 5 && i == 7 ? 0 : i);
        if (p == s.size())
            return true;
        if (s[p++] != ',' || p == s.size())
            return false;
    }
    return false;
}

bool contains(const CronPattern& p, unsigned f, unsigned value) {
    return f < 2 ? (p.seconds_minutes[f] & (1ULL << value)) != 0
                 : (p.hours_days_months_weekdays[f - 2] & (1UL << value)) != 0;
}

/** @brief Advances to the next multiple of unit, failing past the 32-bit epoch range. */
bool advance(std::uint32_t& candidate, std::uint32_t unit) {
    const auto start = candidate - candidate % unit;
    if (start > UINT32_MAX - unit)
        return false;
    candidate = start + unit;
    return true;
}
} // namespace

bool parse_cron(std::string_view expression, CronPattern& pattern) {
    if (expression.empty() || expression.size() > 128 || expression.find('\0') != expression.npos)
        return false;
    CronPattern result;
    std::size_t p = 0;
    for (unsigned f = 0; f < 6; ++f) {
        while (p < expression.size() && whitespace(expression[p]))
            ++p;
        auto start = p;
        while (p < expression.size() && !whitespace(expression[p]))
            ++p;
        std::uint64_t bits = 0;
        if (!field(expression.substr(start, p - start), f, bits))
            return false;
        if (f < 2)
            result.seconds_minutes[f] = bits;
        else
            result.hours_days_months_weekdays[f - 2] = static_cast<std::uint32_t>(bits);
    }
    while (p < expression.size() && whitespace(expression[p]))
        ++p;
    if (p != expression.size())
        return false;
    pattern = result;
    return true;
}

bool next_cron_after(const CronPattern& p, std::uint32_t epoch, std::uint32_t& next) {
    if (epoch == UINT32_MAX)
        return false;
    std::uint32_t candidate = epoch + 1;
    for (;;) {
        auto c = TimeService::civil_time(candidate);
        if (!contains(p, 4, c.month) || !contains(p, 3, c.day) ||
            !contains(p, 5, c.day_of_week - 1)) {
            if (!advance(candidate, 86400))
                return false;
            continue;
        }
        if (!contains(p, 2, c.hour)) {
            if (!advance(candidate, 3600))
                return false;
            continue;
        }
        if (!contains(p, 1, c.minute)) {
            if (!advance(candidate, 60))
                return false;
            continue;
        }
        unsigned second = c.second;
        while (second < 60 && !contains(p, 0, second))
            ++second;
        if (second == 60) {
            if (!advance(candidate, 60))
                return false;
            continue;
        }
        if (second - c.second > UINT32_MAX - candidate)
            return false;
        next = candidate + (second - c.second);
        return true;
    }
}
} // namespace newmota
