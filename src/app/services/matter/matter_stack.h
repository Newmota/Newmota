/*
  matter_stack.h - Matter sessions, reliable messaging and transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "services/matter/matter_dns_sd.h"

#include <cstddef>
#include <cstdint>

namespace newmota::matter {

inline constexpr std::uint16_t kProtocolSecureChannel = 0;
inline constexpr std::uint16_t kDeliveryFailed = 0xFFFF;

/** @brief One decoded message; the payload stays valid until the next stack_next. */
struct Inbound {
    int context{};
    std::uint16_t exchange{};
    bool peer_initiator{};
    std::uint16_t protocol{};
    std::uint8_t opcode{};
    const std::uint8_t* payload{};
    std::size_t size{};
};

/** @brief Allocates the stack; idempotent. @return false without memory. */
[[nodiscard]] bool stack_start();
/** @brief Runs retransmissions, delayed acknowledgements, and DNS-SD. */
void stack_poll(std::uint32_t now);
/**
 * @brief Decodes queued datagrams until one needs the application.
 * @return true with a message, or with protocol kDeliveryFailed when reliable delivery gave up.
 */
[[nodiscard]] bool stack_next(std::uint32_t now, Inbound& message);
/** @brief Sends on a context, piggybacking a pending acknowledgement on the same exchange. */
[[nodiscard]] bool stack_send(int context, std::uint16_t exchange, bool initiator,
    std::uint16_t protocol, std::uint8_t opcode, const std::uint8_t* payload, std::size_t size,
    bool reliable, std::uint32_t now);
/** @brief Reports the largest payload a context can carry in one message. */
[[nodiscard]] std::size_t stack_max_message(int context);
/** @brief Reports a context's transport: 0 UDP, 1 BTP, 2 TCP, negative without one. */
[[nodiscard]] int stack_transport(int context);
/**
 * @brief Opens a TCP connection to the peer of another context, for a handshake this device
 * starts. @return The unsecured context to send Sigma1 on, negative when none could start.
 */
[[nodiscard]] int stack_connect(int via_context);
/** @brief Uses the active interval a peer announced for its retransmissions. */
void stack_peer_active_interval(int context, std::uint32_t milliseconds);
/** @brief Allocates an unused session id for a handshake. */
[[nodiscard]] std::uint16_t stack_new_session_id();

/** @brief Keys and identities of an established session. */
struct SessionParameters {
    std::uint16_t local_id{};
    std::uint16_t peer_id{};
    std::uint8_t decrypt_key[16]{};
    std::uint8_t encrypt_key[16]{};
    std::uint8_t peer_node[8]{};
    std::uint8_t local_node[8]{};
    std::uint8_t fabric{};
};

/** @brief Installs a session reaching the peer of another context; evicts the oldest if full. */
[[nodiscard]] int stack_session_new(int from_context, const SessionParameters& parameters);
inline constexpr std::size_t kSessionRecordSize = 84;

/**
 * @brief Writes a session as a record that survives a restart, and reserves its send counters.
 * @return kSessionRecordSize, or 0 for no session or one that cannot outlive its transport.
 */
std::size_t stack_session_export(int context, std::uint8_t* output);
/** @brief Restores an exported session; its counters resume above the reserved range. */
[[nodiscard]] int stack_session_import(
    const std::uint8_t* record, std::size_t size, std::uint32_t now);
/** @brief Takes the bit set of fabrics whose stored counter range runs short. */
[[nodiscard]] std::uint32_t stack_take_dirty_fabrics();
/** @brief Reports the addresses of the link, for diagnostics. */
[[nodiscard]] const DnsRecords& stack_addresses();
/** @brief Reports whether the sockets and an address are up, so sends can leave the device. */
[[nodiscard]] bool stack_network_ready();
/** @brief Forgets one session and its queued reliable messages. */
void stack_session_remove(int context);
/** @brief Forgets every session of a fabric. */
void stack_fabric_remove(std::uint8_t fabric);

/** @brief The records the responder publishes; announced after dns_changed. */
[[nodiscard]] DnsRecords& stack_dns_records();
void stack_dns_changed();

} // namespace newmota::matter
