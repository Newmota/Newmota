/*
  rtu_service.cpp - Queued Modbus RTU transactions over shared serial buses

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtu_service.cpp
 * @brief Queued Modbus RTU transactions over shared serial buses.
 */

#include "drivers/modbus/rtu_service.h"
#include "newmota/platform/platform.h"
#include "core/command/command.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace newmota::modbus {
namespace {
constexpr std::uint8_t kNoClient = 255;
enum class Job : std::uint8_t { idle, queued, active, complete };
enum class Phase : std::uint8_t { quiet, writing, draining, reading, cancelling };

struct ClientState {
    std::array<std::uint8_t, kMaximumFrame> request{};
    Response response{};
    Transaction transaction{};
    std::uint32_t generation{};
    std::uint16_t request_size{};
    std::uint8_t bus{}, address{}, attempts{};
    Job job{Job::idle};
    bool closing{}, cancellation{}, exclusive_programmer{}, exclusive_bus{};
};

struct BusState {
    BusConfiguration configuration{};
    serial::Handle serial{};
    serial::Status status{};
    std::array<std::uint8_t, kMaximumFrame> received{};
    std::uint32_t deadline{}, quiet_since{}, silence_ms{};
    std::uint16_t written{}, received_size{};
    std::uint8_t active_client{kNoClient}, next_client{};
    Phase phase{Phase::quiet};
    bool faulted{};
};

std::array<std::unique_ptr<BusState>, kMaximumBuses> buses{};
std::array<std::unique_ptr<ClientState>, kMaximumClients> clients{};
std::uint32_t generation{1};

/** @brief Accepts wrapping deadlines bounded to much less than half the millisecond range. */
bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

ClientState* client(Client handle, bool closing = false) {
    if (handle.slot >= clients.size())
        return nullptr;
    auto* state = clients[handle.slot].get();
    return state && state->generation == handle.generation && (closing || !state->closing)
               ? state
               : nullptr;
}

bool equal(const BusConfiguration& a, const BusConfiguration& b) {
    return a.uart.rx.value() == b.uart.rx.value() && a.uart.tx.value() == b.uart.tx.value() &&
           a.transmit_enable.value() == b.transmit_enable.value() &&
           a.receive_enable.value() == b.receive_enable.value() &&
           a.receive_active_level == b.receive_active_level && a.uart.baud == b.uart.baud &&
           a.uart.data_bits == b.uart.data_bits && a.uart.stop_bits == b.uart.stop_bits &&
           a.uart.parity == b.uart.parity && a.uart.inverted == b.uart.inverted &&
           a.uart.rx_capacity == b.uart.rx_capacity;
}

std::uint32_t character_bits(const BusConfiguration& config) {
    return 1U + config.uart.data_bits + config.uart.stop_bits +
           (config.uart.parity == platform::serial::Parity::none ? 0U : 1U);
}

/** @brief Rounds silence upward and adds one tick because buffer observations use milliseconds. */
std::uint32_t silence(const BusConfiguration& config) {
    const auto computed = (character_bits(config) * 3500 + config.uart.baud - 1) / config.uart.baud;
    return (config.uart.baud > 19200 ? std::max<std::uint32_t>(computed, 2U) : computed) + 1;
}

/** @brief Completes an attempt; a retry rejoins the fair queue behind other clients. */
void complete(BusState& bus, std::uint32_t now, Result result, std::uint8_t exception = 0) {
    if (bus.active_client != kNoClient && clients[bus.active_client]) {
        auto& state = *clients[bus.active_client];
        if (state.cancellation || state.closing)
            result = Result::cancelled;
        const bool retry = result != Result::success && result != Result::exception &&
                           result != Result::cancelled &&
                           state.attempts <= state.transaction.retries;
        if (retry)
            state.job = Job::queued;
        else {
            state.response.result = result;
            state.response.attempts = state.attempts;
            state.response.exception_code = exception;
            state.job = Job::complete;
        }
    }
    bus.active_client = kNoClient;
    bus.phase = Phase::quiet;
    bus.quiet_since = now;
    bus.received_size = 0;
}

/** @brief Quarantines a failed UART without releasing pads or leaving queued clients unresolved. */
void fault(std::uint8_t index, BusState& bus, std::uint32_t now, Result result) {
    bus.faulted = true;
    (void)serial::request_close(bus.serial);
    complete(bus, now, result);
    for (auto& entry : clients) {
        if (!entry || entry->bus != index ||
            (entry->job != Job::queued && entry->job != Job::active))
            continue;
        entry->response.result = result;
        entry->response.attempts = entry->attempts;
        entry->job = Job::complete;
    }
}

/** @brief Consumes a bounded amount of stale input and restarts the observed silence guard. */
bool discard(BusState& bus, std::uint32_t now) {
    std::array<std::uint8_t, 96> bytes{};
    const auto transfer = serial::read(bus.serial, bytes.data(), bytes.size());
    if (transfer.result != serial::Result::success)
        return false;
    if (transfer.count)
        bus.quiet_since = now;
    return true;
}

/** @brief Starts the next queued address client after the physical bus has been quiet. */
void start(std::uint8_t index, BusState& bus, std::uint32_t now) {
    for (std::size_t offset = 0; offset < clients.size(); ++offset) {
        const auto selected = (bus.next_client + offset) % clients.size();
        if (!clients[selected])
            continue;
        auto& state = *clients[selected];
        if (state.bus != index || state.job != Job::queued || state.closing)
            continue;
        const auto result = serial::begin_tx(bus.serial);
        if (result == serial::Result::busy)
            return;
        if (result != serial::Result::success) {
            fault(index, bus, now, Result::io_error);
            return;
        }
        state.job = Job::active;
        ++state.attempts;
        state.response = {};
        bus.active_client = static_cast<std::uint8_t>(selected);
        bus.next_client = static_cast<std::uint8_t>((selected + 1) % clients.size());
        bus.written = bus.received_size = 0;
        const auto wire_ms =
            (std::uint32_t(state.request_size) * character_bits(bus.configuration) * 1000 +
                bus.configuration.uart.baud - 1) /
            bus.configuration.uart.baud;
        bus.deadline = now + wire_ms + state.transaction.timeout_ms;
        bus.status = serial::status(bus.serial);
        bus.phase = Phase::writing;
        return;
    }
}

/** @brief Checks exact address/function/length, optional byte count or echo, and CRC. */
void receive(BusState& bus, ClientState& state, std::uint32_t now) {
    const auto transfer = serial::read(bus.serial, bus.received.data() + bus.received_size,
        bus.received.size() - bus.received_size);
    if (transfer.result != serial::Result::success) {
        complete(bus, now, Result::io_error);
        return;
    }
    bus.received_size += transfer.count;
    if (bus.received_size >= 2) {
        const auto function = bus.received[1];
        const bool exception = function == (state.request[1] | 0x80U);
        if (bus.received[0] != state.address || (!exception && function != state.request[1])) {
            complete(bus, now, Result::protocol_error);
            return;
        }
        const auto expected =
            exception ? 5U : std::uint32_t(state.transaction.response_data_size) + 4;
        if (bus.received_size > expected) {
            complete(bus, now, Result::protocol_error);
            return;
        }
        if (bus.received_size == expected) {
            const auto check = crc16(bus.received.data(), expected - 2);
            if (bus.received[expected - 2] != std::uint8_t(check) ||
                bus.received[expected - 1] != std::uint8_t(check >> 8)) {
                complete(bus, now, Result::crc_error);
                return;
            }
            if (exception) {
                state.response.function = function;
                complete(bus, now, Result::exception, bus.received[2]);
                return;
            }
            const auto size = state.transaction.response_data_size;
            if ((state.transaction.match == ResponseMatch::byte_count &&
                    bus.received[2] != size - 1) ||
                (state.transaction.match == ResponseMatch::echo_prefix &&
                    std::memcmp(bus.received.data() + 2, state.request.data() + 2,
                        state.transaction.echo_bytes))) {
                complete(bus, now, Result::protocol_error);
                return;
            }
            state.response.function = function;
            state.response.size = size;
            std::copy_n(bus.received.begin() + 2, size, state.response.data.begin());
            complete(bus, now, Result::success);
            return;
        }
    }
    if (reached(now, bus.deadline))
        complete(bus, now, Result::timeout);
}
} // namespace

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
    }
    return crc;
}

Result open_bus(std::uint8_t index, const BusConfiguration& config) {
    if (index >= buses.size() || !config.uart.rx.valid() || !config.uart.tx.valid() ||
        config.uart.data_bits != 8 || config.uart.baud < 300 || config.uart.baud > 115200 ||
        config.uart.rx_capacity != platform::serial::kCapacity)
        return Result::invalid_argument;
    if (buses[index])
        return !buses[index]->faulted && equal(buses[index]->configuration, config)
                   ? Result::success
                   : Result::busy;
    auto bus = std::unique_ptr<BusState>(new (std::nothrow) BusState);
    if (!bus)
        return Result::no_memory;
    const auto opened = serial::open(config.uart, bus->serial);
    if (opened != serial::Result::success)
        return opened == serial::Result::busy ? Result::busy : Result::unavailable;
    bus->configuration = config;
    bus->silence_ms = silence(config);
    bus->quiet_since = platform::monotonic_milliseconds();
    if (config.transmit_enable.valid() &&
        serial::config_tx_en(bus->serial, config.transmit_enable) != serial::Result::success) {
        if (serial::close(bus->serial) != serial::Result::success) {
            bus->faulted = true;
            buses[index] = std::move(bus);
        }
        return Result::io_error;
    }
    if (config.receive_enable.valid() &&
        serial::config_rx_en(bus->serial, config.receive_enable, config.receive_active_level) !=
            serial::Result::success) {
        if (serial::close(bus->serial) != serial::Result::success) {
            bus->faulted = true;
            buses[index] = std::move(bus);
        }
        return Result::io_error;
    }
    buses[index] = std::move(bus);
    return Result::success;
}

bool write_bus(CommandOutput& output, std::uint8_t index) {
    if (index >= buses.size())
        return false;
    BusConfiguration config{};
    const bool retained = configuration(index, config);
    if (!buses[index] && !retained)
        return output.format("{\"ModbusBus\":%u,\"Configured\":false,\"StorageAvailable\":%s}",
            unsigned(index + 1), storage_available() ? "true" : "false");
    if (buses[index])
        config = buses[index]->configuration;
    std::size_t count{};
    for (const auto& state : clients)
        if (state && state->bus == index)
            ++count;
    return output.format(
        "{\"ModbusBus\":%u,\"Configured\":true,\"Available\":%s,\"Rx\":%u,\"Tx\":%u,\"DE\":%d,"
        "\"RE\":%d,\"RxActive\":%s,\"Baud\":%lu,\"Parity\":%u,\"StopBits\":%u,\"Clients\":%u,"
        "\"SilenceMs\":%lu,\"StorageAvailable\":%s}",
        unsigned(index + 1), bus_available(index) ? "true" : "false",
        unsigned(config.uart.rx.value()), unsigned(config.uart.tx.value()),
        config.transmit_enable.valid() ? int(config.transmit_enable.value()) : -1,
        config.receive_enable.valid() ? int(config.receive_enable.value()) : -1,
        config.receive_active_level ? "true" : "false",
        static_cast<unsigned long>(config.uart.baud), unsigned(config.uart.parity),
        unsigned(config.uart.stop_bits), unsigned(count),
        static_cast<unsigned long>(silence(config)), storage_available() ? "true" : "false");
}

Result close_native_bus(std::uint8_t index) {
    if (index >= buses.size())
        return Result::invalid_argument;
    if (!buses[index])
        return Result::success;
    for (const auto& state : clients)
        if (state && state->bus == index)
            return Result::busy;
    auto& bus = *buses[index];
    if (serial::active(bus.serial) && !serial::tx_idle(bus.serial))
        return Result::busy;
    if (serial::close(bus.serial) != serial::Result::success) {
        bus.faulted = true;
        return Result::io_error;
    }
    buses[index].reset();
    return Result::success;
}

bool bus_available(std::uint8_t index) {
    return index < buses.size() && buses[index] && !buses[index]->faulted &&
           serial::active(buses[index]->serial);
}

std::size_t client_count(std::uint8_t bus) {
    std::size_t count{};
    for (const auto& state : clients)
        if (state && state->bus == bus)
            ++count;
    return count;
}

Result open_client(std::uint8_t bus, std::uint8_t address, Client& handle, bool vendor_address,
    bool exclusive_programmer, bool exclusive_bus) {
    handle = {};
    if (bus >= buses.size() || (address > 247 && !vendor_address))
        return Result::invalid_argument;
    if (!bus_available(bus))
        return Result::unavailable;
    for (const auto& state : clients)
        if (state && state->bus == bus && (state->exclusive_programmer || state->exclusive_bus))
            return Result::busy;
    if ((exclusive_bus && client_count(bus)) || (exclusive_programmer && client_count(bus) > 1))
        return Result::busy;
    std::size_t slot{};
    while (slot < clients.size() && clients[slot])
        ++slot;
    if (slot == clients.size() || generation == UINT32_MAX)
        return Result::busy;
    auto state = std::unique_ptr<ClientState>(new (std::nothrow) ClientState);
    if (!state)
        return Result::no_memory;
    state->generation = ++generation;
    state->bus = bus;
    state->address = address;
    state->exclusive_programmer = exclusive_programmer;
    state->exclusive_bus = exclusive_bus;
    handle = {state->generation, static_cast<std::uint8_t>(slot)};
    clients[slot] = std::move(state);
    return Result::success;
}

bool active(Client handle) {
    return client(handle) != nullptr;
}

Result retarget(Client handle, std::uint8_t address, bool vendor_address) {
    auto* state = client(handle);
    if (!state || !state->exclusive_bus || (address > 247 && !vendor_address))
        return Result::invalid_argument;
    if (state->job != Job::idle)
        return Result::busy;
    state->address = address;
    return Result::success;
}

void close_client(Client handle) {
    auto* state = client(handle, true);
    if (!state)
        return;
    state->closing = true;
    state->cancellation = true;
    if (state->job != Job::active)
        clients[handle.slot].reset();
}

Result cancel(Client handle) {
    auto* state = client(handle);
    if (!state)
        return Result::invalid_argument;
    if (state->job == Job::idle || state->job == Job::complete)
        return Result::success;
    state->cancellation = true;
    if (state->job == Job::queued) {
        state->response.result = Result::cancelled;
        state->job = Job::complete;
    }
    return Result::success;
}

Result submit(Client handle, std::uint8_t function, const std::uint8_t* data, std::size_t size,
    const Transaction& transaction) {
    auto* state = client(handle);
    if (!state || !function || function > 127 || size > kMaximumData || (size && !data) ||
        transaction.response_data_size > kMaximumData || transaction.timeout_ms < 10 ||
        transaction.timeout_ms > 5000 || transaction.retries > 3 ||
        unsigned(transaction.match) > unsigned(ResponseMatch::echo_prefix) ||
        (transaction.match == ResponseMatch::byte_count && !transaction.response_data_size) ||
        (transaction.match == ResponseMatch::echo_prefix && !transaction.echo_bytes) ||
        transaction.echo_bytes > size || transaction.echo_bytes > transaction.response_data_size ||
        (!state->address && transaction.expect_response))
        return Result::invalid_argument;
    if (!bus_available(state->bus))
        return Result::unavailable;
    if (state->job != Job::idle)
        return Result::busy;
    state->request[0] = state->address;
    state->request[1] = function;
    if (size)
        std::memcpy(state->request.data() + 2, data, size);
    const auto check = crc16(state->request.data(), size + 2);
    state->request[size + 2] = check;
    state->request[size + 3] = check >> 8;
    state->request_size = size + 4;
    state->transaction = transaction;
    state->attempts = 0;
    state->cancellation = false;
    state->response = {};
    state->job = Job::queued;
    return Result::success;
}

bool take_response(Client handle, Response& response) {
    auto* state = client(handle);
    if (!state || state->job != Job::complete)
        return false;
    response = state->response;
    state->job = Job::idle;
    state->cancellation = false;
    return true;
}

void poll(std::uint32_t now) {
    for (std::size_t index = 0; index < buses.size(); ++index) {
        if (!buses[index])
            continue;
        auto& bus = *buses[index];
        if (bus.faulted)
            continue;
        if (!serial::active(bus.serial)) {
            fault(index, bus, now, Result::io_error);
            continue;
        }
        if (bus.phase == Phase::quiet) {
            if (!discard(bus, now)) {
                fault(index, bus, now, Result::io_error);
                continue;
            }
            if (now - bus.quiet_since >= bus.silence_ms && !serial::available(bus.serial))
                start(index, bus, now);
            continue;
        }
        auto& state = *clients[bus.active_client];
        const auto status = serial::status(bus.serial);
        if (status.errors != bus.status.errors || status.dropped != bus.status.dropped) {
            fault(index, bus, now, Result::io_error);
            continue;
        }
        if (state.cancellation && bus.phase != Phase::cancelling) {
            if (bus.phase == Phase::writing &&
                serial::end_tx(bus.serial) != serial::Result::success) {
                fault(index, bus, now, Result::io_error);
                continue;
            }
            bus.phase = Phase::cancelling;
        }
        if (bus.phase == Phase::cancelling) {
            if (serial::tx_idle(bus.serial))
                complete(bus, now, Result::cancelled);
            else if (reached(now, bus.deadline))
                fault(index, bus, now, Result::timeout);
        } else if (bus.phase == Phase::writing) {
            // A drained partial ADU cannot be extended safely without per-byte wire timestamps.
            // Abort conservatively, then let the retry rejoin the fair queue after silence.
            if (bus.written && serial::transmitter_empty(bus.serial)) {
                if (serial::end_tx(bus.serial) != serial::Result::success)
                    fault(index, bus, now, Result::io_error);
                else
                    complete(bus, now, Result::timing_error);
                continue;
            }
            const auto transfer = serial::write_some(
                bus.serial, state.request.data() + bus.written, state.request_size - bus.written);
            if (transfer.result != serial::Result::success) {
                fault(index, bus, now, Result::io_error);
                continue;
            }
            bus.written += transfer.count;
            if (bus.written == state.request_size) {
                if (serial::end_tx(bus.serial) != serial::Result::success) {
                    fault(index, bus, now, Result::io_error);
                    continue;
                }
                bus.phase = Phase::draining;
            } else if (reached(now, bus.deadline))
                fault(index, bus, now, Result::timeout);
        } else if (bus.phase == Phase::draining) {
            if (serial::tx_idle(bus.serial)) {
                if (!state.transaction.expect_response)
                    complete(bus, now, Result::success);
                else {
                    bus.phase = Phase::reading;
                    bus.deadline = now + state.transaction.timeout_ms;
                }
            } else if (reached(now, bus.deadline))
                fault(index, bus, now, Result::timeout);
        } else
            receive(bus, state, now);
    }
    for (auto& state : clients)
        if (state && state->closing && state->job != Job::active)
            state.reset();
}
} // namespace newmota::modbus
