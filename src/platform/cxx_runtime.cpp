/*
  cxx_runtime.cpp - Non-throwing C++ runtime helpers

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file cxx_runtime.cpp
 * @brief Replaces libstdc++'s throwing helpers on families without a nano libstdc++.
 *
 * Newmota builds without exceptions, but checked library calls still reference the
 * out-of-line `std::__throw_*` helpers. libstdc++ defines all of them in one object
 * that raises real exceptions, which links the unwinder, the personality routine and,
 * through the verbose terminate handler, the demangler. Every helper is defined here
 * so that object is never pulled; a partial set would collide with it.
 */

#include <bits/functexcept.h>
#include <cstdlib>
#include <new>

namespace std {
void __throw_bad_exception() {
    std::abort();
}

void __throw_bad_alloc() {
    std::abort();
}

void __throw_bad_array_new_length() {
    std::abort();
}

void __throw_bad_cast() {
    std::abort();
}

void __throw_bad_typeid() {
    std::abort();
}

void __throw_logic_error(const char*) {
    std::abort();
}

void __throw_domain_error(const char*) {
    std::abort();
}

void __throw_invalid_argument(const char*) {
    std::abort();
}

void __throw_length_error(const char*) {
    std::abort();
}

void __throw_out_of_range(const char*) {
    std::abort();
}

void __throw_out_of_range_fmt(const char*, ...) {
    std::abort();
}

void __throw_runtime_error(const char*) {
    std::abort();
}

void __throw_range_error(const char*) {
    std::abort();
}

void __throw_overflow_error(const char*) {
    std::abort();
}

void __throw_underflow_error(const char*) {
    std::abort();
}
} // namespace std

namespace __gnu_cxx {
/** @brief Default terminate handler; libstdc++'s prints a demangled exception name. */
void __verbose_terminate_handler() {
    std::abort();
}
} // namespace __gnu_cxx

// libstdc++ implements the nothrow forms as a try/catch around the throwing form,
// which needs the personality routine and turns allocation failure into an abort
// once the helpers above no longer throw. Allocate directly instead.
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1U);
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    return std::malloc(size ? size : 1U);
}
