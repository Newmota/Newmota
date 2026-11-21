/*
  energy_storage.cpp - Energy record integrity and exact accounting tests

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_storage.cpp
 * @brief Exercises persistent record validation and exact arithmetic without SDK or device mocks.
 */
#include "services/energy/energy_storage.h"
#include <cassert>
#include <cstring>
#include <vector>

using namespace newmota::energy;

namespace {
/** @brief Independent fixture writer deliberately uses the frozen on-disk field widths. */
void field(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void energy(std::vector<std::uint8_t>& bytes, std::uint64_t wh, std::uint32_t fraction) {
    field(bytes, wh, 8);
    field(bytes, fraction, 4);
}

void signed_energy(
    std::vector<std::uint8_t>& bytes, std::uint64_t wh, std::uint32_t fraction, bool negative) {
    energy(bytes, wh, fraction);
    field(bytes, negative, 1);
}

/** @brief Writes one configured BL0942 provider with one channel. */
std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> bytes{'N', 'M', 'E', 'G', storage::kFormat};
    field(bytes, 42, 2); // Stable BL0942 ID, even when the driver is excluded.
    field(bytes, 0, 1);
    field(bytes, 8, 1);
    field(bytes, 1, 1);
    field(bytes, 7, 1);
    field(bytes, 128, 2);
    for (const auto coefficient : {123456U, 234567U, 345678U, 456789U})
        field(bytes, coefficient, 4);
    for (unsigned i = 0; i < 8; ++i) {
        field(bytes, i < 2 ? i + 1 : 0, 1);
        field(bytes, i < 2 ? i + 6 : 65535, 2);
    }
    field(bytes, 0, 2); // no Modbus map
    for (unsigned i = 0; i < 10; ++i)
        field(bytes, 0, 1); // tariff schedule
    field(bytes, 1700000000U, 4);
    field(bytes, 0, 1); // absent signed tariffs
    field(bytes, 20000, 4);
    field(bytes, 3, 1);
    energy(bytes, 123456789012ULL, 123456);
    energy(bytes, 9876543210ULL, 654321);
    field(bytes, 3599, 2);
    field(bytes, 3499, 2);
    field(bytes, 35999, 4);
    energy(bytes, 100, 777777);
    energy(bytes, 10, 888888);
    field(bytes, 2, 4);
    field(bytes, 3, 4);
    field(bytes, 3, 1);
    field(bytes, 2, 4);
    field(bytes, 3, 4);
    field(bytes, 4, 4);
    field(bytes, 5, 4);
    for (const auto coefficient : {456789U, 567890U, 678901U, 789012U})
        field(bytes, coefficient, 4);
    field(bytes, 0, 1);
    field(bytes, 0, 1); // calendar and tariff absent
    signed_energy(bytes, 20, 1234, true);
    field(bytes, 0, 1);
    signed_energy(bytes, 9, 8765, false);
    field(bytes, 100, 2); // power delta
    for (unsigned i = 0; i < 6; ++i)
        field(bytes, i + 1, 2);
    for (const auto value : {100U, 10U, 30U})
        field(bytes, value, 2);
    signed_energy(bytes, 17, 111111, true); // live daily value
    signed_energy(bytes, 0, 10009, true);   // exact negative sub-quantum carry
    signed_energy(bytes, 16, 222222, true); // independent restored offset
    field(bytes, 5, 1);                     // present and incomplete
    field(bytes, 600, 2);
    field(bytes, 3, 2);
    signed_energy(bytes, 25, 333333, false);
    field(bytes, 1, 1);
    field(bytes, storage::crc(bytes.data(), bytes.size()), 4);
    return bytes;
}

void checksum(std::vector<std::uint8_t>& bytes) {
    auto* cursor = bytes.data() + bytes.size() - 4;
    storage::put(cursor, storage::crc(bytes.data(), bytes.size() - 4), 4);
}
} // namespace

int main() {
    auto record = fixture();
    assert(storage::validate(record.data(), record.size()));
    const auto* cursor = record.data() + storage::kHeaderSize;
    Configuration config{};
    assert(storage::read_configuration(cursor, record.data() + record.size() - 4, config));
    assert(config.identity.driver == 42 && config.identity.address == 8 && config.channels == 1);
    assert(config.options.flags == 7 && config.options.invalid_power_frames == 128);
    assert(config.calibration.power == 123456 && config.calibration.frequency == 456789);
    assert(config.identity.pin(PinFunction::uart_rx).value() == 6);
    assert(config.identity.pin(PinFunction::uart_tx).value() == 7);
    const auto* account = record.data() + storage::kHeaderSize + 50 + 10 + 4 + 1 + 5;
    assert(storage::get(account, 8) == 123456789012ULL);
    assert(storage::get(account, 4) == 123456);
    for (std::size_t size = 0; size < record.size(); ++size)
        assert(!storage::validate(record.data(), size));
    record.back() ^= 1;
    assert(!storage::validate(record.data(), record.size()));
    record = fixture();
    record[4] = storage::kFormat - 1;
    checksum(record);
    assert(storage::has_magic(record.data(), record.size()));
    assert(!storage::validate(record.data(), record.size()));
    record = fixture();
    auto* fraction = record.data() + storage::kHeaderSize + 50 + 10 + 4 + 1 + 5 + 8;
    storage::put(fraction, 1000000, 4);
    checksum(record);
    assert(!storage::validate(record.data(), record.size()));
    // Exercise the real fractional arithmetic used after restore, including overflow rollback.
    Account exact{};
    assert(exact.add_pulses(1, 1));
    assert(exact.imported.wh == 0 && exact.imported.micro_wh == 0 && exact.pulse_remainder == 1);
    assert(exact.add_pulses(35999, 1));
    assert(exact.imported.micro_wh == 1 && exact.pulse_remainder == 0);
    exact.imported = {UINT64_MAX, 999999};
    const auto before = exact.imported;
    assert(!exact.imported.add({0, 1}) && exact.imported == before);
    SignedEnergy signed_total{{2, 1}, true};
    assert(signed_total.add({1, 2}) && signed_total.negative && signed_total.magnitude.wh == 0 &&
           signed_total.magnitude.micro_wh == 999999);
}
