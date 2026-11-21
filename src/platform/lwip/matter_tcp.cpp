/*
  matter_tcp.cpp - lwIP IPv6 TCP connections for Matter large messages

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file matter_tcp.cpp
 * @brief Matter's TCP transport: each message travels after its 4-byte little-endian length.
 *
 * The raw API runs on the lwIP core. The application claims a slot for an outgoing connection
 * and hands sends over as copies; complete messages come back through a single-producer queue,
 * as on the UDP side. A peer that breaks the framing or floods the queue loses its connection.
 */
#include "newmota/platform/matter_tcp.h"

extern "C" {
#include "lwip/err.h"
#include "lwip/ip6_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#if !defined(PICO_ON_DEVICE)
#include "lwip/tcpip.h"
#endif
}
#if defined(PICO_ON_DEVICE)
#include "pico/cyw43_arch.h"
#endif

#if !defined(LWIP_IPV6_SCOPES)
#define ip6_addr_zone(address) 0U
#define ip6_addr_set_zone(address, zone) ((void)(address), (void)(zone))
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace newmota::platform {
namespace {
constexpr std::uint16_t kMatterPort = 5540;
constexpr std::size_t kConnections = 4;
constexpr std::size_t kQueueSlots = 8;
constexpr std::size_t kMaxQueuedBytes = 2U * kMatterTcpMaxMessage;
constexpr std::size_t kLengthPrefix = 4;

enum Phase : std::uint8_t { kFree, kOpening, kOpen };

/** @brief One queued outgoing message with its prefix, freed once lwIP holds a copy. */
struct OutBuffer {
    OutBuffer* next{};
    std::size_t size{};
    std::size_t sent{};
    std::uint8_t data[1]{};
};

struct Connection {
    std::atomic<std::uint8_t> phase{kFree};
    std::atomic<std::uint8_t> generation{};
    // Owned by the lwIP core.
    tcp_pcb* pcb{};
    UdpPeer peer{};
    std::uint8_t header[kLengthPrefix]{};
    std::size_t header_size{};
    UdpDatagram* message{};
    std::size_t received{};
    OutBuffer* out{};
};

Connection connections[kConnections]{};
tcp_pcb* listener{};
std::atomic<bool> listening{false};
std::atomic<bool> listen_queued{false};

// Single producer on the lwIP core, single consumer in the application context.
struct Queued {
    std::atomic<UdpDatagram*> datagram{};
    int connection{};
};

Queued queue[kQueueSlots]{};
std::size_t queue_write{};
std::size_t queue_read{};
std::atomic<std::size_t> queued_bytes{0};

/** @brief Runs a function on the lwIP core, as matter_udp.cpp does. */
[[nodiscard]] bool on_core(void (*function)(void*), void* const argument) {
#if defined(PICO_ON_DEVICE)
    cyw43_arch_lwip_begin();
    function(argument);
    cyw43_arch_lwip_end();
    return true;
#else
    return tcpip_callback_with_block(function, argument, 0) == ERR_OK;
#endif
}

[[nodiscard]] int handle_of(const Connection& connection) {
    return static_cast<int>(&connection - connections) |
           (static_cast<int>(connection.generation.load()) << 8U);
}

[[nodiscard]] Connection* find(const int handle) {
    const auto slot = static_cast<std::size_t>(handle & 0xFF);
    if (handle < 0 || slot >= kConnections)
        return nullptr;
    Connection& connection = connections[slot];
    return connection.phase.load() != kFree && connection.generation.load() == (handle >> 8)
               ? &connection
               : nullptr;
}

/** @brief Claims a free slot under a new generation, from either side. */
[[nodiscard]] Connection* claim(const Phase phase) {
    for (Connection& connection : connections) {
        std::uint8_t expected = kFree;
        if (connection.phase.compare_exchange_strong(expected, kOpening)) {
            connection.generation.store(
                static_cast<std::uint8_t>(connection.generation.load() + 1U));
            connection.phase.store(phase);
            return &connection;
        }
    }
    return nullptr;
}

/** @brief Frees what a connection holds on the core; the pcb is already gone or closed. */
void release(Connection& connection) {
    connection.pcb = nullptr;
    while (OutBuffer* const buffer = connection.out) {
        connection.out = buffer->next;
        std::free(buffer);
    }
    std::free(connection.message);
    connection.message = nullptr;
    connection.received = 0;
    connection.header_size = 0;
    connection.phase.store(kFree);
}

/** @brief Drops a connection on the core; the caller must not touch the pcb afterwards. */
void abort_connection(Connection& connection) {
    tcp_pcb* const pcb = connection.pcb;
    release(connection);
    if (pcb != nullptr) {
        tcp_arg(pcb, nullptr);
        tcp_abort(pcb);
    }
}

/** @brief Writes queued messages as far as the send buffer allows. */
void flush(Connection& connection) {
    tcp_pcb* const pcb = connection.pcb;
    if (pcb == nullptr || connection.phase.load() != kOpen)
        return;
    while (OutBuffer* const buffer = connection.out) {
        const std::size_t room = tcp_sndbuf(pcb);
        if (room == 0U)
            break;
        const auto size = static_cast<u16_t>(
            std::min<std::size_t>({buffer->size - buffer->sent, room, std::size_t{0xFFFF}}));
        if (tcp_write(pcb, buffer->data + buffer->sent, size, TCP_WRITE_FLAG_COPY) != ERR_OK)
            break;
        buffer->sent += size;
        if (buffer->sent < buffer->size)
            break;
        connection.out = buffer->next;
        std::free(buffer);
    }
    (void)tcp_output(pcb);
}

[[nodiscard]] bool enqueue(UdpDatagram* const datagram, const int handle) {
    Queued& slot = queue[queue_write];
    if (slot.datagram.load() != nullptr || queued_bytes.load() + datagram->size > kMaxQueuedBytes)
        return false;
    queued_bytes.fetch_add(datagram->size);
    slot.connection = handle;
    slot.datagram.store(datagram);
    queue_write = (queue_write + 1U) % kQueueSlots;
    return true;
}

/** @brief Splits received bytes into messages. False when the peer broke the rules. */
[[nodiscard]] bool take_bytes(Connection& connection, const std::uint8_t* data, std::size_t size) {
    while (size != 0U) {
        if (connection.message == nullptr) {
            const std::size_t take = std::min(kLengthPrefix - connection.header_size, size);
            std::memcpy(connection.header + connection.header_size, data, take);
            connection.header_size += take;
            data += take;
            size -= take;
            if (connection.header_size < kLengthPrefix)
                continue;
            connection.header_size = 0;
            const std::uint32_t length = connection.header[0] | (connection.header[1] << 8U) |
                                         (connection.header[2] << 16U) |
                                         (static_cast<std::uint32_t>(connection.header[3]) << 24U);
            if (length == 0U || length > kMatterTcpMaxMessage)
                return false;
            auto* const message =
                static_cast<UdpDatagram*>(std::malloc(sizeof(UdpDatagram) + length));
            if (message == nullptr)
                return false;
            new (message) UdpDatagram{};
            message->peer = connection.peer;
            message->size = static_cast<std::uint16_t>(length);
            connection.message = message;
            connection.received = 0;
            continue;
        }
        UdpDatagram* const message = connection.message;
        const std::size_t take = std::min<std::size_t>(message->size - connection.received, size);
        std::memcpy(message->data + connection.received, data, take);
        connection.received += take;
        data += take;
        size -= take;
        if (connection.received == message->size) {
            connection.message = nullptr;
            if (!enqueue(message, handle_of(connection))) {
                std::free(message);
                return false;
            }
        }
    }
    return true;
}

err_t received(void* const argument, tcp_pcb* const pcb, pbuf* const input, const err_t error) {
    auto* const connection = static_cast<Connection*>(argument);
    if (connection == nullptr) {
        if (input != nullptr)
            pbuf_free(input);
        return ERR_OK;
    }
    if (input == nullptr || error != ERR_OK) {
        if (input != nullptr)
            pbuf_free(input);
        tcp_arg(pcb, nullptr);
        release(*connection);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    bool valid = true;
    for (const pbuf* part = input; part != nullptr && valid; part = part->next)
        valid = take_bytes(*connection, static_cast<const std::uint8_t*>(part->payload), part->len);
    tcp_recved(pcb, input->tot_len);
    pbuf_free(input);
    if (!valid) {
        abort_connection(*connection);
        return ERR_ABRT;
    }
    return ERR_OK;
}

err_t sent(void* const argument, tcp_pcb*, u16_t) {
    if (auto* const connection = static_cast<Connection*>(argument))
        flush(*connection);
    return ERR_OK;
}

/** @brief lwIP has already freed the pcb. */
void failed(void* const argument, err_t) {
    if (auto* const connection = static_cast<Connection*>(argument))
        release(*connection);
}

void attach(Connection& connection, tcp_pcb* const pcb) {
    connection.pcb = pcb;
    tcp_arg(pcb, &connection);
    tcp_recv(pcb, received);
    tcp_sent(pcb, sent);
    tcp_err(pcb, failed);
    tcp_nagle_disable(pcb);
}

void set_peer(Connection& connection, const ip_addr_t& address, const u16_t port) {
    std::memcpy(connection.peer.address.data(), ip_2_ip6(&address)->addr, 16);
    connection.peer.port = port;
    connection.peer.zone = static_cast<std::uint8_t>(ip6_addr_zone(ip_2_ip6(&address)));
}

err_t accepted(void*, tcp_pcb* const pcb, const err_t error) {
    if (error != ERR_OK || pcb == nullptr)
        return ERR_VAL;
    Connection* const connection = IP_IS_V6(&pcb->remote_ip) ? claim(kOpen) : nullptr;
    if (connection == nullptr) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    set_peer(*connection, pcb->remote_ip, pcb->remote_port);
    attach(*connection, pcb);
    return ERR_OK;
}

void listen_on_core(void*) {
    listen_queued.store(false);
    if (listener != nullptr)
        return;
    tcp_pcb* const pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (pcb == nullptr)
        return;
    ip_set_option(pcb, SOF_REUSEADDR);
    if (tcp_bind(pcb, IP6_ADDR_ANY, kMatterPort) != ERR_OK) {
        tcp_abort(pcb);
        return;
    }
    listener = tcp_listen_with_backlog(pcb, 2);
    if (listener == nullptr) {
        tcp_abort(pcb);
        return;
    }
    tcp_accept(listener, accepted);
    listening.store(true);
}

void close_on_core(void*) {
    for (Connection& connection : connections) {
        if (connection.phase.load() != kFree)
            abort_connection(connection);
    }
    if (listener != nullptr) {
        (void)tcp_close(listener);
        listener = nullptr;
    }
    listening.store(false);
}

err_t connected(void* const argument, tcp_pcb*, const err_t error) {
    auto* const connection = static_cast<Connection*>(argument);
    if (connection == nullptr)
        return ERR_OK;
    if (error != ERR_OK) {
        abort_connection(*connection);
        return ERR_ABRT;
    }
    connection->phase.store(kOpen);
    flush(*connection);
    return ERR_OK;
}

struct ConnectJob {
    Connection* connection{};
    std::uint8_t generation{};
    UdpPeer peer{};
};

void connect_on_core(void* const argument) {
    auto* const job = static_cast<ConnectJob*>(argument);
    Connection& connection = *job->connection;
    const bool current = connection.generation.load() == job->generation;
    tcp_pcb* const pcb = current ? tcp_new_ip_type(IPADDR_TYPE_V6) : nullptr;
    if (pcb == nullptr) {
        if (current)
            release(connection);
        delete job;
        return;
    }
    ip_addr_t address{};
    IP_SET_TYPE_VAL(address, IPADDR_TYPE_V6);
    std::memcpy(ip_2_ip6(&address)->addr, job->peer.address.data(), 16);
    ip6_addr_set_zone(ip_2_ip6(&address), job->peer.zone);
    connection.peer = job->peer;
    attach(connection, pcb);
    if (tcp_connect(pcb, &address, job->peer.port, connected) != ERR_OK)
        abort_connection(connection);
    delete job;
}

struct SendJob {
    Connection* connection{};
    std::uint8_t generation{};
    OutBuffer* buffer{};
};

void send_on_core(void* const argument) {
    auto* const job = static_cast<SendJob*>(argument);
    Connection& connection = *job->connection;
    if (connection.phase.load() == kFree || connection.generation.load() != job->generation) {
        std::free(job->buffer);
    } else {
        OutBuffer** tail = &connection.out;
        while (*tail != nullptr)
            tail = &(*tail)->next;
        *tail = job->buffer;
        flush(connection);
    }
    delete job;
}
} // namespace

void matter_tcp_poll() {
    if (listening.load() || listen_queued.exchange(true))
        return;
    if (!on_core(listen_on_core, nullptr))
        listen_queued.store(false);
}

void matter_tcp_close() {
    (void)on_core(close_on_core, nullptr);
    int connection = 0;
    while (UdpDatagram* const message = matter_tcp_receive(connection))
        matter_udp_release(message);
}

UdpDatagram* matter_tcp_receive(int& connection) {
    Queued& slot = queue[queue_read];
    UdpDatagram* const message = slot.datagram.load();
    if (message == nullptr)
        return nullptr;
    connection = slot.connection;
    queued_bytes.fetch_sub(message->size);
    slot.datagram.store(nullptr);
    queue_read = (queue_read + 1U) % kQueueSlots;
    return message;
}

bool matter_tcp_send(const int handle, const void* const data, const std::size_t size) {
    Connection* const connection = find(handle);
    if (connection == nullptr || size == 0U || size > kMatterTcpMaxMessage)
        return false;
    auto* const buffer =
        static_cast<OutBuffer*>(std::malloc(sizeof(OutBuffer) + kLengthPrefix + size));
    auto* const job = new (std::nothrow) SendJob{};
    if (buffer == nullptr || job == nullptr) {
        std::free(buffer);
        delete job;
        return false;
    }
    new (buffer) OutBuffer{};
    buffer->size = kLengthPrefix + size;
    for (std::size_t index = 0; index < kLengthPrefix; ++index)
        buffer->data[index] = static_cast<std::uint8_t>(size >> (index * 8U));
    std::memcpy(buffer->data + kLengthPrefix, data, size);
    *job = {connection, connection->generation.load(), buffer};
    if (!on_core(send_on_core, job)) {
        std::free(buffer);
        delete job;
        return false;
    }
    return true;
}

int matter_tcp_connect(const UdpPeer& peer) {
    Connection* const connection = claim(kOpening);
    if (connection == nullptr)
        return -1;
    auto* const job =
        new (std::nothrow) ConnectJob{connection, connection->generation.load(), peer};
    if (job == nullptr || !on_core(connect_on_core, job)) {
        delete job;
        connection->phase.store(kFree);
        return -1;
    }
    return handle_of(*connection);
}

bool matter_tcp_alive(const int handle) {
    return find(handle) != nullptr;
}

} // namespace newmota::platform
