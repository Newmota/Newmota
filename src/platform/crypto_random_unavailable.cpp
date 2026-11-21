/*
  crypto_random_unavailable.cpp - Unavailable cryptographic randomness adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file crypto_random_unavailable.cpp
 * @brief Unavailable cryptographic randomness adapter.
 */

#include "newmota/platform/crypto_random.h"

namespace newmota::platform {
CryptoRandomResult crypto_random(void* destination, std::size_t size) {
    if (!size)
        return CryptoRandomResult::success;
    if (!destination)
        return CryptoRandomResult::invalid_request;
    auto* p = static_cast<volatile unsigned char*>(destination);
    for (std::size_t i = 0; i < size; ++i)
        p[i] = 0;
    return CryptoRandomResult::unavailable;
}
} // namespace newmota::platform
