/*
  widgets.cpp - script-defined dashboard widgets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widgets.cpp
 * @brief Fixed-slot registry; only the stored markup and data are heap-allocated.
 */
#include "services/web/widgets.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/web.h"

#include <array>
#include <cstdlib>
#include <cstring>

namespace newmota::widgets {
namespace {

struct Text {
    char* bytes{};
    std::size_t size{};
    std::uint32_t revision{};
};

struct Entry {
    std::array<char, kIdCapacity + 1> id{};
    Text markup{}, data{};
    std::int32_t order{};
    std::uint32_t sequence{};
};

std::array<Entry, kCapacity> entries{};
std::uint32_t next_sequence{}, revision{}, generation{};
std::array<Action, kActionCapacity> actions{};
std::size_t action_count{};
std::uint32_t last_viewer{};
bool viewer_seen{};

/** @brief Copies @p text into a terminated field. @return false when it does not fit. */
template <std::size_t N> bool copy_field(std::array<char, N>& field, std::string_view text) {
    if (text.size() >= N)
        return false;
    std::memcpy(field.data(), text.data(), text.size());
    field[text.size()] = '\0';
    return true;
}

/** @brief Ids keep to characters that need no escaping in a path or a JSON string. */
bool valid_id(const std::string_view id) {
    // "." and ".." would be resolved as path segments by the browser.
    if (id.empty() || id.size() > kIdCapacity || id == "." || id == "..")
        return false;
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}

Entry* find(const std::string_view id) {
    for (auto& entry : entries)
        if (entry.markup.bytes != nullptr && id == entry.id.data())
            return &entry;
    return nullptr;
}

/** @brief Replaces @p text with a copy of @p value under the next revision. */
bool store(Text& text, const std::string_view value) {
    auto* copy = static_cast<char*>(std::malloc(value.size()));
    if (copy == nullptr)
        return false;
    std::memcpy(copy, value.data(), value.size());
    std::free(text.bytes);
    text.bytes = copy;
    text.size = value.size();
    text.revision = ++revision;
    return true;
}

void release(Entry& entry) {
    std::free(entry.markup.bytes);
    std::free(entry.data.bytes);
    entry = {};
}

/** @brief Display order: explicit order first, then first registration. */
bool before(const Entry& a, const Entry& b) {
    return a.order != b.order ? a.order < b.order : a.sequence < b.sequence;
}

} // namespace

SetResult set_markup(
    const std::string_view id, const std::string_view markup, const std::int32_t* order) {
    // Only what keeps memory and the 4 KiB response safe is checked.
    if (!valid_id(id) || markup.empty() || markup.size() > kMarkupBytes)
        return SetResult::invalid;
    auto* entry = find(id);
    const bool created = entry == nullptr;
    if (created) {
        for (auto& candidate : entries)
            if (candidate.markup.bytes == nullptr) {
                entry = &candidate;
                break;
            }
        if (entry == nullptr)
            return SetResult::full;
    }
    if (!store(entry->markup, markup))
        return SetResult::memory;
    if (created) {
        std::memcpy(entry->id.data(), id.data(), id.size());
        entry->id[id.size()] = '\0';
        entry->sequence = next_sequence++;
    }
    if (order != nullptr)
        entry->order = *order;
    return SetResult::ok;
}

SetResult set_data(const std::string_view id, const std::string_view json) {
    if (json.empty() || json.size() > kDataBytes)
        return SetResult::invalid;
    auto* entry = find(id);
    if (entry == nullptr)
        return SetResult::unknown;
    return store(entry->data, json) ? SetResult::ok : SetResult::memory;
}

int slot(const std::string_view id) {
    const auto* entry = find(id);
    return entry == nullptr ? -1 : static_cast<int>(entry - entries.data());
}

bool remove(const std::string_view id) {
    auto* entry = find(id);
    if (entry == nullptr)
        return false;
    release(*entry);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < action_count; ++i)
        if (id != actions[i].id.data())
            actions[kept++] = actions[i];
    action_count = kept;
    return true;
}

void clear() {
    for (auto& entry : entries)
        if (entry.markup.bytes != nullptr)
            release(entry);
    action_count = 0;
}

ActionResult enqueue_action(const std::string_view body) {
    const auto first = body.find('\n');
    const auto second = first == body.npos ? body.npos : body.find('\n', first + 1);
    if (second == body.npos)
        return ActionResult::invalid;
    const auto id = body.substr(0, first);
    const auto name = body.substr(first + 1, second - first - 1);
    const auto value = body.substr(second + 1);
    Action action{};
    if (name.empty() || !copy_field(action.id, id) || !copy_field(action.name, name) ||
        !copy_field(action.value, value))
        return ActionResult::invalid;
    if (find(id) == nullptr)
        return ActionResult::unknown;
    if (action_count == actions.size())
        return ActionResult::full;
    actions[action_count++] = action;
    return ActionResult::queued;
}

bool pop_action(Action& action) {
    if (action_count == 0)
        return false;
    action = actions[0];
    for (std::size_t i = 1; i < action_count; ++i)
        actions[i - 1] = actions[i];
    --action_count;
    return true;
}

bool actions_pending() {
    return action_count != 0;
}

void note_viewer(const std::uint32_t now) {
    last_viewer = now;
    viewer_seen = true;
}

bool viewers(const std::uint32_t now) {
    return viewer_seen && now - last_viewer < kViewerWindowMilliseconds;
}

bool write_updates(const std::uint32_t since, CommandOutput& output, const std::size_t limit) {
    // A page that survived a restart sees another generation and starts over.
    if (generation == 0 && !platform::web_random(&generation, sizeof generation))
        generation = platform::monotonic_milliseconds();
    if (generation == 0)
        generation = 1;
    if (!output.format("{\"gen\":%lu,\"widgets\":[", static_cast<unsigned long>(generation)))
        return false;
    const Entry* previous = nullptr;
    for (std::size_t written = 0;; ++written) {
        const Entry* next = nullptr;
        for (const auto& entry : entries)
            if (entry.markup.bytes != nullptr &&
                (previous == nullptr || before(*previous, entry)) &&
                (next == nullptr || before(entry, *next)))
                next = &entry;
        if (next == nullptr)
            break;
        if (!output.write(written ? ",{\"id\":" : "{\"id\":") ||
            !output.json_string(next->id.data()) ||
            !output.format(",\"m\":%lu,\"d\":%lu}",
                static_cast<unsigned long>(next->markup.revision),
                static_cast<unsigned long>(next->data.revision)))
            return false;
        previous = next;
    }
    if (!output.write("],\"data\":{"))
        return false;
    // Oldest change first, so a partial answer can name the revision it covers.
    static constexpr std::size_t kClosing = 48;
    std::uint32_t covered = since;
    bool more = false;
    for (std::size_t written = 0;; ++written) {
        const Entry* next = nullptr;
        for (const auto& entry : entries)
            if (entry.markup.bytes != nullptr && entry.data.bytes != nullptr &&
                entry.data.revision > covered &&
                (next == nullptr || entry.data.revision < next->data.revision))
                next = &entry;
        if (next == nullptr)
            break;
        if (output.size() + std::strlen(next->id.data()) + next->data.size + 8 + kClosing > limit) {
            more = true;
            break;
        }
        if (!output.write(written ? "," : "") || !output.json_string(next->id.data()) ||
            !output.write(":") || !output.write({next->data.bytes, next->data.size}))
            return false;
        covered = next->data.revision;
    }
    return output.format("},\"rev\":%lu%s}", static_cast<unsigned long>(more ? covered : revision),
        more ? ",\"more\":true" : "");
}

bool write_markup(const std::string_view id, CommandOutput& output) {
    const auto* entry = find(id);
    return entry != nullptr && output.write({entry->markup.bytes, entry->markup.size});
}

} // namespace newmota::widgets
