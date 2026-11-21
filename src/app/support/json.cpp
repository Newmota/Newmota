/*
  json.cpp - Bounded JSON tokenizer shared by configuration parsers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file json.cpp
 * @brief Bounded JSON tokenizer and value helpers without heap allocation.
 */
#include "support/json.h"
#include "support/number.h"
#include "support/text.h"
#include <climits>

namespace newmota::support {
void JsonReader::whitespace() {
    while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
                                            input_[position_] == '\r' || input_[position_] == '\n'))
        ++position_;
}

bool JsonReader::string(std::size_t& index) {
    if (position_ >= input_.size() || input_[position_] != '"' || count_ >= capacity_)
        return false;
    index = count_++;
    auto& token = tokens_[index];
    token.kind = JsonKind::string;
    token.start = static_cast<std::uint16_t>(++position_);
    while (position_ < input_.size()) {
        const auto c = static_cast<unsigned char>(input_[position_++]);
        if (c == '"') {
            token.length = static_cast<std::uint16_t>(position_ - token.start - 1);
            token.next = static_cast<std::uint16_t>(count_);
            return true;
        }
        if (c < 0x20)
            return false;
        if (c == '\\') {
            if (position_ >= input_.size())
                return false;
            const auto escape = input_[position_++];
            if (escape == 'u') {
                if (position_ + 4 > input_.size())
                    return false;
                for (unsigned i = 0; i < 4; ++i) {
                    const auto digit = input_[position_++];
                    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                            (digit >= 'A' && digit <= 'F')))
                        return false;
                }
            } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
                       escape != 'f' && escape != 'n' && escape != 'r' && escape != 't')
                return false;
        }
    }
    return false;
}

bool JsonReader::value(std::size_t& index, std::uint8_t depth) {
    whitespace();
    if (position_ >= input_.size() || depth > 8 || count_ >= capacity_)
        return false;
    const auto first = input_[position_];
    if (first == '"')
        return string(index);
    index = count_++;
    auto& token = tokens_[index];
    if (first == '{' || first == '[') {
        const bool object = first == '{';
        token.kind = object ? JsonKind::object : JsonKind::array;
        ++position_;
        whitespace();
        const auto close = object ? '}' : ']';
        if (position_ < input_.size() && input_[position_] == close) {
            ++position_;
            token.next = static_cast<std::uint16_t>(count_);
            return true;
        }
        while (position_ < input_.size()) {
            std::size_t child{};
            if (object) {
                if (!string(child))
                    return false;
                whitespace();
                if (position_ >= input_.size() || input_[position_++] != ':')
                    return false;
            }
            if (!value(child, depth + 1))
                return false;
            whitespace();
            if (position_ < input_.size() && input_[position_] == close) {
                ++position_;
                token.next = static_cast<std::uint16_t>(count_);
                return true;
            }
            if (position_ >= input_.size() || input_[position_++] != ',')
                return false;
            whitespace();
        }
        return false;
    }
    token.kind = JsonKind::number;
    token.start = static_cast<std::uint16_t>(position_);
    while (position_ < input_.size() && input_[position_] != ',' && input_[position_] != ']' &&
           input_[position_] != '}' && input_[position_] != ' ' && input_[position_] != '\t' &&
           input_[position_] != '\r' && input_[position_] != '\n')
        ++position_;
    if (token.start == position_)
        return false;
    token.length = static_cast<std::uint16_t>(position_ - token.start);
    const auto raw = input_.substr(token.start, token.length);
    if (raw == "true" || raw == "false" || raw == "null")
        token.kind = JsonKind::other;
    else if (raw.size() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
        for (std::size_t i = 2; i < raw.size(); ++i)
            if (!((raw[i] >= '0' && raw[i] <= '9') || (raw[i] >= 'a' && raw[i] <= 'f') ||
                    (raw[i] >= 'A' && raw[i] <= 'F')))
                return false;
    } else {
        std::size_t digit{};
        if (raw[0] == '-')
            digit = 1;
        if (digit == raw.size())
            return false;
        if (raw[digit] == '0')
            ++digit;
        else if (raw[digit] >= '1' && raw[digit] <= '9')
            while (digit < raw.size() && raw[digit] >= '0' && raw[digit] <= '9')
                ++digit;
        else
            return false;
        const auto digits = [&] {
            const auto start = digit;
            while (digit < raw.size() && raw[digit] >= '0' && raw[digit] <= '9')
                ++digit;
            return digit != start;
        };
        if (digit < raw.size() && raw[digit] == '.' && (++digit, !digits()))
            return false;
        if (digit < raw.size() && (raw[digit] == 'e' || raw[digit] == 'E')) {
            ++digit;
            if (digit < raw.size() && (raw[digit] == '+' || raw[digit] == '-'))
                ++digit;
            if (!digits())
                return false;
        }
        if (digit != raw.size())
            return false;
    }
    token.next = static_cast<std::uint16_t>(count_);
    return true;
}

bool JsonReader::parse(std::size_t& root) {
    count_ = position_ = 0;
    if (input_.size() > UINT16_MAX || capacity_ > UINT16_MAX) // offsets are 16-bit
        return false;
    if (!value(root, 0))
        return false;
    whitespace();
    return position_ == input_.size();
}

std::size_t JsonReader::member(std::size_t object, std::string_view name, bool ignore_case) const {
    if (object >= count_ || tokens_[object].kind != JsonKind::object)
        return capacity_;
    for (auto key = object + 1; key < tokens_[object].next;) {
        const auto value = tokens_[key].next;
        if (tokens_[key].kind != JsonKind::string || value >= tokens_[object].next)
            return capacity_;
        if (ignore_case ? ascii_iequals(text(key), name) : text(key) == name)
            return value;
        key = tokens_[value].next;
    }
    return capacity_;
}

std::size_t JsonReader::first(std::size_t container) const {
    if (container >= count_ ||
        (tokens_[container].kind != JsonKind::object && tokens_[container].kind != JsonKind::array))
        return capacity_;
    return container + 1 < tokens_[container].next ? container + 1 : capacity_;
}

std::size_t JsonReader::next(std::size_t container, std::size_t item) const {
    if (container >= count_ || item >= count_)
        return capacity_;
    const auto after = tokens_[item].next;
    return after < tokens_[container].next ? after : capacity_;
}

std::size_t JsonReader::array_item(std::size_t array, std::size_t wanted) const {
    if (array >= count_ || tokens_[array].kind != JsonKind::array)
        return capacity_;
    auto item = array + 1;
    for (std::size_t i = 0; i < wanted && item < tokens_[array].next; ++i)
        item = tokens_[item].next;
    return item < tokens_[array].next ? item : capacity_;
}

bool json_integer(const JsonReader& json, std::size_t token, std::int32_t minimum,
    std::int32_t maximum, std::int32_t& output) {
    if (token >= json.token_count() || json.token(token).kind != JsonKind::number)
        return false;
    return parse_signed_decimal(json.text(token), minimum, maximum, output);
}

bool json_unsigned(
    const JsonReader& json, std::size_t token, std::uint32_t maximum, std::uint32_t& output) {
    if (token >= json.token_count() || json.token(token).kind != JsonKind::number)
        return false;
    auto text = json.text(token);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::uint32_t value{};
        for (const auto digit : text.substr(2)) {
            if (value > 0x0fffffffU)
                return false;
            value <<= 4;
            if (digit >= '0' && digit <= '9')
                value |= digit - '0';
            else if (digit >= 'a' && digit <= 'f')
                value |= digit - 'a' + 10;
            else if (digit >= 'A' && digit <= 'F')
                value |= digit - 'A' + 10;
            else
                return false;
        }
        if (value > maximum)
            return false;
        output = value;
        return true;
    }
    return parse_unsigned_decimal(text, maximum, output);
}

bool json_decimal(
    const JsonReader& json, std::size_t token, std::uint32_t maximum, std::uint32_t& output) {
    return token < json.token_count() && json.token(token).kind == JsonKind::number &&
           parse_unsigned_decimal(json.text(token), maximum, output);
}

bool json_copy_string(const JsonReader& json, std::size_t token, char* output, std::size_t capacity,
    std::size_t& length, bool escaped_controls) {
    if (token >= json.token_count() || json.token(token).kind != JsonKind::string || capacity == 0)
        return false;
    const auto text = json.text(token);
    length = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto value = static_cast<unsigned char>(text[i]);
        if (value == '\\') {
            if (++i >= text.size())
                return false;
            switch (text[i]) {
            case '"':
                value = '"';
                break;
            case '\\':
                value = '\\';
                break;
            case '/':
                value = '/';
                break;
            case 'b':
                value = '\b';
                break;
            case 'f':
                value = '\f';
                break;
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case 'u': {
                if (i + 4 >= text.size())
                    return false;
                unsigned code{};
                for (unsigned digit = 0; digit < 4; ++digit) {
                    const auto c = text[++i];
                    code <<= 4;
                    if (c >= '0' && c <= '9')
                        code |= c - '0';
                    else if (c >= 'a' && c <= 'f')
                        code |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        code |= c - 'A' + 10;
                    else
                        return false;
                }
                if (code > 0x7f)
                    return false;
                value = static_cast<unsigned char>(code);
                break;
            }
            default:
                return false;
            }
        }
        // Raw controls never reach here: the tokenizer rejects them inside strings.
        if ((value < 0x20 && !escaped_controls) || length + 1 >= capacity)
            return false;
        output[length++] = static_cast<char>(value);
    }
    output[length] = '\0';
    return true;
}
} // namespace newmota::support
