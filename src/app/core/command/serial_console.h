/*
  serial_console.h - serial command console interface for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "core/command/command.h"

#include <array>
#include <cstddef>

namespace newmota {

class SerialConsole final {
  public:
    /**
     * @brief Reads a bounded amount of console input and dispatches complete lines.
     * @return true when any input arrived, so an idle wait can end.
     */
    bool poll(CommandProcessor& processor);

  private:
#if USE_HARDWARE_COMMANDS
    // A maximal 64-slot Template document with bounded NAME and ARCH fields
    // fits without heap allocation. Safeboot retains the smaller console.
    static constexpr std::size_t kLineCapacity = 768;
#else
    static constexpr std::size_t kLineCapacity = 192;
#endif

    /**
     * @brief Executes or rejects the currently buffered line, then clears it.
     */
    void finish_line(CommandProcessor& processor);

    std::array<char, kLineCapacity> line_{};
    std::size_t length_{0};
    bool overflow_{false};
    bool last_was_carriage_return_{false};
};

} // namespace newmota
