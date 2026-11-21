/*
  boot_counter.h - persistent application boot counter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

struct BootStatus final {
    std::uint32_t count{};
    bool available{};
};

/**
 * @brief Owns the persistent count of normal application starts.
 *
 * The counter is separate from fast-restart protection and from the settings
 * record generation.  A failed or unavailable filesystem leaves the status
 * unavailable rather than inventing a value.
 */
class BootCounter final {
  public:
    /**
     * @brief Loads, increments, and atomically stores the application count.
     * @param increment false only loads the count, as Tasmota does while deep sleep is enabled
     *        without SetOption76.
     * @return true when the count is persistent and available.
     */
    [[nodiscard]] bool initialize(bool increment = true);
    /**
     * @brief Clears the persistent count before a deliberate restart.
     * @return true when zero was persisted.
     */
    [[nodiscard]] bool reset();

    /** @brief Returns the current count and availability state. */
    [[nodiscard]] BootStatus status() const { return {count_, available_}; }

  private:
    std::uint32_t count_{};
    bool available_{};
};

} // namespace newmota
