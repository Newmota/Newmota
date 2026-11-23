/*
  widgets.h - script-defined dashboard widgets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widgets.h
 * @brief Registry of dashboard widgets, each holding trusted markup and a separate JSON value.
 *
 * Producers (the Berry `widget` module) set a widget's markup rarely and its data often. Every
 * change takes the next number of one global revision, so a page asks for everything newer
 * than the revision it has seen and the registry keeps no state per viewer. Content is copied
 * in, so no producer object outlives its owner here. Control actions from the page wait in a
 * bounded queue until the producer's loop takes them.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota {
class CommandOutput;

namespace widgets {

inline constexpr std::size_t kCapacity = 16;
inline constexpr std::size_t kIdCapacity = 31;
/** @brief Largest markup; the markup route sends it as the whole response. */
inline constexpr std::size_t kMarkupBytes = 4096;
/** @brief Largest data value; it fits one update answer beside a full widget list. */
inline constexpr std::size_t kDataBytes = 2816;
inline constexpr std::size_t kActionCapacity = 8;
inline constexpr std::size_t kNameCapacity = 31;
inline constexpr std::size_t kValueCapacity = 63;
/** @brief A page question within this window counts as a viewer. */
inline constexpr std::uint32_t kViewerWindowMilliseconds = 3000;

enum class SetResult : std::uint8_t {
    ok,
    invalid,
    unknown,
    full,
    memory,
};

enum class ActionResult : std::uint8_t {
    queued,
    invalid,
    unknown,
    full,
};

/** @brief One control operation, copied out of the request that carried it. */
struct Action {
    std::array<char, kIdCapacity + 1> id{};
    std::array<char, kNameCapacity + 1> name{};
    std::array<char, kValueCapacity + 1> value{};
};

/**
 * @brief Creates a widget or replaces its markup; its data stays.
 * @param id 1..kIdCapacity characters of `A-Z a-z 0-9 _ - .` other than `.` and `..`, which
 * need no escaping in a path or JSON, so the update answer's size bound holds.
 * @param markup At most kMarkupBytes of HTML; its content is not checked.
 * @param order Display order, or nullptr to keep the current one (0 for a new widget); equal
 * orders keep first-registration order.
 */
[[nodiscard]] SetResult set_markup(
    std::string_view id, std::string_view markup, const std::int32_t* order);

/**
 * @brief Replaces the data of an existing widget.
 * @param json One JSON value of 1..kDataBytes; its content is not checked.
 */
[[nodiscard]] SetResult set_data(std::string_view id, std::string_view json);

/** @return The fixed slot of @p id in 0..kCapacity-1, or -1 when it is unknown. */
[[nodiscard]] int slot(std::string_view id);

/** @return true when a widget with @p id existed; its queued actions are dropped too. */
bool remove(std::string_view id);

/** @brief Drops every widget and action, for example when the producing VM goes away. */
void clear();

/**
 * @brief Queues a control action from the page for an existing widget.
 * @param body `id\nname\nvalue`: a widget id, a control name of 1..31 characters, and a
 * value of at most 63 bytes that may be empty.
 */
[[nodiscard]] ActionResult enqueue_action(std::string_view body);

/** @return true and the oldest action, or false when none waits. */
bool pop_action(Action& action);

[[nodiscard]] bool actions_pending();

/** @brief Records a page question; `now` is monotonic milliseconds. */
void note_viewer(std::uint32_t now);

/** @return true when a page asked within kViewerWindowMilliseconds. */
[[nodiscard]] bool viewers(std::uint32_t now);

/**
 * @brief Writes the answer to "what changed after @p since?".
 *
 * `{"gen":G,"widgets":[{"id":...,"m":M,"d":D},...],"data":{"<id>":<json>,...},"rev":R}`: the
 * per-boot generation, every widget in display order with its markup and data revisions (D is
 * 0 without data), the data changed after @p since, oldest change first, and the revision to
 * ask from next. When not all of
 * it fits @p limit, `"more":true` follows and R is the newest revision included.
 */
[[nodiscard]] bool write_updates(std::uint32_t since, CommandOutput& output, std::size_t limit);

/** @return false when @p id is unknown or output fails; writes the markup as stored. */
[[nodiscard]] bool write_markup(std::string_view id, CommandOutput& output);

} // namespace widgets
} // namespace newmota
