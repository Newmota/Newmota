/*
  cxx_guard.cpp - Non-throwing static initialization guards

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file cxx_guard.cpp
 * @brief Function-local static guards for toolchains whose libstdc++ guard throws.
 *
 * The Bouffalo and AmebaD libstdc++ guards run single-threaded (AmebaD's is built
 * that way; Bouffalo's only locks through weak pthread symbols nothing defines) and
 * throw `recursive_init_error`, which links the exception runtime. This keeps the
 * same semantics and aborts instead. Byte 0 marks completion: the generic C++ ABI
 * tests that byte and the Arm ABI tests bit 0 of the word, which on these
 * little-endian targets is the same bit. Byte 1 marks an initialization in progress.
 */

#include <cstdlib>

extern "C" int __cxa_guard_acquire(void* const guard) {
    auto* const bytes = static_cast<unsigned char*>(guard);
    if (bytes[0] != 0U)
        return 0;
    if (bytes[1] != 0U)
        std::abort(); // Recursive initialization of the same static.
    bytes[1] = 1U;
    return 1;
}

extern "C" void __cxa_guard_release(void* const guard) {
    auto* const bytes = static_cast<unsigned char*>(guard);
    bytes[1] = 0U;
    bytes[0] = 1U;
}

extern "C" void __cxa_guard_abort(void* const guard) {
    static_cast<unsigned char*>(guard)[1] = 0U;
}
