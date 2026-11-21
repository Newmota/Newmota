/*
  widgets.cpp - script-defined dashboard widgets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widgets.cpp
 * @brief Fixed-slot registry; only the stored JSON objects are heap-allocated.
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

struct Entry {
    std::array<char, kIdCapacity + 1> id{};
    char* json{};
    std::size_t size{};
    std::int32_t order{};
    std::uint32_t sequence{}, revision{};
};

std::array<Entry, kCapacity> entries{};
std::uint32_t next_sequence{}, next_revision{};
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

Entry* find(const std::string_view id) {
    for (auto& entry : entries)
        if (entry.json != nullptr && id == entry.id.data())
            return &entry;
    return nullptr;
}

void release(Entry& entry) {
    std::free(entry.json);
    entry = {};
}

/** @brief Display order: explicit order first, then first registration. */
bool before(const Entry& a, const Entry& b) {
    return a.order != b.order ? a.order < b.order : a.sequence < b.sequence;
}

} // namespace

SetResult set(const std::string_view id, const std::string_view json, const std::int32_t order) {
    // Only what keeps memory and the 4 KiB response safe is checked.
    if (id.empty() || id.size() > kIdCapacity || json.size() < 3 || json.size() > kMaximumBytes)
        return SetResult::invalid;
    auto* entry = find(id);
    if (entry == nullptr) {
        for (auto& candidate : entries)
            if (candidate.json == nullptr) {
                entry = &candidate;
                break;
            }
        if (entry == nullptr)
            return SetResult::full;
    }
    auto* copy = static_cast<char*>(std::malloc(json.size()));
    if (copy == nullptr)
        return SetResult::memory;
    std::memcpy(copy, json.data(), json.size());
    if (entry->json == nullptr) {
        std::memcpy(entry->id.data(), id.data(), id.size());
        entry->id[id.size()] = '\0';
        entry->sequence = next_sequence++;
    }
    std::free(entry->json);
    entry->json = copy;
    entry->size = json.size();
    entry->order = order;
    // A random per-boot seed keeps a page that survived a restart from trusting an old
    // revision; uptime alone repeats when scripts register at the same startup point.
    if (next_revision == 0 && !platform::web_random(&next_revision, sizeof next_revision))
        next_revision = platform::monotonic_milliseconds();
    if (next_revision == 0)
        next_revision = 1;
    entry->revision = next_revision++;
    return SetResult::ok;
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
        if (entry.json != nullptr)
            release(entry);
    action_count = 0;
}

ActionResult enqueue_action(const std::string_view body) {
    const auto first = body.find('\n');
    const auto second = first == body.npos ? body.npos : body.find('\n', first + 1);
    if (second == body.npos)
        return ActionResult::invalid;
    const auto id = body.substr(0, first);
    const auto part = body.substr(first + 1, second - first - 1);
    const auto value = body.substr(second + 1);
    Action action{};
    if (part.empty() || !copy_field(action.id, id) || !copy_field(action.part, part) ||
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

bool write_index(CommandOutput& output) {
    if (!output.write("{\"widgets\":["))
        return false;
    const Entry* previous = nullptr;
    for (std::size_t written = 0;; ++written) {
        const Entry* next = nullptr;
        for (const auto& entry : entries)
            if (entry.json != nullptr && (previous == nullptr || before(*previous, entry)) &&
                (next == nullptr || before(entry, *next)))
                next = &entry;
        if (next == nullptr)
            break;
        if (!output.write(written ? ",{\"id\":" : "{\"id\":") ||
            !output.json_string(next->id.data()) ||
            !output.format(",\"rev\":%lu}", static_cast<unsigned long>(next->revision)))
            return false;
        previous = next;
    }
    return output.write("]}");
}

bool write_widget(const std::string_view id, CommandOutput& output) {
    const auto* entry = find(id);
    return entry != nullptr && output.write("{\"id\":") && output.json_string(entry->id.data()) &&
           output.format(",\"rev\":%lu,", static_cast<unsigned long>(entry->revision)) &&
           output.write({entry->json + 1, entry->size - 1});
}

} // namespace newmota::widgets
