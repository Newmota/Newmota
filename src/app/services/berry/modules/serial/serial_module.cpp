/*
  serial_module.cpp - Berry serial class bindings

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "services/berry/modules/serial/serial_module.h"
#include "drivers/serial/serial_service.h"
#include "services/berry/berry_service.h"
#include "newmota/platform/platform.h"
#include <array>
#include <cstdio>
extern "C" {
#include "berry.h"
}

namespace {
namespace serial = newmota::serial;
std::array<serial::Handle, serial::kPortCount> handles{};

/** @brief Validates a bounded Berry integer argument before hardware mutation. */
int integer(bvm* vm, int index, int low, int high) {
    if (!be_isint(vm, index))
        be_raise(vm, "type_error", "serial expects integer arguments");
    const auto value = be_toint(vm, index);
    if (value < low || value > high)
        be_raise(vm, "value_error", "serial argument out of range");
    return static_cast<int>(value);
}

/** @brief Raises a Berry exception for a portable operation error. */
void result(bvm* vm, serial::Result value) {
    if (value == serial::Result::success)
        return;
    if (value == serial::Result::timeout)
        be_raise(vm, "timeout_error", "serial transmitter is busy");
    if (value == serial::Result::invalid_argument)
        be_raise(vm, "value_error", "invalid serial configuration");
    if (value == serial::Result::unsupported)
        be_raise(vm, "value_error", "serial configuration is unsupported");
    be_raise(vm, "runtime_error", "serial port is unavailable or reserved");
}

/** @brief Resolves an instance generation against the retained VM handle table. */
serial::Handle handle(bvm* vm) {
    be_getmember(vm, 1, ".generation");
    const auto generation = be_isint(vm, -1) ? static_cast<std::uint32_t>(be_toint(vm, -1)) : 0;
    be_pop(vm, 1);
    if (!generation)
        return {};
    for (auto& h : handles)
        if (h.generation == generation) {
            if (serial::active(h))
                return h;
            h = {};
            return {};
        }
    return {};
}

/** @brief Validates Berry serial arguments and retains successful ownership before VM mutation. */
int Init(bvm* vm) {
    if (be_top(vm) < 4 || be_top(vm) > 6)
        be_raise(vm, "type_error", "serial(rx, tx, baud[, mode, inverted])");
    serial::Config config{};
    const auto rx = integer(vm, 2, -1, 65534), tx = integer(vm, 3, -1, 65534);
    if (rx >= 0)
        config.rx = newmota::PinId{static_cast<std::uint16_t>(rx)};
    if (tx >= 0)
        config.tx = newmota::PinId{static_cast<std::uint16_t>(tx)};
    config.baud = static_cast<std::uint32_t>(integer(vm, 4, 1, 4000000));
    const auto mode = be_top(vm) >= 5 ? integer(vm, 5, 0, 0x0800003f) : 0x0800001c;
    const auto parity = mode & 3;
    if ((mode & ~0x2f) != 0x08000010 || parity == 1)
        be_raise(vm, "value_error", "invalid serial framing mode");
    config.data_bits = static_cast<std::uint8_t>(5 + ((mode >> 2) & 3));
    config.stop_bits = (mode & 0x20) ? 2 : 1;
    config.parity = parity == 0   ? newmota::platform::serial::Parity::none
                    : parity == 2 ? newmota::platform::serial::Parity::even
                                  : newmota::platform::serial::Parity::odd;
    if (be_top(vm) == 6) {
        if (!be_isbool(vm, 6))
            be_raise(vm, "type_error", "serial inverted expects bool");
        config.inverted = be_tobool(vm, 6);
    }
    const auto previous = handle(vm);
    if (previous.generation)
        be_raise(vm, "runtime_error", "close serial before reinitializing");
    serial::Handle opened{};
    result(vm, serial::open(config, opened));
    handles[opened.slot] = opened; // Retained before any Berry operation that can raise.
    be_pushint(vm, static_cast<bint>(opened.generation));
    be_setmember(vm, 1, ".generation");
    be_pop(vm, 1);
    be_return_nil(vm);
}

/** @brief Drains under the VM deadline and clears a successfully closed instance. */
int Close(bvm* vm) {
    auto h = handle(vm);
    if (h.generation) {
        const auto deadline = newmota::platform::monotonic_milliseconds() + 25;
        while (!serial::tx_idle(h)) {
            newmota::check_berry_execution_deadline(vm);
            if (newmota::platform::monotonic_milliseconds() - deadline < 0x80000000U)
                result(vm, serial::Result::timeout);
            newmota::platform::sleep_for_milliseconds(1);
        }
        result(vm, serial::close(h));
        handles[h.slot] = {};
    }
    be_pushnil(vm);
    be_setmember(vm, 1, ".generation");
    be_pop(vm, 1);
    be_return_nil(vm);
}

/** @brief Attempts safe cleanup during Berry object collection without raising. */
int Deinit(bvm* vm) {
    auto h = handle(vm);
    if (h.generation)
        (void)serial::request_close(h);
    serial::poll();
    be_return_nil(vm);
}

/** @brief Returns the buffered RX count, or nil for a closed instance. */
int Available(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    be_pushint(vm, static_cast<bint>(serial::available(h)));
    be_return(vm);
}

/** @brief Allocates Berry bytes before consuming the corresponding native RX bytes. */
int Read(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    const auto count = be_top(vm) == 1 ? newmota::platform::serial::kCapacity
                                       : static_cast<std::size_t>(integer(vm, 2, 0, 256));
    std::array<std::uint8_t, 256> bytes{};
    // Allocate the Berry bytes first; a VM allocation failure cannot consume RX.
    const auto received = serial::read(h, bytes.data(), count, true);
    result(vm, received.result);
    be_pushbytes(vm, bytes.data(), received.count);
    result(vm, serial::read(h, bytes.data(), received.count).result);
    be_return(vm);
}

/** @brief Retries bounded bulk transfers under the VM deadline and returns a short count. */
int Write(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    std::uint8_t byte{};
    const std::uint8_t* data{};
    std::size_t size{};
    if (be_isint(vm, 2)) {
        byte = static_cast<std::uint8_t>(integer(vm, 2, 0, 255));
        data = &byte;
        size = 1;
    } else
        data = static_cast<const std::uint8_t*>(be_tobytes(vm, 2, &size));
    if (!data && size != 0)
        be_raise(vm, "type_error", "serial write expects integer or bytes");
    if (!be_isint(vm, 2) && !be_isbytes(vm, 2))
        be_raise(vm, "type_error", "serial write expects integer or bytes");
    const auto deadline = newmota::platform::monotonic_milliseconds() + 10;
    std::size_t sent{};
    do {
        newmota::check_berry_execution_deadline(vm);
        const auto written = serial::write_some(h, data ? data + sent : nullptr, size - sent);
        result(vm, written.result);
        sent += written.count;
        if (sent == size || newmota::platform::monotonic_milliseconds() - deadline < 0x80000000U)
            break;
        newmota::platform::sleep_for_milliseconds(1);
    } while (true);
    serial::poll();
    be_pushint(vm, static_cast<bint>(sent));
    be_return(vm);
}

/** @brief Drains TX under the VM deadline, then clears RX only after success. */
int Flush(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    const auto deadline = newmota::platform::monotonic_milliseconds() + 25;
    while (!serial::tx_idle(h)) {
        newmota::check_berry_execution_deadline(vm);
        if (newmota::platform::monotonic_milliseconds() - deadline < 0x80000000U)
            result(vm, serial::Result::timeout);
        newmota::platform::sleep_for_milliseconds(1);
    }
    result(vm, serial::discard_rx(h));
    be_return_nil(vm);
}

/** @brief Sets or clears an idle RS-485 enable pin through the common owner. */
int TxEnable(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    const auto pin = integer(vm, 2, -1, 65534);
    result(vm, serial::config_tx_en(h,
                   pin < 0 ? newmota::PinId{} : newmota::PinId{static_cast<std::uint16_t>(pin)}));
    be_return_nil(vm);
}

/** @brief Exposes cumulative RX overflow and native error counters to Berry. */
int Status(bvm* vm) {
    auto h = handle(vm);
    if (!h.generation)
        be_return_nil(vm);
    const auto status = serial::status(h);
    be_newobject(vm, "map");
    be_pushstring(vm, "dropped");
    be_pushint(vm, static_cast<bint>(status.dropped));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
    be_pushstring(vm, "errors");
    be_pushint(vm, static_cast<bint>(status.errors));
    be_data_insert(vm, -3);
    be_pop(vm, 2);
    be_pop(vm, 1);
    be_return(vm);
}

const bnfuncinfo methods[] = {{".generation", nullptr}, {"init", Init}, {"close", Close},
    {"deinit", Deinit}, {"available", Available}, {"read", Read}, {"write", Write},
    {"flush", Flush}, {"config_tx_en", TxEnable}, {"status", Status}, {"SERIAL_5N1", Init},
    {"SERIAL_5N2", Init}, {"SERIAL_5E1", Init}, {"SERIAL_5E2", Init}, {"SERIAL_5O1", Init},
    {"SERIAL_5O2", Init}, {"SERIAL_6N1", Init}, {"SERIAL_6N2", Init}, {"SERIAL_6E1", Init},
    {"SERIAL_6E2", Init}, {"SERIAL_6O1", Init}, {"SERIAL_6O2", Init}, {"SERIAL_7N1", Init},
    {"SERIAL_7N2", Init}, {"SERIAL_7E1", Init}, {"SERIAL_7E2", Init}, {"SERIAL_7O1", Init},
    {"SERIAL_7O2", Init}, {"SERIAL_8N1", Init}, {"SERIAL_8N2", Init}, {"SERIAL_8E1", Init},
    {"SERIAL_8E2", Init}, {"SERIAL_8O1", Init}, {"SERIAL_8O2", Init}, {nullptr, nullptr}};
} // namespace

namespace newmota {
/** @brief Registers the global class and all 24 upstream framing constants. */
bool install_serial_module(bvm* vm) {
    be_pushclass(vm, "serial", methods);
    be_setglobal(vm, "serial");
    for (int bits = 5; bits <= 8; ++bits)
        for (int parity = 0; parity < 3; ++parity)
            for (int stop = 1; stop <= 2; ++stop) {
                char name[16];
                std::snprintf(name, sizeof(name), "SERIAL_%d%c%d", bits, "NEO"[parity], stop);
                be_pushint(vm, 0x08000010 | ((bits - 5) << 2) | (parity ? parity + 1 : 0) |
                                   (stop == 2 ? 0x20 : 0));
                if (!be_setmember(vm, -2, name)) {
                    be_pop(vm, 2);
                    return false;
                }
                be_pop(vm, 1);
            }
    be_pop(vm, 1);
    return true;
}

/** @brief Closes retained VM handles while preserving uncertain native ownership. */
void remove_serial_module() {
    for (auto& h : handles)
        if (h.generation) {
            // A timeout retains native ownership and pin claims. No IRQ touches VM data.
            if (serial::close(h) == serial::Result::success)
                h = {};
        }
}
} // namespace newmota
