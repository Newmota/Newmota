/*
  berry_port.h - Newmota Berry file-port status

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota {

enum class BerryFileOpenResult : std::uint8_t {
    opened,
    not_found,
    unavailable,
    failed,
};

/**
 * @brief Starts an exception report: only as many lines as the Berry Console history holds
 * enter it, so the traceback cannot push out the message. The log still gets every line.
 */
void begin_berry_exception_report();
/** @brief Ends the report and notes in the history when the traceback was cut short. */
void end_berry_exception_report();

/** @brief Returns the outcome of Berry's most recent file-open attempt. */
[[nodiscard]] BerryFileOpenResult berry_file_open_result();

} // namespace newmota
