/*
  ble_cache.h - Per-link GATT discovery cache shared by the BLE backends

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file ble_cache.h
 * @brief Per-link cache of discovered services and characteristics.
 *
 * A backend keeps one Cache for its client link and a generation counter that its link events
 * and Service Changed indications advance. Only the backend's job context reads or writes the
 * cache; event callbacks only advance the counter. Entries become usable once the peer is known
 * to keep its database fixed or to indicate changes to us (see Watch). The first job on a link
 * fills the cache unchecked, so one-shot jobs cost no extra round trips.
 */

#pragma once
#include "newmota/platform/ble.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace newmota::platform::ble_cache {

constexpr std::size_t kServices = 4;
constexpr std::size_t kCharacteristics = 8;
constexpr std::uint16_t kServiceChangedUuid = 0x2A05;

/** @brief Whether cached handles may be used on the current link. */
enum class Trust : std::uint8_t { unknown, trusted, untrusted };

/** @brief What watching the peer's database says about the entries already cached. */
enum class Watch : std::uint8_t {
    valid,      ///< The database is fixed (no Service Changed), or was already watched.
    rediscover, ///< Watched from now on; a change before that went unannounced.
    failed,     ///< Changes cannot be observed on this link.
};

/** @brief Handle range of a primary service. */
struct Range {
    std::uint16_t start, end;
};

/** @brief Discovery results for one link.
 *  @tparam Characteristic What the backend needs to reuse a characteristic without discovery.
 */
template <typename Characteristic> struct Cache {
    struct Service {
        std::uint8_t uuid[16];
        Range range;
    };

    struct Entry {
        std::uint8_t uuid[16];
        Characteristic value;
        std::uint8_t service; ///< Index into services.
    };

    Service services[kServices];
    Entry characteristics[kCharacteristics];
    std::uint32_t generation;
    std::uint8_t service_count, characteristic_count;
    std::uint8_t next; ///< Characteristic replaced next once the table is full.
    Trust trust;

    /** @brief Empties the cache when @p current moved past the generation it was filled in,
     *  or on request.
     */
    void forget(std::uint32_t current, bool always = false) {
        if (always || generation != current) {
            *this = Cache{};
            generation = current;
        }
    }

    /** @brief Whether entries wait for the once-per-link check. */
    [[nodiscard]] bool unchecked() const { return service_count != 0 && trust == Trust::unknown; }

    /** @brief Applies the check's outcome. Call forget() with the current generation first, so a
     *  change during the check leaves an empty, unchecked cache.
     */
    void settle(Watch watch) {
        if (!unchecked())
            return;
        if (watch != Watch::valid)
            clear(); // entries found before the watch may have missed a change
        trust = watch == Watch::failed ? Trust::untrusted : Trust::trusted;
    }

    /** @return The service's index, or -1. */
    [[nodiscard]] int find_service(const std::uint8_t* uuid) const {
        for (std::uint8_t i = 0; i < service_count; ++i)
            if (std::memcmp(services[i].uuid, uuid, 16) == 0)
                return i;
        return -1;
    }

    /** @brief Stores a discovered service. Its generation stays the one the lookup began in, so
     *  a change announced during the discovery drops it on the next lookup.
     *  @return Its index. Without trust it is kept for the current job only and never matched.
     */
    int add_service(const std::uint8_t* uuid, Range range) {
        if (trust == Trust::untrusted) {
            services[0] = {{}, range};
            return 0;
        }
        if (service_count == kServices) {
            const Trust kept = trust;
            clear(); // characteristics refer to services by index
            trust = kept;
        }
        auto& service = services[service_count];
        std::memcpy(service.uuid, uuid, 16);
        service.range = range;
        return service_count++;
    }

    [[nodiscard]] const Characteristic* find_characteristic(
        int service, const std::uint8_t* uuid) const {
        for (std::uint8_t i = 0; i < characteristic_count; ++i)
            if (characteristics[i].service == service &&
                std::memcmp(characteristics[i].uuid, uuid, 16) == 0)
                return &characteristics[i].value;
        return nullptr;
    }

    void add_characteristic(int service, const std::uint8_t* uuid, const Characteristic& value) {
        if (trust == Trust::untrusted)
            return;
        std::uint8_t slot = characteristic_count;
        if (slot < kCharacteristics) {
            ++characteristic_count;
        } else {
            slot = next;
            next = static_cast<std::uint8_t>((slot + 1) % kCharacteristics);
        }
        auto& entry = characteristics[slot];
        std::memcpy(entry.uuid, uuid, 16);
        entry.value = value;
        entry.service = static_cast<std::uint8_t>(service);
    }

  private:
    void clear() {
        const auto kept = generation;
        *this = Cache{};
        generation = kept;
    }
};

/** @brief Key of a watched characteristic; handles are scoped to their connection. */
[[nodiscard]] constexpr std::uint32_t watch_key(std::uint16_t connection, std::uint16_t handle) {
    return (static_cast<std::uint32_t>(connection) << 16) | handle;
}

/** @brief Repeats a lookup until no change was announced during it, at most three times.
 *  @param generation Reads the backend's counter.
 *  @return The lookup's result, or no_characteristic when every successful attempt was overtaken.
 */
template <typename Generation, typename Lookup>
[[nodiscard]] BleError settled(Generation generation, Lookup lookup) {
    BleError error = BleError::none;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uint32_t before = generation();
        error = lookup();
        if (generation() == before)
            return error;
    }
    return error == BleError::none ? BleError::no_characteristic : error;
}
} // namespace newmota::platform::ble_cache
