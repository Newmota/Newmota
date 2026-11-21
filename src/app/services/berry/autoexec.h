/*
  autoexec.h - protected Berry startup script service for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

class BerryService;

enum class AutoexecState : std::uint8_t {
    waiting,
    executed,
    not_found,
    disabled,
    boot_loop,
    protection_unavailable,
    storage_unavailable,
    failed,
};

class AutoexecService final {
  public:
    /** @brief Binds startup-script execution to the persistent Berry VM. */
    constexpr explicit AutoexecService(BerryService& berry) : berry_(berry) {}

    /**
     * @brief Arms the startup script for this application boot.
     * @param disabled True when SetOption153 disables the next startup script.
     */
    void initialize(bool disabled);

    /** @brief Executes autoexec.be once. */
    void poll();

    /** @brief Returns the current startup-script state for diagnostics and tests. */
    [[nodiscard]] AutoexecState state() const { return state_; }

  private:
    BerryService& berry_;
    std::uint8_t fast_restart_count_{};
    AutoexecState state_{AutoexecState::waiting};
    bool disabled_{};
    bool tracking_available_{};
};

} // namespace newmota
