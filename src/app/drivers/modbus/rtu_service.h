/*
  rtu_service.h - Fair poll-driven Modbus RTU bus ownership

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file rtu_service.h
 * @brief Fair poll-driven Modbus RTU bus ownership.
 */

#pragma once
#include "drivers/serial/serial_service.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace newmota {
class CommandOutput;
struct CommandRequest;
enum class CommandStatus : std::uint8_t;

namespace modbus {
inline constexpr std::size_t kMaximumBuses = 4;
inline constexpr std::size_t kMaximumClients = 16;
inline constexpr std::size_t kMaximumFrame = 256;
inline constexpr std::size_t kMaximumData = kMaximumFrame - 4;
/** @brief Completion and admission outcomes; no function waits for response bytes. */
enum class Result : std::uint8_t {
    success,
    invalid_argument,
    unavailable,
    busy,
    no_memory,
    io_error,
    timeout,
    crc_error,
    protocol_error,
    exception,
    cancelled,
    storage_error,
    timing_error
};

/** @brief One physical bus owns its UART, optional active-HIGH DE and optional separate RE. */
struct BusConfiguration {
    serial::Config uart{};
    PinId transmit_enable{}, receive_enable{};
    bool receive_active_level{};
};

/** @brief Generation-safe attachment to one physical bus and slave address. */
struct Client {
    std::uint32_t generation{};
    std::uint8_t slot{};
};
/** @brief Additional response-shape check applied after address, function, and CRC validation. */
enum class ResponseMatch : std::uint8_t { length, byte_count, echo_prefix };

/** @brief Fixed bounded reply contract; data excludes address, function and CRC. */
struct Transaction {
    std::uint16_t response_data_size{};
    std::uint16_t timeout_ms{250};
    std::uint8_t retries{2}, echo_bytes{};
    ResponseMatch match{ResponseMatch::length};
    bool expect_response{true};
};

/** @brief A retained result belongs only to its submitting client until take_response(). */
struct Response {
    Result result{Result::unavailable};
    std::array<std::uint8_t, kMaximumData> data{};
    std::uint16_t size{};
    std::uint8_t function{}, exception_code{}, attempts{};
};

/** @brief Restores retained physical buses before device owners are initialized. */
void initialize();
/** @brief Reports whether retained configuration can be safely mutated. */
bool storage_available();
/** @brief Retrieves retained configuration, including a bus whose native restore failed. */
bool configuration(std::uint8_t bus, BusConfiguration& output);
/** @brief Opens an unassigned physical bus slot, or accepts its identical existing configuration.
 * @param bus Zero-based bus slot. Successful mutations are durable before returning.
 */
Result configure_bus(std::uint8_t bus, const BusConfiguration& configuration);
/** @brief Streams current runtime bus configuration and health. */
bool write_bus(CommandOutput&, std::uint8_t bus);
/** @brief Closes an idle bus only after all its clients have detached. */
Result close_bus(std::uint8_t bus);
/** @brief Reports whether a configured bus has a healthy native UART lease. */
bool bus_available(std::uint8_t bus);
/** @brief Counts attached clients, including asynchronously closing clients. */
std::size_t client_count(std::uint8_t bus);
/** @brief Attaches an address client; zero permits broadcasts only.
 * @param exclusive_programmer Prevents new clients until detach; admits at most one existing
 * address client for a single-device programming operation. Physical wiring remains the caller's
 * responsibility.
 * @param exclusive_bus Requires zero existing clients and rejects all further attachments;
 * use for externally multiplexed routes, retaining the route through request completion.
 */
Result open_client(std::uint8_t bus, std::uint8_t address, Client& client,
    bool vendor_address = false, bool exclusive_programmer = false, bool exclusive_bus = false);
/** @brief Retargets an idle exclusive-bus client; queued/active/completed jobs must finish first.
 */
Result retarget(Client client, std::uint8_t address, bool vendor_address = false);
/** @brief Cancels work and asynchronously detaches after active TX has drained. */
void close_client(Client client);
/** @brief Reports whether this generation still names an attached client. */
bool active(Client client);
/** @brief Copies one PDU into the client's single queue slot and appends its RTU CRC.
 * @param data Function data, excluding address/function/CRC; maximum kMaximumData bytes.
 */
Result submit(Client client, std::uint8_t function, const std::uint8_t* data, std::size_t size,
    const Transaction& transaction);
/** @brief Requests cancellation without interrupting an in-flight UART stop bit. */
Result cancel(Client client);
/** @brief Copies a completed response and frees that client's queue slot. */
bool take_response(Client client, Response& response);
/** @brief Advances each configured bus once; call on every cooperative application iteration. */
void poll(std::uint32_t now);
/** @brief Computes standard Modbus reflected CRC-16 (polynomial 0xA001, initial 0xFFFF). */
std::uint16_t crc16(const std::uint8_t* data, std::size_t size);
/** @brief Configures and inspects runtime buses through the common command path. */
CommandStatus dispatch(const CommandRequest&, CommandOutput&);
} // namespace modbus
} // namespace newmota
