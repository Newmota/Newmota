/*
  crypto_random.cpp - RP2350 randomness

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/crypto_random.h"

#include "pico.h"
#include "pico/rand.h"

#include <cstdint>

namespace newmota::platform {
CryptoRandomResult crypto_random(void* const destination, const std::size_t size) {
    if (size == 0U)
        return CryptoRandomResult::success;
    if (destination == nullptr)
        return CryptoRandomResult::invalid_request;
    auto* const output = static_cast<volatile unsigned char*>(destination);
#if PICO_RP2350
    // The platform's own generator, which hashes hardware TRNG samples into its state.
    std::size_t offset = 0;
    while (offset < size) {
        const std::uint64_t value = get_rand_64();
        for (unsigned index = 0; index < sizeof(value) && offset < size; ++index)
            output[offset++] = static_cast<unsigned char>(value >> (index * 8U));
    }
    return CryptoRandomResult::success;
#else
    for (std::size_t index = 0; index < size; ++index)
        output[index] = 0;
    return CryptoRandomResult::unavailable;
#endif
}
} // namespace newmota::platform
