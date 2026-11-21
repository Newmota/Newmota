/*
  wire_module.cpp - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/berry/modules/wire/wire_module.h"
#include "newmota/platform/i2c.h"
#include "services/berry/berry_service.h"
extern "C" {
#include "berry.h"
}
#include <array>
#include <cstring>

namespace {
namespace i2c = newmota::platform::i2c;

struct Transaction {
    std::array<std::uint8_t, i2c::kMaxTransfer> tx{}, rx{};
    std::size_t tx_size{}, rx_size{}, position{};
    std::uint8_t address{};
    bool started{}, pending{};
};

std::array<Transaction, i2c::kBusCount> transactions{};

/** @brief Validates a bounded Berry integer argument. */
int integer(bvm* vm, int index, int low, int high) {
    if (!be_isint(vm, index))
        be_raise(vm, "type_error", "Wire expects integer arguments");
    const auto value = be_toint(vm, index);
    if (value < low || value > high)
        be_raise(vm, "value_error", "Wire argument out of range");
    return static_cast<int>(value);
}

/** @brief Resolves a Wire instance to its zero-based retained bus. */
std::uint8_t bus(bvm* vm) {
    be_getmember(vm, 1, "bus");
    const auto result = integer(vm, -1, 1, i2c::kBusCount) - 1;
    be_pop(vm, 1);
    return static_cast<std::uint8_t>(result);
}

/** @brief Initializes a Wire instance with its one-based bus number. */
int Init(bvm* vm) {
    const auto index = be_top(vm) >= 2 ? integer(vm, 2, 1, i2c::kBusCount) : 1;
    be_pushint(vm, index);
    be_setmember(vm, 1, "bus");
    be_pop(vm, 1);
    be_return_nil(vm);
}

/** @brief Reports runtime availability of the configured bus. */
int Enabled(bvm* vm) {
    be_pushbool(vm, i2c::available(bus(vm)));
    be_return(vm);
}

/** @brief Probes a device address through the shared bus owner. */
int Detect(bvm* vm) {
    const auto index = bus(vm);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    be_pushbool(vm, i2c::transfer(index, address, nullptr, 0, nullptr, 0) == i2c::Result::success);
    be_return(vm);
}

/** @brief Lists responding addresses while observing the Berry execution deadline. */
int Scan(bvm* vm) {
    const auto index = bus(vm);
    be_newobject(vm, "list");
    if (i2c::available(index))
        for (std::uint8_t address = 1; address < 128; ++address) {
            newmota::check_berry_execution_deadline(vm);
            if (i2c::transfer(index, address, nullptr, 0, nullptr, 0) == i2c::Result::success) {
                be_pushint(vm, address);
                be_data_push(vm, -2);
                be_pop(vm, 1);
            }
        }
    be_pop(vm, 1);
    be_return(vm);
}

/** @brief Starts a bounded script-side staged transmission. */
int Begin(bvm* vm) {
    auto& t = transactions[bus(vm)];
    t = {};
    t.address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    t.started = true;
    be_return_nil(vm);
}

/** @brief Appends integer, string or byte data to the staged transmission. */
int Write(bvm* vm) {
    auto& t = transactions[bus(vm)];
    if (!t.started || t.pending)
        be_raise(vm, "value_error", "Wire transmission not started");
    std::uint8_t byte{};
    const void* data{};
    std::size_t size{};
    if (be_isint(vm, 2)) {
        byte = static_cast<std::uint8_t>(integer(vm, 2, 0, 255));
        data = &byte;
        size = 1;
    } else if (be_isstring(vm, 2)) {
        data = be_tostring(vm, 2);
        size = static_cast<std::size_t>(be_strlen(vm, 2));
    } else
        data = be_tobytes(vm, 2, &size);
    if (!data)
        be_raise(vm, "type_error", "Wire write expects integer, string or bytes");
    if (size > t.tx.size() - t.tx_size)
        be_raise(vm, "value_error", "Wire transfer capacity exceeded");
    std::memcpy(t.tx.data() + t.tx_size, data, size);
    t.tx_size += size;
    be_pushint(vm, static_cast<bint>(size));
    be_return(vm);
}

/** @brief Maps portable errors to familiar Wire status codes. */
int status(i2c::Result result) {
    if (result == i2c::Result::success)
        return 0;
    if (result == i2c::Result::invalid_argument)
        return 1;
    if (result == i2c::Result::nack)
        return 2;
    if (result == i2c::Result::timeout)
        return 5;
    return 4;
}

/** @brief Completes a write or stages a following combined transfer. */
int End(bvm* vm) {
    const auto index = bus(vm);
    auto& t = transactions[index];
    bool stop = true;
    if (be_top(vm) > 1) {
        if (!be_isbool(vm, 2))
            be_raise(vm, "type_error", "Wire stop expects bool");
        stop = be_tobool(vm, 2);
    }
    if (!t.started) {
        be_pushint(vm, 4);
        be_return(vm);
    }
    if (!stop) {
        t.pending = true;
        be_pushint(vm, 0);
        be_return(vm);
    }
    const auto result = i2c::transfer(index, t.address, t.tx.data(), t.tx_size, nullptr, 0);
    t.started = t.pending = false;
    t.tx_size = 0;
    be_pushint(vm, status(result));
    be_return(vm);
}

/** @brief Performs a bounded read, optionally combining a staged write. */
int Request(bvm* vm) {
    const auto index = bus(vm);
    auto& t = transactions[index];
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const auto count = static_cast<std::size_t>(integer(vm, 3, 0, i2c::kMaxTransfer));
    if (be_top(vm) > 3 && (!be_isbool(vm, 4) || !be_tobool(vm, 4)))
        be_raise(vm, "value_error", "Wire request requires stop=true");
    if (t.pending && t.address != address)
        be_raise(vm, "value_error", "Wire repeated start address differs");
    const auto result = i2c::transfer(index, address, t.pending ? t.tx.data() : nullptr,
        t.pending ? t.tx_size : 0, t.rx.data(), count);
    t.started = t.pending = false;
    t.tx_size = 0;
    t.position = 0;
    t.rx_size = result == i2c::Result::success ? count : 0;
    be_pushint(vm, static_cast<bint>(t.rx_size));
    be_return(vm);
}

/** @brief Returns the unread received-byte count. */
int Available(bvm* vm) {
    const auto& t = transactions[bus(vm)];
    be_pushint(vm, static_cast<bint>(t.rx_size - t.position));
    be_return(vm);
}

/** @brief Consumes one received byte, or returns minus one when empty. */
int Read(bvm* vm) {
    auto& t = transactions[bus(vm)];
    be_pushint(vm, t.position < t.rx_size ? t.rx[t.position++] : -1);
    be_return(vm);
}

/** @brief Reads bounded register bytes with a repeated START. */
int ReadBytes(bvm* vm) {
    const auto index = bus(vm);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const auto reg = static_cast<std::uint8_t>(integer(vm, 3, 0, 255));
    const auto count = static_cast<std::size_t>(integer(vm, 4, 0, i2c::kMaxTransfer));
    std::array<std::uint8_t, i2c::kMaxTransfer> data{};
    if (i2c::transfer(index, address, &reg, 1, data.data(), count) != i2c::Result::success)
        be_return_nil(vm);
    be_pushbytes(vm, data.data(), count);
    be_return(vm);
}

/** @brief Writes a register selector followed by bounded byte data. */
int WriteBytes(bvm* vm) {
    const auto index = bus(vm);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const auto reg = static_cast<std::uint8_t>(integer(vm, 3, 0, 255));
    std::size_t size{};
    const auto* bytes = static_cast<const std::uint8_t*>(be_tobytes(vm, 4, &size));
    if (!bytes)
        be_raise(vm, "type_error", "Wire write_bytes expects bytes");
    if (size >= i2c::kMaxTransfer)
        be_raise(vm, "value_error", "Wire transfer capacity exceeded");
    std::array<std::uint8_t, i2c::kMaxTransfer> data{};
    data[0] = reg;
    std::memcpy(data.data() + 1, bytes, size);
    be_pushbool(vm,
        i2c::transfer(index, address, data.data(), size + 1, nullptr, 0) == i2c::Result::success);
    be_return(vm);
}

/** @brief Reads an integer register in the requested byte order. */
int RegisterRead(bvm* vm) {
    const auto index = bus(vm);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const auto reg = static_cast<std::uint8_t>(integer(vm, 3, 0, 255));
    const auto requested = integer(vm, 4, -4, 4);
    const auto size = static_cast<std::size_t>(requested < 0 ? -requested : requested);
    if (!size)
        be_raise(vm, "value_error", "Wire register size must be 1..4");
    std::array<std::uint8_t, 4> data{};
    if (i2c::transfer(index, address, &reg, 1, data.data(), size) != i2c::Result::success)
        be_return_nil(vm);
    std::uint32_t value{};
    for (std::size_t i = 0; i < size; ++i)
        value = (value << 8) | data[requested < 0 ? size - 1 - i : i];
    std::int32_t signed_value{};
    std::memcpy(&signed_value, &value, sizeof(value));
    be_pushint(vm, signed_value);
    be_return(vm);
}

/** @brief Writes an integer register in network byte order. */
int RegisterWrite(bvm* vm) {
    const auto index = bus(vm);
    const auto address = static_cast<std::uint8_t>(integer(vm, 2, 1, 127));
    const auto reg = static_cast<std::uint8_t>(integer(vm, 3, 0, 255));
    if (!be_isint(vm, 4))
        be_raise(vm, "type_error", "Wire register value expects integer");
    const auto value = static_cast<std::uint32_t>(be_toint(vm, 4));
    const auto size = static_cast<std::size_t>(integer(vm, 5, 1, 4));
    std::array<std::uint8_t, 5> data{};
    data[0] = reg;
    for (std::size_t i = 0; i < size; ++i)
        data[i + 1] = static_cast<std::uint8_t>(value >> ((size - 1 - i) * 8));
    be_pushbool(vm,
        i2c::transfer(index, address, data.data(), size + 1, nullptr, 0) == i2c::Result::success);
    be_return(vm);
}

const bnfuncinfo methods[] = {
    {"bus", nullptr},
    {"init", Init},
    {"enabled", Enabled},
    {"detect", Detect},
    {"scan", Scan},
    {"_begin_transmission", Begin},
    {"_end_transmission", End},
    {"_request_from", Request},
    {"_available", Available},
    {"_write", Write},
    {"_read", Read},
    {"read", RegisterRead},
    {"write", RegisterWrite},
    {"read_bytes", ReadBytes},
    {"write_bytes", WriteBytes},
    {nullptr, nullptr},
};

/** @brief Finds the first configured bus responding at the requested address. */
int WireScan(bvm* vm) {
    const auto address = static_cast<std::uint8_t>(integer(vm, 1, 1, 127));
    if (be_top(vm) > 1 && !be_isnil(vm, 2))
        be_raise(vm, "value_error", "Wire driver index masks are not available");
    for (std::uint8_t index = 0; index < i2c::kBusCount; ++index)
        if (i2c::transfer(index, address, nullptr, 0, nullptr, 0) == i2c::Result::success) {
            be_getglobal(vm, index == 0 ? "wire1" : "wire2");
            be_return(vm);
        }
    be_return_nil(vm);
}
} // namespace

namespace newmota {
/** @brief Installs Wire instances and shared Newmota/Tasmota bus helpers. */
bool install_wire_module(bvm* vm) {
    transactions = {};
    be_pushclass(vm, "Wire", methods);
    be_setglobal(vm, "Wire");
    be_pop(vm, 1);
    for (int index = 1; index <= static_cast<int>(i2c::kBusCount); ++index) {
        be_getglobal(vm, "Wire");
        be_pushint(vm, index);
        if (be_pcall(vm, 1) != BE_OK)
            return false;
        be_pop(vm, 1); // Protected calls leave their arguments above the result.
        be_setglobal(vm, index == 1 ? "wire1" : "wire2");
        be_getglobal(vm, "newmota");
        be_pushvalue(vm, -2);
        const bool added = be_setmember(vm, -2, index == 1 ? "wire1" : "wire2");
        be_pop(vm, 3);
        if (!added)
            return false;
    }
    be_getglobal(vm, "newmota");
    be_pushntvfunction(vm, WireScan);
    const bool added = be_setmember(vm, -2, "wire_scan");
    be_pop(vm, 1);
    be_setglobal(vm, "tasmota");
    be_pop(vm, 1);
    return added;
}

/** @brief Clears staged state when the Berry VM shuts down. */
void remove_wire_module() {
    transactions = {};
}
} // namespace newmota
