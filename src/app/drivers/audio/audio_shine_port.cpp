/*
  audio_shine_port.cpp - Build the carried Shine encoder with portable allocation

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_shine_port.cpp
 * @brief Keeps the carried implementation unchanged while reserving all core memory first.
 * The carried initializer dereferences unchecked suballocations. Its destructor also
 * dereferences l3loop after freeing it. A single arena and an owned destructor repair
 * those contracts without modifying the borrowed encoder algorithm.
 */
#include "types.h"
#include "layer3.h"
#include <cstddef>
#include <cstdint>
#include <cstdint>
#include <cstdlib>

namespace {
std::uint8_t* arena{};
std::size_t position{};
constexpr std::size_t kArenaAlignment = alignof(std::max_align_t);
// Shine's initializer allocates eight granule arrays and two loop arrays.
constexpr std::size_t kGranuleArrayCount = 10;
// The twelve allocations are the config, loop state, and those ten arrays.
constexpr std::size_t kAlignedAllocationCount = 12;
constexpr std::size_t kArenaBytes = sizeof(shine_global_config) + sizeof(l3loop_t) +
                                    kGranuleArrayCount * GRANULE_SIZE * sizeof(int) +
                                    kAlignedAllocationCount * kArenaAlignment;

/** @brief Individual core allocations live until the owning arena is released. */
void discard(void*) {}
} // namespace

/** @brief Carves one aligned allocation from the preallocated initialization arena. */
void* newmota_shine_allocate(std::size_t size) {
    const auto aligned = (position + kArenaAlignment - 1U) / kArenaAlignment * kArenaAlignment;
    if (!arena || aligned > kArenaBytes || size > kArenaBytes - aligned)
        return nullptr;
    auto* block = arena + aligned;
    position = aligned + size;
    return block;
}
#ifndef ESP32
#define ESP32
#endif
#define shine_initialise shine_initialise_carried
#define shine_close shine_close_carried
#define free discard
#include "layer3.cpp"
#undef free
#undef shine_close
#undef shine_initialise

shine_t shine_initialise(shine_config_t* config) {
    if (arena || shine_check_config(config->wave.samplerate, config->mpeg.bitr) < 0)
        return nullptr;
    arena = static_cast<std::uint8_t*>(std::calloc(1, kArenaBytes));
    if (!arena)
        return nullptr;
    position = 0;
    auto* allocation = arena;
    auto* opened = shine_initialise_carried(config);
    arena = nullptr;
    if (!opened) {
        std::free(allocation);
        return nullptr;
    }
    if (!opened->bs.data) {
        std::free(allocation);
        return nullptr;
    }
    return opened;
}

void shine_close(shine_t config) {
    if (config) {
        std::free(config->bs.data);
        std::free(config);
    }
}
