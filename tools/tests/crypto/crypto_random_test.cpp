/* Copyright (C) 2026 Newmota contributors. SPDX-License-Identifier: GPL-3.0-or-later */
#include "newmota/platform/crypto_random.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
bool running{};
unsigned fills{};
} // namespace

namespace newmota::platform {
bool wifi_driver_running() {
    return running;
}
} // namespace newmota::platform

void esp_fill_random(void* destination, std::size_t size) {
    ++fills;
    std::memset(destination, 0xa5, size);
}

int main() {
    using namespace newmota::platform;
    std::array<unsigned char, 32> output;
    output.fill(0xff);
    assert(crypto_random(nullptr, 0) == CryptoRandomResult::success);
    assert(crypto_random(nullptr, 1) == CryptoRandomResult::invalid_request);
    assert(crypto_random(output.data(), output.size()) == CryptoRandomResult::unavailable);
    for (auto value : output)
        assert(value == 0);
    assert(fills == 0);
    running = true;
    assert(crypto_random(output.data(), output.size()) == CryptoRandomResult::success);
    for (auto value : output)
        assert(value == 0xa5);
    assert(fills == 1);
    running = false;
    assert(crypto_random(output.data(), output.size()) == CryptoRandomResult::unavailable);
    for (auto value : output)
        assert(value == 0);
    assert(fills == 1);
    std::puts("ESP-IDF entropy adapter lifecycle fixture passed");
}
