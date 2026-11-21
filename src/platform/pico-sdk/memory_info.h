/*
  memory_info.h - Pico SDK stack-watermark startup hook

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstdint>

namespace newmota::platform::pico_detail {

/** @brief Paints currently free main-stack memory before application startup. */
void initialize_stack_watermark();

/** @brief Returns untouched main-stack bytes since the watermark was initialized. */
[[nodiscard]] std::uint32_t stack_watermark_bytes();

/** @brief Reports whether the main-stack watermark has been initialized. */
[[nodiscard]] bool stack_watermark_initialized();

} // namespace newmota::platform::pico_detail
