/*
  integer_object.h - Bounded integer JSON object parser

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file integer_object.h
 * @brief Parses up to three nested object levels with signed integer leaves.
 */
#pragma once

#include "support/json.h"
#include "support/number.h"
#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

namespace newmota::support {
/** @brief Invokes a caller's visitor for each integer leaf of a tokenized document.
 * The visitor accepts (path array, zero-based leaf depth, int32 value) and returns
 * false to reject a field. Keys must be unescaped; arrays and non-integer values
 * are rejected. Caller-owned output must be staged to ensure atomic validation.
 * Token storage is allocated for the duration of parse() and sized from the input: every
 * token spans at least two characters, so size / 2 + 1 tokens hold any valid document.
 */
template <typename Visitor> class IntegerObjectParser {
  public:
    IntegerObjectParser(std::string_view text, Visitor& visitor) : text_(text), visitor_(visitor) {}

    bool parse() {
        const std::size_t capacity = text_.size() / 2 + 1;
        std::unique_ptr<JsonToken[]> tokens{new (std::nothrow) JsonToken[capacity]};
        if (!tokens)
            return false;
        JsonReader json{text_, tokens.get(), capacity};
        std::size_t root = 0;
        return json.parse(root) && json.token(root).kind == JsonKind::object &&
               object(json, root, 0);
    }

  private:
    bool object(const JsonReader& json, std::size_t container, unsigned depth) {
        if (depth >= path_.size())
            return false;
        for (auto key = json.first(container); key < json.token_count();) {
            const auto value = json.next(container, key);
            if (value >= json.token_count() || json.text(key).find('\\') != std::string_view::npos)
                return false;
            path_[depth] = json.text(key);
            if (json.token(value).kind == JsonKind::object) {
                if (!object(json, value, depth + 1))
                    return false;
            } else {
                std::int32_t number{};
                if (json.token(value).kind != JsonKind::number ||
                    !parse_signed_decimal(json.text(value), INT32_MIN, INT32_MAX, number) ||
                    !visitor_(path_, depth, number))
                    return false;
            }
            key = json.next(container, value);
        }
        return true;
    }

    std::string_view text_;
    Visitor& visitor_;
    std::array<std::string_view, 3> path_{};
};
} // namespace newmota::support
