/*
  energy_storage.cpp - Energy record validation and wire primitives

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_storage.cpp
 * @brief Energy record validation and wire primitives.
 */

#include "services/energy/energy_storage.h"
#include "support/crc32.h"
#include <cstring>

namespace newmota::energy::storage {
/** @brief Encodes integers explicitly; never persists C++ padding or machine endianness. */
void put(std::uint8_t*& cursor, std::uint64_t value, std::size_t bytes) {
    while (bytes--) {
        *cursor++ = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

std::uint64_t get(const std::uint8_t*& cursor, std::size_t bytes) {
    std::uint64_t value{};
    for (std::size_t i = 0; i < bytes; ++i)
        value |= std::uint64_t(*cursor++) << (8 * i);
    return value;
}

std::uint32_t crc(const std::uint8_t* bytes, std::size_t size) {
    return support::crc32_finalize(support::crc32_update(support::kCrc32InitialState, bytes, size));
}

/** @brief Reads the optional dated state with bounds checked before every variable record. */
bool read_calendar(const std::uint8_t*& cursor, const std::uint8_t* end, CalendarData& calendar,
    std::uint8_t& flags) {
    if (cursor == end)
        return false;
    flags = static_cast<std::uint8_t>(get(cursor, 1));
    if (flags & ~3U)
        return false;
    if (!(flags & 1))
        return true;
    if (end - cursor < 126)
        return false;
    const auto energy = [&](EnergyValue& value) {
        value = {get(cursor, 8), std::uint32_t(get(cursor, 4))};
        return value.micro_wh < 1000000;
    };
    for (auto& day : calendar.days) {
        const auto present = get(cursor, 1);
        if (present > 1)
            return false;
        day.present = present;
        day.day = static_cast<std::uint32_t>(get(cursor, 4));
        day.started_local = static_cast<std::uint32_t>(get(cursor, 4));
        if (!energy(day.energy.imported) || !energy(day.energy.exported) ||
            (day.present && day.started_local / 86400 != day.day) ||
            (!day.present && (day.day || day.started_local || day.energy.imported.wh ||
                                 day.energy.imported.micro_wh || day.energy.exported.wh ||
                                 day.energy.exported.micro_wh)))
            return false;
        if (!energy(day.compatibility.magnitude))
            return false;
        const auto negative = get(cursor, 1), seeded = get(cursor, 1);
        day.compatibility.negative = negative;
        day.seeded = seeded;
        day.seed_time = static_cast<std::uint32_t>(get(cursor, 4));
        if (negative > 1 || seeded > 1 ||
            (negative && !day.compatibility.magnitude.wh &&
                !day.compatibility.magnitude.micro_wh) ||
            (!day.present && (day.compatibility.magnitude.wh ||
                                 day.compatibility.magnitude.micro_wh || seeded || day.seed_time)))
            return false;
        day.offset_nonzero = day.compatibility.magnitude.wh || day.compatibility.magnitude.micro_wh;
    }
    if (calendar.days[1].present && (!calendar.days[0].present || calendar.days[0].day == 0 ||
                                        calendar.days[1].day != calendar.days[0].day - 1))
        return false;
    return energy(calendar.unassigned.imported) && energy(calendar.unassigned.exported);
}

/** @brief Preserves calendar records independently of whether collection is compiled. */
void put_calendar(std::uint8_t*& cursor, const CalendarData* calendar, bool fault) {
    put(cursor, (calendar ? 1 : 0) | (fault ? 2 : 0), 1);
    if (!calendar)
        return;
    const auto energy = [&](EnergyValue value) {
        put(cursor, value.wh, 8);
        put(cursor, value.micro_wh, 4);
    };
    for (const auto& day : calendar->days) {
        put(cursor, day.present, 1);
        put(cursor, day.day, 4);
        put(cursor, day.started_local, 4);
        energy(day.energy.imported);
        energy(day.energy.exported);
        energy(day.compatibility.magnitude);
        put(cursor, day.compatibility.negative, 1);
        put(cursor, day.seeded, 1);
        put(cursor, day.seed_time, 4);
    }
    energy(calendar->unassigned.imported);
    energy(calendar->unassigned.exported);
}

/** @brief Decodes the provider identity, options, calibration and optional Modbus map. */
bool read_configuration(
    const std::uint8_t*& cursor, const std::uint8_t* end, Configuration& config) {
    if (end - cursor < 50)
        return false;
    config.identity.driver = static_cast<std::uint16_t>(get(cursor, 2));
    config.identity.bus = static_cast<std::uint8_t>(get(cursor, 1));
    config.identity.address = static_cast<std::uint8_t>(get(cursor, 1));
    config.channels = static_cast<std::uint8_t>(get(cursor, 1));
    config.options.flags = static_cast<std::uint8_t>(get(cursor, 1));
    config.options.invalid_power_frames = static_cast<std::uint16_t>(get(cursor, 2));
    config.calibration.power = static_cast<std::uint32_t>(get(cursor, 4));
    config.calibration.voltage = static_cast<std::uint32_t>(get(cursor, 4));
    config.calibration.current = static_cast<std::uint32_t>(get(cursor, 4));
    config.calibration.frequency = static_cast<std::uint32_t>(get(cursor, 4));
    for (auto& binding : config.identity.pins) {
        binding.function = static_cast<PinFunction>(get(cursor, 1));
        binding.pin = PinId{static_cast<std::uint16_t>(get(cursor, 2))};
    }
    const auto profile_size = static_cast<std::size_t>(get(cursor, 2));
    if (profile_size > Configuration::kMaximumModbusProfile ||
        end - cursor < static_cast<std::ptrdiff_t>(profile_size))
        return false;
    config.modbus_profile = std::string_view(reinterpret_cast<const char*>(cursor), profile_size);
    cursor += profile_size;
    if (config.modbus_profile.find('\0') != std::string_view::npos)
        return false;
    return config.identity.valid();
}

/** @brief Reads a schedule even when collection is excluded, validating all bounded fields. */
bool read_schedule(const std::uint8_t*& cursor, const std::uint8_t* end, TariffSchedule& schedule) {
    if (end - cursor < 10)
        return false;
    for (auto& boundary : schedule.minutes)
        for (auto& minute : boundary) {
            minute = static_cast<std::uint16_t>(get(cursor, 2));
            if (minute > 1439)
                return false;
        }
    schedule.forced = static_cast<std::uint8_t>(get(cursor, 1));
    const auto weekend = get(cursor, 1);
    schedule.weekend = weekend;
    return schedule.forced <= 2 && weekend <= 1;
}

/** @brief Reads optional exact tariff totals without a selected tariff implementation. */
bool read_tariff(
    const std::uint8_t*& cursor, const std::uint8_t* end, TariffData& tariff, std::uint8_t& flags) {
    if (cursor == end)
        return false;
    flags = static_cast<std::uint8_t>(get(cursor, 1));
    if (flags > 3 || ((flags & 1) && end - cursor < 72))
        return false;
    if (!(flags & 1))
        return true;
    for (auto* bucket : {&tariff.totals[0], &tariff.totals[1], &tariff.unassigned})
        for (auto* energy : {&bucket->imported, &bucket->exported}) {
            energy->wh = static_cast<std::uint64_t>(get(cursor, 8));
            energy->micro_wh = static_cast<std::uint32_t>(get(cursor, 4));
            if (energy->micro_wh >= 1000000)
                return false;
        }
    return true;
}

/** @brief Reads the optional signed tariff projection even when collection is excluded. */
bool read_tariff_usage(const std::uint8_t*& cursor, const std::uint8_t* end, TariffUsage& values,
    std::uint8_t& flags) {
    if (cursor == end)
        return false;
    flags = static_cast<std::uint8_t>(get(cursor, 1));
    if ((flags & ~0x07U) || ((flags & 4) && !(flags & 1)) || ((flags & 1) && end - cursor < 68))
        return false;
    if (!(flags & 1))
        return true;
    for (auto* pair : {&values.usage, &values.returned})
        for (auto& value : *pair) {
            value.magnitude = {get(cursor, 8), std::uint32_t(get(cursor, 4))};
            const auto negative = get(cursor, 1);
            value.negative = negative;
            if (value.magnitude.micro_wh >= 1000000 || negative > 1 ||
                (negative && !value.magnitude.wh && !value.magnitude.micro_wh))
                return false;
        }
    values.pending_export = {get(cursor, 8), std::uint32_t(get(cursor, 4))};
    values.started_local = static_cast<std::uint32_t>(get(cursor, 4));
    values.incomplete = flags & 4;
    return values.pending_export.micro_wh < 1000000;
}

bool has_magic(const std::uint8_t* bytes, std::size_t size) {
    return size >= kHeaderSize && !std::memcmp(bytes, "NMEG", 4);
}

/** @brief Checks the complete record before any runtime allocation or resource claim. */
bool validate(const std::uint8_t* bytes, std::size_t size) {
    if (size < kHeaderSize + 4 || !has_magic(bytes, size) || bytes[4] != kFormat)
        return false;
    const auto* tail = bytes + size - 4;
    const auto* checksum = tail;
    if (get(checksum, 4) != crc(bytes, size - 4))
        return false;
    const auto* cursor = bytes + kHeaderSize;
    Configuration config{};
    TariffSchedule schedule{};
    TariffUsage tariffs{};
    std::uint8_t tariff_flags{};
    if (!read_configuration(cursor, tail, config) ||
        (config.identity.driver == 0) != (config.channels == 0) ||
        config.channels > kMaximumChannels || !read_schedule(cursor, tail, schedule) ||
        tail - cursor < 4)
        return false;
    (void)get(cursor, 4); // Total start time.
    if (!read_tariff_usage(cursor, tail, tariffs, tariff_flags) || tail - cursor < 5)
        return false;
    const auto day = get(cursor, 4), day_flags = get(cursor, 1);
    if (day_flags > 3 || day > UINT32_MAX / 86400 || (!(day_flags & 1) && day))
        return false;
    for (std::size_t c = 0; c < config.channels; ++c) {
        if (tail - cursor < 97)
            return false;
        const auto energy = [&] {
            (void)get(cursor, 8);
            return get(cursor, 4) < 1000000;
        };
        if (!energy() || !energy())
            return false;
        if (get(cursor, 2) >= 3600 || get(cursor, 2) >= 3600 || get(cursor, 4) >= 36000)
            return false;
        if (!energy() || !energy())
            return false;
        (void)get(cursor, 4);
        (void)get(cursor, 4);
        if (get(cursor, 1) > 3)
            return false;
        for (unsigned d = 0; d < 2; ++d) {
            const auto remainder = get(cursor, 4), denominator = get(cursor, 4);
            if (!denominator || remainder >= denominator)
                return false;
        }
        for (unsigned f = 0; f < 4; ++f)
            (void)get(cursor, 4); // Channel calibration.
        CalendarData calendar{};
        TariffData tariff{};
        std::uint8_t flags{};
        if (!read_calendar(cursor, tail, calendar, flags) ||
            !read_tariff(cursor, tail, tariff, flags) || tail - cursor < 105)
            return false;
        const auto signed_energy = [&](bool fault) {
            const auto wh = get(cursor, 8), micro = get(cursor, 4), negative = get(cursor, 1);
            const auto faulted = fault ? get(cursor, 1) : 0;
            return micro < 1000000 && negative <= 1 && faulted <= 1 && (!negative || wh || micro);
        };
        if (!signed_energy(true) || !signed_energy(false) || get(cursor, 2) > 32000)
            return false;
        for (unsigned m = 0; m < 6; ++m)
            if (get(cursor, 2) > (m < 2 ? 6000U : m < 4 ? 500U : 25000U))
                return false;
        for (unsigned f = 0; f < 3; ++f)
            if (get(cursor, 2) > 6000)
                return false;
        for (unsigned v = 0; v < 3; ++v) {
            const auto wh = get(cursor, 8), micro = get(cursor, 4), negative = get(cursor, 1);
            if (micro >= 1000000 || negative > 1 || (negative && !wh && !micro) ||
                (v == 1 && (wh || micro > 10009)))
                return false;
        }
        if (get(cursor, 1) > 7 || get(cursor, 2) > 6000 || get(cursor, 2) > 23)
            return false;
        const auto wh = get(cursor, 8), micro = get(cursor, 4), negative = get(cursor, 1),
                   present = get(cursor, 1);
        if (micro >= 1000000 || negative > 1 || present > 1 || (negative && !wh && !micro) ||
            (!present && (wh || micro || negative)))
            return false;
    }
    return cursor == tail;
}
} // namespace newmota::energy::storage
