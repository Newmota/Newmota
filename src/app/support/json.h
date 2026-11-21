/*
  json.h - Bounded JSON tokenizer shared by configuration parsers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file json.h
 * @brief Bounded JSON tokenizer and value helpers without heap allocation.
 *
 * The caller owns the token storage, so each user sizes it for its own documents while the
 * code exists once. A token is 8 bytes and records where its value ends, so skipping a member
 * never walks its children. Lookups return a value of at least token_count() when nothing
 * matches. Numbers follow the JSON grammar plus Tasmota's 0x-prefixed hexadecimal.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::support {
enum class JsonKind : std::uint8_t { object, array, string, number, other };

struct JsonToken {
    JsonKind kind{};
    std::uint16_t start{}, length{};
    std::uint16_t next{}; ///< The token after this value and everything inside it.
};

/** @brief Tokenizes one JSON document of at most 64 KiB into caller-owned tokens. */
class JsonReader {
  public:
    JsonReader(std::string_view input, JsonToken* tokens, std::size_t capacity)
        : input_(input), tokens_(tokens), capacity_(capacity) {}

    /** @brief Tokenizes the whole input. @return False when invalid, too deep or too large. */
    [[nodiscard]] bool parse(std::size_t& root);

    [[nodiscard]] const JsonToken& token(std::size_t index) const { return tokens_[index]; }

    [[nodiscard]] std::string_view text(std::size_t index) const {
        return input_.substr(tokens_[index].start, tokens_[index].length);
    }

    /** @brief Finds the value of an object member; keys are compared as written (not decoded).
     *  @param ignore_case Compares ASCII letters case-insensitively, as Tasmota commands do.
     */
    [[nodiscard]] std::size_t member(
        std::size_t object, std::string_view name, bool ignore_case = false) const;
    /** @brief Returns the first item of an array or the first key of an object. */
    [[nodiscard]] std::size_t first(std::size_t container) const;
    /** @brief Returns the item after @p item inside @p container; for an object the sequence
     *  alternates key, value, key, ...
     */
    [[nodiscard]] std::size_t next(std::size_t container, std::size_t item) const;
    /** @brief Finds the zero-based item of an array. */
    [[nodiscard]] std::size_t array_item(std::size_t array, std::size_t wanted) const;

    [[nodiscard]] std::size_t token_count() const { return count_; }

  private:
    void whitespace();
    bool string(std::size_t& index);
    bool value(std::size_t& index, std::uint8_t depth);

    std::string_view input_;
    JsonToken* tokens_;
    std::size_t capacity_, count_{}, position_{};
};

/** @brief Parses a number token as a bounded signed decimal integer. */
[[nodiscard]] bool json_integer(const JsonReader& json, std::size_t token, std::int32_t minimum,
    std::int32_t maximum, std::int32_t& output);
/** @brief Parses a number token in decimal or 0x-prefixed hexadecimal within @p maximum. */
[[nodiscard]] bool json_unsigned(
    const JsonReader& json, std::size_t token, std::uint32_t maximum, std::uint32_t& output);
/** @brief Parses a number token as plain unsigned decimal digits within @p maximum: no sign,
 *  fraction, exponent or hexadecimal.
 */
[[nodiscard]] bool json_decimal(
    const JsonReader& json, std::size_t token, std::uint32_t maximum, std::uint32_t& output);
/** @brief Decodes a string token into NUL-terminated ASCII storage.
 *  @param escaped_controls Accepts control characters written as escapes (`\n`, `\t`).
 *  @return False for invalid escapes, non-ASCII escapes, controls or too little capacity.
 */
[[nodiscard]] bool json_copy_string(const JsonReader& json, std::size_t token, char* output,
    std::size_t capacity, std::size_t& length, bool escaped_controls = false);
} // namespace newmota::support
