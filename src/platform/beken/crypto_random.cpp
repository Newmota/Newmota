/*
  crypto_random.cpp - Beken cryptographic randomness adapter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file crypto_random.cpp
 * @brief Beken cryptographic randomness adapter.
 */

#include "newmota/platform/crypto_random.h"

#include <climits>

extern "C" {
// BkDriverRng.h needs the legacy OSStatus header first; OSStatus is int.
// BK7231N and BK7238 read the hardware TRNG here. bk_rand() is not used
// because it masks each word with RAND_MAX and loses the top bit.
int BkRandomNumberRead(void* inBuffer, int inByteCount);
}

namespace newmota::platform {
CryptoRandomResult crypto_random(void* const destination, const std::size_t size) {
    if (size == 0U)
        return CryptoRandomResult::success;
    if (destination == nullptr)
        return CryptoRandomResult::invalid_request;
    if (size > static_cast<std::size_t>(INT_MAX) ||
        BkRandomNumberRead(destination, static_cast<int>(size)) != 0) {
        auto* const output = static_cast<volatile unsigned char*>(destination);
        for (std::size_t index = 0; index < size; ++index)
            output[index] = 0;
        return CryptoRandomResult::failed;
    }
    return CryptoRandomResult::success;
}
} // namespace newmota::platform
