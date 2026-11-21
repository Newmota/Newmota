/*
  ping.cpp - Fixed-slot raw lwIP IPv4 echo transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "newmota/platform/ping.h"
#include "newmota/platform/platform.h"
#include "ping_packet.h"

extern "C" {
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#if !defined(PICO_ON_DEVICE)
#include "lwip/tcpip.h"
#endif
}
#if defined(PICO_ON_DEVICE)
#include "pico/cyw43_arch.h"
#endif
#include <atomic>
#include <cstring>

namespace newmota::platform {
namespace {
struct Exchange final {
    std::atomic<PingState> state{PingState::idle};
    std::atomic<bool> queued{false}, cancelled{false}, send_requested{false};
    std::array<char, 128> host{};
    char address_text[16]{};
    ip_addr_t address{};
    raw_pcb* socket{};
    ping_packet::Echo echo{};
    std::uint32_t sent_at{}, elapsed_ms{};
    std::uint32_t next_token{};
    std::uint16_t next_sequence{};
    bool dns_pending{}, started{};
} exchange;

void close_socket() {
    if (exchange.socket != nullptr) {
        raw_remove(exchange.socket);
        exchange.socket = nullptr;
    }
}

void fail() {
    close_socket();
    exchange.state.store(PingState::failed);
}

void cancel() {
    close_socket();
    exchange.send_requested.store(false);
    if (!exchange.dns_pending)
        exchange.state.store(PingState::idle);
}

/** @brief Owns a pbuf only for a complete correlated echo reply; leaves all other ICMP alone. */
u8_t received(void*, raw_pcb*, pbuf* packet, const ip_addr_t* address) {
    if (packet == nullptr || exchange.cancelled.load() ||
        exchange.state.load() != PingState::waiting || !ip_addr_cmp(address, &exchange.address) ||
        packet->tot_len > 100)
        return 0;
    const auto elapsed = monotonic_milliseconds() - exchange.sent_at;
    if (elapsed >= ping_packet::kReplyTimeout)
        return 0;
    std::array<std::uint8_t, 100> bytes{};
    const auto size = pbuf_copy_partial(packet, bytes.data(), packet->tot_len, 0);
    if (size != packet->tot_len || !ping_packet::reply(bytes.data(), size, exchange.echo))
        return 0;
    exchange.elapsed_ms = elapsed;
    exchange.state.store(PingState::replied);
    pbuf_free(packet);
    return 1;
}

void resolved(const char*, const ip_addr_t* address, void*) {
    exchange.dns_pending = false;
    if (exchange.cancelled.load()) {
        cancel();
        return;
    }
    if (address == nullptr || !IP_IS_V4(address)) {
        fail();
        return;
    }
    exchange.address = *address;
    format_ipv4(exchange.address_text, ip_2_ip4(address));
    exchange.socket = raw_new(IP_PROTO_ICMP);
    if (exchange.socket == nullptr) {
        fail();
        return;
    }
    raw_recv(exchange.socket, received, nullptr);
    exchange.state.store(PingState::ready);
}

/** @brief Mutates DNS/raw state only in lwIP's core context or under Pico's lock. */
void advance(void*) {
    if (exchange.cancelled.load()) {
        cancel();
    } else if (exchange.state.load() == PingState::resolving && !exchange.started) {
        exchange.started = exchange.dns_pending = true;
        const auto result =
            dns_gethostbyname(exchange.host.data(), &exchange.address, resolved, nullptr);
        if (result == ERR_OK)
            resolved(nullptr, &exchange.address, nullptr);
        else if (result != ERR_INPROGRESS)
            resolved(nullptr, nullptr, nullptr);
    } else if (exchange.send_requested.load()) {
        exchange.state.store(PingState::sending);
        exchange.echo = ping_packet::request(exchange.next_token, exchange.next_sequence);
        auto* packet = pbuf_alloc(PBUF_IP, static_cast<u16_t>(exchange.echo.size()), PBUF_RAM);
        exchange.sent_at = monotonic_milliseconds();
        const bool sent = packet != nullptr &&
                          pbuf_take(packet, exchange.echo.data(), exchange.echo.size()) == ERR_OK &&
                          raw_sendto(exchange.socket, packet, &exchange.address) == ERR_OK;
        if (packet != nullptr)
            pbuf_free(packet);
        if (sent)
            exchange.state.store(PingState::waiting);
        else
            fail();
        exchange.send_requested.store(false);
    }
    exchange.queued.store(false);
}

void schedule() {
    if (exchange.queued.load())
        return;
    exchange.queued.store(true);
#if defined(PICO_ON_DEVICE)
    cyw43_arch_lwip_begin();
    advance(nullptr);
    cyw43_arch_lwip_end();
#else
    if (tcpip_callback_with_block(advance, nullptr, 0) != ERR_OK)
        exchange.queued.store(false);
#endif
}
} // namespace

NetworkResult ping_start(std::string_view host) {
    if (exchange.state.load() != PingState::idle || exchange.queued.load())
        return NetworkResult::busy;
    if (host.empty() || host.size() >= exchange.host.size())
        return NetworkResult::failed;
    exchange.host.fill('\0');
    std::memcpy(exchange.host.data(), host.data(), host.size());
    exchange.address_text[0] = '\0';
    exchange.started = false;
    exchange.send_requested.store(false);
    exchange.cancelled.store(false);
    exchange.state.store(PingState::resolving);
    schedule();
    return NetworkResult::success;
}

NetworkResult ping_send(std::uint32_t token, std::uint16_t sequence) {
    const auto state = exchange.state.load();
    if (exchange.queued.load() || exchange.cancelled.load() || exchange.send_requested.load() ||
        (state != PingState::ready && state != PingState::replied && state != PingState::waiting))
        return NetworkResult::busy;
    // The core owns echo identity; receive callbacks may still be finishing an old reply.
    exchange.next_token = token;
    exchange.next_sequence = sequence;
    exchange.send_requested.store(true);
    schedule();
    return NetworkResult::success;
}

PingSnapshot ping_poll() {
    if ((exchange.cancelled.load() && exchange.state.load() != PingState::idle) ||
        exchange.send_requested.load() ||
        (exchange.state.load() == PingState::resolving && !exchange.queued.load()))
        schedule();
    PingSnapshot result{};
    result.state = exchange.state.load();
    // Do not expose partially initialized or callback-owned fields.
    if (exchange.queued.load() || exchange.send_requested.load()) {
        result.state = exchange.cancelled.load() ? PingState::resolving : PingState::sending;
        return result;
    }
    if (result.state == PingState::ready || result.state == PingState::waiting ||
        result.state == PingState::replied) {
        std::memcpy(result.address, exchange.address_text, sizeof(result.address));
        result.sent_at = exchange.sent_at;
        if (result.state == PingState::replied)
            result.elapsed_ms = exchange.elapsed_ms;
    }
    return result;
}

void ping_cancel() {
    exchange.cancelled.store(true);
    schedule();
}
} // namespace newmota::platform
