/*
  time_format.cpp - bounded portable civil-time formats

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file time_format.cpp
 * @brief Bounded portable civil-time formats.
 */

#include "services/time/time_format.h"

#include <cstdio>
#include <cstring>

namespace newmota {
namespace {
constexpr const char* months[] = {"January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"};
constexpr const char* weekdays[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

const char* composite(char c) {
    switch (c) {
    case 'F':
        return "%Y-%m-%d";
    case 'T':
        return "%H:%M:%S";
    case 'R':
        return "%H:%M";
    case 'D':
        return "%m/%d/%y";
    default:
        return nullptr;
    }
}

bool supported(char c) {
    return std::strchr("%YymdeHIMSp aAbBhwujFTRDnt", c) && c != ' ';
}

bool valid(std::string_view f) {
    if (f.size() > 128 || f.find('\0') != f.npos)
        return false;
    for (std::size_t i = 0; i < f.size(); ++i)
        if (f[i] == '%' && (++i == f.size() || !supported(f[i])))
            return false;
    return true;
}

unsigned day_year(const CivilTime& c) {
    CivilTime start = c;
    start.month = 1;
    start.day = 1;
    start.hour = 0;
    start.minute = 0;
    start.second = 0;
    std::uint32_t a{}, b{};
    if (!TimeService::epoch_seconds(start, a) || !TimeService::epoch_seconds(c, b))
        return 0;
    return (b - a) / 86400 + 1;
}

struct Writer {
    char* out;
    std::size_t capacity, size{};

    bool append(std::string_view s) {
        if (s.size() > 255 - size || size + s.size() >= capacity)
            return false;
        std::memcpy(out + size, s.data(), s.size());
        size += s.size();
        out[size] = 0;
        return true;
    }

    bool number(unsigned n, unsigned width, char pad = '0') {
        char b[16];
        std::snprintf(b, sizeof(b), "%u", n);
        auto len = std::strlen(b);
        while (len < width) {
            if (!append(std::string_view(&pad, 1)))
                return false;
            --width;
        }
        return append(b);
    }
};

bool format(Writer& w, std::string_view f, const CivilTime& c) {
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            if (!w.append(f.substr(i, 1)))
                return false;
            continue;
        }
        char k = f[++i];
        if (auto s = composite(k)) {
            if (!format(w, s, c))
                return false;
            continue;
        }
        bool ok = true;
        switch (k) {
        case '%':
            ok = w.append("%");
            break;
        case 'n':
            ok = w.append("\n");
            break;
        case 't':
            ok = w.append("\t");
            break;
        case 'Y':
            ok = w.number(c.year, 4);
            break;
        case 'y':
            ok = w.number(c.year % 100, 2);
            break;
        case 'm':
            ok = w.number(c.month, 2);
            break;
        case 'd':
            ok = w.number(c.day, 2);
            break;
        case 'e':
            ok = w.number(c.day, 2, ' ');
            break;
        case 'H':
            ok = w.number(c.hour, 2);
            break;
        case 'I':
            ok = w.number(c.hour % 12 ? c.hour % 12 : 12, 2);
            break;
        case 'M':
            ok = w.number(c.minute, 2);
            break;
        case 'S':
            ok = w.number(c.second, 2);
            break;
        case 'p':
            ok = w.append(c.hour < 12 ? "AM" : "PM");
            break;
        case 'a':
            ok = w.append(std::string_view(weekdays[c.day_of_week - 1], 3));
            break;
        case 'A':
            ok = w.append(weekdays[c.day_of_week - 1]);
            break;
        case 'b':
        case 'h':
            ok = w.append(std::string_view(months[c.month - 1], 3));
            break;
        case 'B':
            ok = w.append(months[c.month - 1]);
            break;
        case 'w':
            ok = w.number(c.day_of_week - 1, 1);
            break;
        case 'u':
            ok = w.number(c.day_of_week == 1 ? 7 : c.day_of_week - 1, 1);
            break;
        case 'j':
            ok = w.number(day_year(c), 3);
            break;
        default:
            return false;
        }
        if (!ok)
            return false;
    }
    return true;
}

struct Parser {
    std::string_view input;
    std::size_t pos{};
    int fields[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1}; // year month day hour minute second weekday yearday hour12 ampm

    bool set(unsigned field, int value) {
        if (fields[field] >= 0 && fields[field] != value)
            return false;
        fields[field] = value;
        return true;
    }

    bool number(unsigned field, unsigned digits, int min, int max, bool blank = false) {
        if (blank)
            while (pos < input.size() && input[pos] == ' ')
                ++pos;
        int n = 0;
        unsigned count = 0;
        while (pos < input.size() && count < digits && input[pos] >= '0' && input[pos] <= '9') {
            n = n * 10 + input[pos++] - '0';
            ++count;
        }
        return count && n >= min && n <= max && set(field, n);
    }

    bool name(unsigned field, const char* const* names, unsigned count, bool shortname) {
        for (unsigned n = 0; n < count; ++n) {
            auto length = shortname ? 3 : std::strlen(names[n]);
            if (input.size() - pos < length)
                continue;
            bool match = true;
            for (unsigned j = 0; j < length; ++j)
                if (lower(input[pos + j]) != lower(names[n][j]))
                    match = false;
            if (match) {
                pos += length;
                return set(field, static_cast<int>(field == 1 ? n + 1 : n));
            }
        }
        return false;
    }

    bool parse(std::string_view f) {
        for (std::size_t i = 0; i < f.size(); ++i) {
            char k = f[i];
            if (space(k)) {
                while (pos < input.size() && space(input[pos]))
                    ++pos;
                continue;
            }
            if (k != '%') {
                if (pos == input.size() || input[pos++] != k)
                    return false;
                continue;
            }
            k = f[++i];
            if (auto s = composite(k)) {
                if (!parse(s))
                    return false;
                continue;
            }
            bool ok = true;
            switch (k) {
            case '%':
                ok = pos < input.size() && input[pos++] == '%';
                break;
            case 'n':
            case 't':
                while (pos < input.size() && space(input[pos]))
                    ++pos;
                break;
            case 'Y':
                ok = number(0, 4, 0, 9999);
                break;
            case 'y': {
                int old = fields[0];
                fields[0] = -1;
                ok = number(0, 2, 0, 99);
                if (ok) {
                    int y = fields[0] + (fields[0] <= 68 ? 2000 : 1900);
                    fields[0] = old;
                    ok = set(0, y);
                }
                break;
            }
            case 'm':
                ok = number(1, 2, 1, 12);
                break;
            case 'd':
            case 'e':
                ok = number(2, 2, 1, 31, k == 'e');
                break;
            case 'H':
                ok = number(3, 2, 0, 23);
                break;
            case 'I':
                ok = number(8, 2, 1, 12);
                break;
            case 'M':
                ok = number(4, 2, 0, 59);
                break;
            case 'S':
                ok = number(5, 2, 0, 59);
                break;
            case 'w':
                ok = number(6, 1, 0, 6);
                break;
            case 'u': {
                int old = fields[6];
                fields[6] = -1;
                ok = number(6, 1, 1, 7);
                if (ok) {
                    int d = fields[6] % 7;
                    fields[6] = old;
                    ok = set(6, d);
                }
                break;
            }
            case 'j':
                ok = number(7, 3, 1, 366);
                break;
            case 'a':
            case 'A':
                ok = name(6, weekdays, 7, k == 'a');
                break;
            case 'b':
            case 'h':
            case 'B':
                ok = name(1, months, 12, k != 'B');
                break;
            case 'p':
                if (input.size() - pos < 2)
                    ok = false;
                else {
                    auto a = lower(input[pos]), b = lower(input[pos + 1]);
                    ok = (a == 'a' || a == 'p') && b == 'm' && set(9, a == 'p');
                    if (ok)
                        pos += 2;
                }
                break;
            default:
                return false;
            }
            if (!ok)
                return false;
        }
        return true;
    }
};
} // namespace

TimeFormatStatus format_time(std::string_view f, std::uint32_t epoch, char* output,
    std::size_t capacity, std::size_t& size) {
    size = 0;
    if (!valid(f))
        return TimeFormatStatus::invalid_format;
    if (!capacity)
        return TimeFormatStatus::overflow;
    output[0] = 0;
    Writer w{output, capacity};
    if (!format(w, f, TimeService::civil_time(epoch)))
        return TimeFormatStatus::overflow;
    size = w.size;
    return TimeFormatStatus::ok;
}

TimeFormatStatus parse_time(
    std::string_view input, std::string_view f, std::uint32_t& epoch, std::size_t& consumed) {
    consumed = 0;
    if (!valid(f) || input.size() > 256 || input.find('\0') != input.npos)
        return TimeFormatStatus::invalid_format;
    Parser p{input};
    if (!p.parse(f))
        return TimeFormatStatus::mismatch;
    auto* v = p.fields;
    if ((v[8] >= 0) != (v[9] >= 0))
        return TimeFormatStatus::mismatch;
    if (v[8] >= 0) {
        int hour = v[8] % 12 + v[9] * 12;
        if (!p.set(3, hour))
            return TimeFormatStatus::mismatch;
    }
    CivilTime c{static_cast<std::uint16_t>(v[0] < 0 ? 1970 : v[0]),
        static_cast<std::uint8_t>(v[1] < 0 ? 1 : v[1]),
        static_cast<std::uint8_t>(v[2] < 0 ? 1 : v[2]),
        static_cast<std::uint8_t>(v[3] < 0 ? 0 : v[3]),
        static_cast<std::uint8_t>(v[4] < 0 ? 0 : v[4]),
        static_cast<std::uint8_t>(v[5] < 0 ? 0 : v[5]), 0};
    if (v[7] >= 0) {
        CivilTime jan = c;
        jan.month = 1;
        jan.day = 1;
        jan.hour = jan.minute = jan.second = 0;
        std::uint32_t first;
        if (!TimeService::epoch_seconds(jan, first))
            return TimeFormatStatus::mismatch;
        std::uint64_t d =
            static_cast<std::uint64_t>(first) + static_cast<std::uint64_t>(v[7] - 1) * 86400ULL;
        if (d > UINT32_MAX)
            return TimeFormatStatus::mismatch;
        auto date = TimeService::civil_time(static_cast<std::uint32_t>(d));
        if (date.year != c.year || (v[1] >= 0 && v[1] != date.month) ||
            (v[2] >= 0 && v[2] != date.day))
            return TimeFormatStatus::mismatch;
        c.month = date.month;
        c.day = date.day;
    }
    if (!TimeService::epoch_seconds(c, epoch))
        return TimeFormatStatus::mismatch;
    if (v[6] >= 0 && TimeService::civil_time(epoch).day_of_week - 1 != v[6])
        return TimeFormatStatus::mismatch;
    consumed = p.pos;
    return TimeFormatStatus::ok;
}
} // namespace newmota
