/*
  widgets.h - script-defined dashboard widgets

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file widgets.h
 * @brief Registry of dashboard widgets, each kept as its latest validated JSON object.
 *
 * Producers (the Berry `widget` module) replace a widget by id; the Home page reads a small
 * index of ids and revisions and fetches only the widgets whose revision changed. Content is
 * copied in, so no producer object outlives its owner here. Control actions from the page wait
 * in a bounded queue until the producer's loop takes them.
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
/** @brief Largest stored object; the widget route adds id and revision within 4 KiB. */
inline constexpr std::size_t kMaximumBytes = 3968;
inline constexpr std::size_t kActionCapacity = 8;
inline constexpr std::size_t kPartCapacity = 31;
inline constexpr std::size_t kValueCapacity = 63;
/** @brief A page fetch of the index within this window counts as a viewer. */
inline constexpr std::uint32_t kViewerWindowMilliseconds = 3000;

enum class SetResult : std::uint8_t {
    ok,
    invalid,
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
    std::array<char, kPartCapacity + 1> part{};
    std::array<char, kValueCapacity + 1> value{};
};

/**
 * @brief Creates or replaces a widget and gives it a new revision.
 * @param id 1..kIdCapacity characters; only its length is checked.
 * @param json A JSON object of at most kMaximumBytes with at least one member; its content is
 * not checked.
 * @param order Display order; equal orders keep first-registration order.
 */
[[nodiscard]] SetResult set(std::string_view id, std::string_view json, std::int32_t order);

/** @return The fixed slot of @p id in 0..kCapacity-1, or -1 when it is unknown. */
[[nodiscard]] int slot(std::string_view id);

/** @return true when a widget with @p id existed; its queued actions are dropped too. */
bool remove(std::string_view id);

/** @brief Drops every widget and action, for example when the producing VM goes away. */
void clear();

/**
 * @brief Queues a control action from the page for an existing widget.
 * @param body `id\npart\nvalue`: a widget id, a part name or index of 1..31 characters, and
 * a value of at most 63 bytes that may be empty.
 */
[[nodiscard]] ActionResult enqueue_action(std::string_view body);

/** @return true and the oldest action, or false when none waits. */
bool pop_action(Action& action);

[[nodiscard]] bool actions_pending();

/** @brief Records a page fetch of the index; `now` is monotonic milliseconds. */
void note_viewer(std::uint32_t now);

/** @return true when a page fetched the index within kViewerWindowMilliseconds. */
[[nodiscard]] bool viewers(std::uint32_t now);

/** @brief Writes `{"widgets":[{"id":...,"rev":...},...]}` in display order. */
[[nodiscard]] bool write_index(CommandOutput& output);

/**
 * @brief Writes one widget as its stored object with `id` and `rev` added.
 * @return false when the id is unknown or output fails.
 */
[[nodiscard]] bool write_widget(std::string_view id, CommandOutput& output);

} // namespace widgets
} // namespace newmota
