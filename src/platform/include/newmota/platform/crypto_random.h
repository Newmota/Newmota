/*
  crypto_random.h - Platform cryptographic randomness contract

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file crypto_random.h
 * @brief Platform cryptographic randomness contract.
 */

#pragma once
#include <cstddef>

namespace newmota::platform {
enum class CryptoRandomResult { success, unavailable, invalid_request, failed };
/** @brief Fills a buffer with platform entropy without a deterministic fallback.
 *
 * Empty requests succeed. On failure, every byte of a valid destination is
 * cleared before returning.
 * @param destination Buffer to fill, or null only when @p size is zero.
 * @param size Number of bytes to generate.
 * @return Whether entropy was obtained or why the request failed.
 */
[[nodiscard]] CryptoRandomResult crypto_random(void* destination, std::size_t size);
} // namespace newmota::platform
