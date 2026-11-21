/*
  crypto_kat.h - shared helpers for the BearSSL host known-answer tests

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef NEWMOTA_CRYPTO_KAT_H
#define NEWMOTA_CRYPTO_KAT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace crypto_kat {

using Bytes = std::vector<std::uint8_t>;

/** @brief Decodes one hexadecimal digit of a compile-time test constant. */
[[nodiscard]] inline std::uint8_t hex_digit(const char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return static_cast<std::uint8_t>(value - 'A' + 10);
    std::fprintf(stderr, "crypto kat: malformed hexadecimal constant\n");
    std::abort();
}

/** @brief Decodes an even-length hexadecimal test constant. */
[[nodiscard]] inline Bytes from_hex(const char* const text) {
    Bytes bytes;
    for (std::size_t index = 0; text[index] != '\0'; index += 2U) {
        const auto high = static_cast<std::uint8_t>(hex_digit(text[index]) << 4);
        bytes.push_back(static_cast<std::uint8_t>(high | hex_digit(text[index + 1U])));
    }
    return bytes;
}

/** @brief Encodes a byte range for a failure message. */
[[nodiscard]] inline std::string to_hex(const std::uint8_t* const data, const std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2U);
    for (std::size_t index = 0; index < size; ++index) {
        text.push_back(digits[data[index] >> 4]);
        text.push_back(digits[data[index] & 0x0FU]);
    }
    return text;
}

/** @brief Encodes a byte vector for a failure message. */
[[nodiscard]] inline std::string to_hex(const Bytes& data) {
    return to_hex(data.data(), data.size());
}

/** @brief Collects the failed expectations of one primitive suite. */
class Report {
  public:
    /** @brief Records one failed boolean expectation. */
    void expect(const bool condition, const char* const message) {
        if (condition)
            return;
        std::fprintf(stderr, "crypto kat: %s\n", message);
        ++failures_;
    }

    /** @brief Records a failed comparison against a hexadecimal constant. */
    void expect_hex(const Bytes& actual, const char* const expected, const char* const message) {
        const Bytes wanted = from_hex(expected);
        if (actual.size() == wanted.size() &&
            std::memcmp(actual.data(), wanted.data(), actual.size()) == 0) {
            return;
        }
        std::fprintf(stderr, "crypto kat: %s\n  expected %s\n  actual   %s\n", message,
            to_hex(wanted).c_str(), to_hex(actual).c_str());
        ++failures_;
    }

    /** @brief Returns the number of failed expectations. */
    [[nodiscard]] int failures() const { return failures_; }

  private:
    int failures_ = 0;
};

} // namespace crypto_kat

#endif
