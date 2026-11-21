/*
  rtu_configuration.cpp - Durable Modbus physical bus configuration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtu_configuration.cpp
 * @brief Verified fixed-size bus storage, recovery and native restoration.
 */
#include "drivers/modbus/rtu_service.h"
#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include <array>
#include <cstring>

namespace newmota::modbus {
// Native lifecycle stays in the transport; this layer commits configuration intent.
Result open_bus(std::uint8_t, const BusConfiguration&);
Result close_native_bus(std::uint8_t);

namespace {
struct Retained {
    BusConfiguration configuration{};
    bool enabled{};
};

std::array<Retained, kMaximumBuses> retained{};
bool initialized{}, storage_fault{};
constexpr const char* path = "/.newmota/modbus.bin";
constexpr const char* previous = "/.newmota/modbus.prev";
constexpr const char* temporary = "/.newmota/modbus.tmp";
// NMRB, version, slot count, four fixed 16-byte records, CRC16 little endian.
using File = std::array<std::uint8_t, 6 + kMaximumBuses * 16 + 2>;

void put(std::uint8_t*& p, std::uint32_t value, unsigned count) {
    while (count--) {
        *p++ = value & 255;
        value >>= 8;
    }
}

std::uint32_t get(const std::uint8_t*& p, unsigned count) {
    std::uint32_t value{};
    for (unsigned i = 0; i < count; ++i)
        value |= std::uint32_t(*p++) << (8 * i);
    return value;
}

/** @brief Storage accepts only the portable RTU framing contract and distinct assigned pads. */
bool valid(const BusConfiguration& c) {
    if (!c.uart.rx.valid() || !c.uart.tx.valid() || c.uart.data_bits != 8 || c.uart.inverted ||
        c.uart.rx_capacity != platform::serial::kCapacity || c.uart.baud < 300 ||
        c.uart.baud > 115200 || unsigned(c.uart.parity) > 2 || c.uart.stop_bits < 1 ||
        c.uart.stop_bits > 2)
        return false;
    const PinId pins[]{c.uart.rx, c.uart.tx, c.transmit_enable, c.receive_enable};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (pins[i].valid() && pins[i].value() == pins[j].value())
                return false;
    return true;
}

File encode() {
    File file{};
    auto* p = file.data();
    std::memcpy(p, "NMRB", 4);
    p += 4;
    *p++ = 1;
    *p++ = kMaximumBuses;
    for (const auto& slot : retained) {
        const auto& c = slot.configuration;
        put(p, c.uart.rx.value(), 2);
        put(p, c.uart.tx.value(), 2);
        put(p, c.transmit_enable.value(), 2);
        put(p, c.receive_enable.value(), 2);
        put(p, c.uart.baud, 4);
        put(p, unsigned(c.uart.parity), 1);
        put(p, c.uart.stop_bits, 1);
        put(p, c.receive_active_level, 1);
        put(p, slot.enabled, 1);
    }
    put(p, crc16(file.data(), file.size() - 2), 2);
    return file;
}

bool decode(const File& file, std::size_t size, std::array<Retained, kMaximumBuses>& slots) {
    if (size != file.size() || std::memcmp(file.data(), "NMRB", 4) || file[4] != 1 ||
        file[5] != kMaximumBuses || crc16(file.data(), file.size()) != 0)
        return false;
    const auto* p = file.data() + 6;
    for (auto& slot : slots) {
        auto& c = slot.configuration;
        c.uart.rx = PinId{std::uint16_t(get(p, 2))};
        c.uart.tx = PinId{std::uint16_t(get(p, 2))};
        c.transmit_enable = PinId{std::uint16_t(get(p, 2))};
        c.receive_enable = PinId{std::uint16_t(get(p, 2))};
        c.uart.baud = get(p, 4);
        c.uart.parity = platform::serial::Parity(get(p, 1));
        c.uart.stop_bits = get(p, 1);
        const auto level = get(p, 1), enabled = get(p, 1);
        if (level > 1 || enabled > 1)
            return false;
        c.receive_active_level = level;
        slot.enabled = enabled;
        if (slot.enabled && !valid(c))
            return false;
    }
    return true;
}

/** @brief Synchronizes and verifies a replacement before rotating the last valid active copy. */
bool save() {
    if (storage_fault)
        return false;
    const auto file = encode();
    File verify{};
    std::array<Retained, kMaximumBuses> decoded{};
    std::size_t size{};
    using M = filesystem::MutationResult;
    using R = filesystem::ReadResult;
    if (filesystem::ensure_directory("/.newmota") != M::succeeded ||
        filesystem::write_file(temporary, file.data(), file.size()) != M::succeeded ||
        filesystem::read_file(temporary, verify.data(), verify.size(), size) != R::succeeded ||
        verify != file || !decode(verify, size, decoded))
        return false;
    const auto active = filesystem::read_file(path, verify.data(), verify.size(), size);
    if (active == R::succeeded && decode(verify, size, decoded)) {
        if (filesystem::remove_if_exists(previous) != M::succeeded ||
            filesystem::rename(path, previous) != M::succeeded)
            return false;
    } else if (active != R::not_found && filesystem::remove_if_exists(path) != M::succeeded)
        return false;
    return filesystem::rename(temporary, path) == M::succeeded;
}
} // namespace

void initialize() {
    if (initialized)
        return;
    initialized = true;
    File file{};
    std::size_t size{};
    using R = filesystem::ReadResult;
    const auto active = filesystem::read_file(path, file.data(), file.size(), size);
    bool loaded = active == R::succeeded && decode(file, size, retained);
    if (!loaded) {
        std::array<Retained, kMaximumBuses> recovered{};
        const auto backup = filesystem::read_file(previous, file.data(), file.size(), size);
        loaded = backup == R::succeeded && decode(file, size, recovered);
        if (loaded)
            retained = recovered;
        else {
            retained = {};
            storage_fault = !(active == R::not_found && backup == R::not_found);
        }
    }
    if (storage_fault)
        AddLog(LogLevel::error, "MBS: invalid or unreadable bus storage; writes blocked");
    if (loaded)
        for (std::size_t i = 0; i < retained.size(); ++i)
            if (retained[i].enabled) {
                const auto result = open_bus(i, retained[i].configuration);
                if (result != Result::success)
                    AddLog(LogLevel::error, "MBS: bus %u restore failed (%u)", unsigned(i + 1),
                        unsigned(result));
            }
}

bool storage_available() {
    return initialized && !storage_fault;
}

bool configuration(std::uint8_t index, BusConfiguration& output) {
    if (index >= retained.size() || !retained[index].enabled)
        return false;
    output = retained[index].configuration;
    return true;
}

Result configure_bus(std::uint8_t index, const BusConfiguration& config) {
    if (!initialized || storage_fault)
        return Result::storage_error;
    if (index >= retained.size() || !valid(config))
        return Result::invalid_argument;
    const auto old = retained[index];
    if (old.enabled) {
        const auto& c = old.configuration;
        if (c.uart.rx.value() != config.uart.rx.value() ||
            c.uart.tx.value() != config.uart.tx.value() ||
            c.transmit_enable.value() != config.transmit_enable.value() ||
            c.receive_enable.value() != config.receive_enable.value() ||
            c.receive_active_level != config.receive_active_level ||
            c.uart.baud != config.uart.baud || c.uart.parity != config.uart.parity ||
            c.uart.stop_bits != config.uart.stop_bits)
            return Result::busy;
        return open_bus(
            index, config); // Durable intent already exists; retry a failed restoration.
    }
    const auto opened = open_bus(index, config);
    if (opened != Result::success)
        return opened;
    retained[index] = {config, true};
    if (save())
        return Result::success;
    retained[index] = old;
    // Do not tear down an existing bus/clients when an identical reapply cannot save.
    if (!old.enabled)
        (void)close_native_bus(index);
    return Result::storage_error;
}

Result close_bus(std::uint8_t index) {
    if (!initialized || storage_fault)
        return Result::storage_error;
    if (index >= retained.size())
        return Result::invalid_argument;
    const auto result = close_native_bus(index);
    if (result != Result::success)
        return result;
    const auto old = retained[index];
    retained[index] = {};
    if (save())
        return Result::success;
    retained[index] = old;
    if (old.enabled)
        (void)open_bus(index, old.configuration);
    return Result::storage_error;
}
} // namespace newmota::modbus
