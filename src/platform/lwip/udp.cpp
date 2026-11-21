/*
  udp.cpp - lwIP UDP request implementation for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/network.h"

// Some vendor lwIP port headers lack their own C++ linkage guards.
extern "C" {
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#if !defined(PICO_ON_DEVICE)
#include "lwip/tcpip.h"
#endif
}
#if defined(PICO_ON_DEVICE)
#include "pico/cyw43_arch.h"
#endif
#include <array>
#include <atomic>
#include <cstring>

namespace newmota::platform {
namespace {
enum class Phase : std::uint8_t { idle, starting, working, succeeded, failed };

struct Exchange final {
    std::atomic<Phase> phase{Phase::idle};
    std::atomic<bool> cancelled{false}, queued{false};
    std::array<char, 64> host{};
    std::array<std::uint8_t, 48> request{}, response{};
    std::size_t request_size{}, response_size{};
    std::uint16_t port{};
    ip_addr_t address{};
    udp_pcb* socket{};
    bool dns_pending{};
};

Exchange exchange;

/** @brief Completes on the lwIP core, publishing data after cleanup. */
void finish(const Phase result) {
    if (exchange.socket != nullptr) {
        udp_remove(exchange.socket);
        exchange.socket = nullptr;
    }
    // DNS cannot be cancelled: retain this slot until its callback drains.
    if (!exchange.dns_pending)
        exchange.phase.store(result);
}

/** @brief Accepts only complete replies from the resolved server and port. */
void received(void*, udp_pcb*, pbuf* packet, const ip_addr_t* address, const u16_t port) {
    if (packet == nullptr)
        return;
    if (!exchange.cancelled.load() && port == exchange.port &&
        ip_addr_cmp(address, &exchange.address) && packet->tot_len == exchange.response.size()) {
        exchange.response_size =
            pbuf_copy_partial(packet, exchange.response.data(), packet->tot_len, 0);
        finish(Phase::succeeded);
    }
    pbuf_free(packet);
}

/** @brief Sends the owned datagram after asynchronous DNS resolution. */
void resolved(const char*, const ip_addr_t* address, void*) {
    exchange.dns_pending = false;
    if (exchange.cancelled.load() || address == nullptr) {
        finish(Phase::failed);
        return;
    }
    exchange.address = *address;
    exchange.socket = udp_new();
    if (exchange.socket == nullptr) {
        finish(Phase::failed);
        return;
    }
    udp_recv(exchange.socket, received, nullptr);
    pbuf* packet = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(exchange.request_size), PBUF_RAM);
    const bool sent =
        packet != nullptr &&
        pbuf_take(packet, exchange.request.data(), exchange.request_size) == ERR_OK &&
        udp_sendto(exchange.socket, packet, &exchange.address, exchange.port) == ERR_OK;
    if (packet != nullptr)
        pbuf_free(packet);
    if (!sent)
        finish(Phase::failed);
}

/** @brief Advances native state only on the lwIP core or under Pico's lock. */
void advance(void*) {
    if (exchange.cancelled.load()) {
        finish(Phase::failed);
    } else if (exchange.phase.load() == Phase::starting) {
        exchange.phase.store(Phase::working);
        exchange.dns_pending = true;
        const auto result =
            dns_gethostbyname(exchange.host.data(), &exchange.address, resolved, nullptr);
        if (result == ERR_OK)
            resolved(nullptr, &exchange.address, nullptr);
        else if (result != ERR_INPROGRESS)
            resolved(nullptr, nullptr, nullptr);
    }
    exchange.queued.store(false);
}

/** @brief Requests core work without waiting for DNS or a datagram. */
void schedule() {
    // Only the application context schedules work; the core only clears queued.
    // Atomic loads/stores suffice here, avoiding byte-sized RMW helper routines
    // which are unavailable in the BL602 toolchain/runtime.
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

/** @brief Reports completion only after queued core work has finished. */
bool complete() {
    const auto phase = exchange.phase.load();
    return (phase == Phase::succeeded || phase == Phase::failed) && !exchange.queued.load();
}
} // namespace

/** @brief Copies one request into the fixed, single-exchange native state. */
NetworkResult udp_start(const std::string_view host, const std::uint16_t port,
    const std::uint8_t* request, const std::size_t size) {
    if (exchange.cancelled.load() && complete())
        exchange.phase.store(Phase::idle);
    if (exchange.phase.load() != Phase::idle) {
        if (exchange.cancelled.load())
            schedule();
        return NetworkResult::busy;
    }
    if (host.empty() || host.size() >= exchange.host.size() || request == nullptr || size == 0 ||
        size > exchange.request.size())
        return NetworkResult::failed;
    exchange.host = {};
    std::memcpy(exchange.host.data(), host.data(), host.size());
    std::memcpy(exchange.request.data(), request, size);
    exchange.request_size = size;
    exchange.port = port;
    exchange.cancelled.store(false);
    exchange.phase.store(Phase::starting);
    schedule();
    return NetworkResult::success;
}

/** @brief Returns a completed response without blocking the application loop. */
NetworkResult udp_poll(std::uint8_t* response, const std::size_t capacity, std::size_t& size,
    std::array<std::uint8_t, 4>* peer_ipv4) {
    size = 0;
    if (exchange.phase.load() == Phase::idle)
        return NetworkResult::failed;
    if (!complete()) {
        if (exchange.phase.load() == Phase::starting || exchange.cancelled.load())
            schedule();
        return NetworkResult::busy;
    }
    const bool success = exchange.phase.load() == Phase::succeeded && !exchange.cancelled.load() &&
                         response != nullptr && capacity >= exchange.response_size;
    if (success) {
        size = exchange.response_size;
        std::memcpy(response, exchange.response.data(), size);
        if (peer_ipv4 != nullptr) {
            *peer_ipv4 = {};
            if (IP_IS_V4(&exchange.address))
                std::memcpy(peer_ipv4->data(), ip_2_ip4(&exchange.address), peer_ipv4->size());
        }
    }
    exchange.phase.store(Phase::idle);
    return success ? NetworkResult::success : NetworkResult::failed;
}

/** @brief Defers cleanup to the core; callbacks never retain caller buffers. */
void udp_cancel() {
    if (exchange.phase.load() == Phase::idle)
        return;
    exchange.cancelled.store(true);
    schedule();
}
} // namespace newmota::platform
