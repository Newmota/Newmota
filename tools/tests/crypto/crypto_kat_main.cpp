/*
  crypto_kat_main.cpp - runner for the BearSSL host known-answer tests

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "crypto_kat.h"

#include <cstdio>

int crypto_hash_kat();
int crypto_sym_kat();
int crypto_ec_kat();
int crypto_extension_kat();

namespace {

struct Suite {
    const char* name;
    int (*run)();
};

} // namespace

int main() {
    const Suite suites[] = {
        {"hash, MAC, and KDF", &crypto_hash_kat},
        {"symmetric cipher and AEAD", &crypto_sym_kat},
        {"elliptic curve", &crypto_ec_kat},
        {"local extensions", &crypto_extension_kat},
    };
    int failures = 0;
    for (const Suite& suite : suites) {
        const int suite_failures = suite.run();
        failures += suite_failures;
        if (suite_failures == 0) {
            std::printf("BearSSL %s: all expectations passed\n", suite.name);
        }
    }
    if (failures == 0) {
        std::puts("BearSSL known-answer tests passed");
    }
    return failures == 0 ? 0 : 1;
}
