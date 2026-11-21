/*
  ntp_server.cpp - independent bounded raw-lwIP NTP listener

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file ntp_server.cpp
 * @brief Owns UDP123 and timestamps on the lwIP core, independently of client DNS/UDP.
 */
#include "newmota/platform/ntp_server.h"
#include "newmota/platform/platform.h"
#include "newmota/platform/ntp_packet.h"

extern "C" {
#include "lwip/err.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#if !defined(PICO_ON_DEVICE)
#include "lwip/tcpip.h"
#endif
}
#if defined(PICO_ON_DEVICE)
#include "pico/cyw43_arch.h"
#endif
#include <atomic>

namespace newmota::platform {
namespace {
struct Listener final {
    std::atomic<bool> queued{false}, enabled{false}, listening{false}, failed{false};
    std::atomic<std::uint32_t> requests{0}, replies{0}, dropped{0};
    NtpServerClock staging{}, clock{};
    udp_pcb* socket{};
    std::uint32_t last_reply{};
    bool sent{};
} listener;

/** @brief Single core writer; avoids unsupported atomic RMW helpers on small targets. */
void count(std::atomic<std::uint32_t>& value) {
    const auto previous = value.load();
    if (previous != UINT32_MAX)
        value.store(previous + 1U);
}

void received(void*, udp_pcb* socket, pbuf* input, const ip_addr_t* address, u16_t port) {
    if (input == nullptr)
        return;
    const auto arrival = monotonic_milliseconds();
    count(listener.requests);
    ntp_packet::Packet bytes{};
    const auto* destination = ip_current_dest_addr();
    const ip_addr_t peer = *address;
    const ip_addr_t local = *destination;
    auto* interface = ip_current_netif();
    const bool unicast = IP_IS_V4(address) && IP_IS_V4(destination) && port != 0 &&
                         !ip_addr_isany(address) && !ip_addr_ismulticast(address) &&
                         !ip4_addr_isbroadcast(ip_2_ip4(address), ip_current_netif()) &&
                         !ip_addr_ismulticast(destination) &&
                         !ip4_addr_isbroadcast(ip_2_ip4(destination), ip_current_netif());
    const bool valid = listener.enabled.load() && unicast && input->tot_len == bytes.size() &&
                       pbuf_copy_partial(input, bytes.data(), static_cast<u16_t>(bytes.size()),
                           0) == bytes.size() &&
                       ntp_packet::request(bytes.data(), bytes.size());
    pbuf_free(input);
    // Four replies per second globally, including unsynchronized replies; no amplification.
    if (!valid ||
        (listener.sent && static_cast<std::uint32_t>(arrival - listener.last_reply) < 250U)) {
        count(listener.dropped);
        return;
    }
    auto* output = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(bytes.size()), PBUF_RAM);
    if (output == nullptr) {
        count(listener.dropped);
        return;
    }
    // Clock and reference metadata cannot change during this callback. Departure is sampled
    // after allocation, immediately before encoding/copying and the native UDP send.
    const auto departure = monotonic_milliseconds();
    bytes = ntp_packet::reply(bytes, listener.clock, arrival, departure);
    const bool sent = pbuf_take(output, bytes.data(), static_cast<u16_t>(bytes.size())) == ERR_OK &&
                      udp_sendto_if_src(socket, output, &peer, port, interface, &local) == ERR_OK;
    pbuf_free(output);
    if (sent) {
        listener.last_reply = departure;
        listener.sent = true;
        count(listener.replies);
    } else {
        count(listener.dropped);
    }
}

/** @brief Owns live clock/PCB on the lwIP core; staging is released only at the end. */
void advance(void*) {
    listener.clock = listener.staging;
    if (!listener.enabled.load()) {
        if (listener.socket != nullptr) {
            udp_remove(listener.socket);
            listener.socket = nullptr;
        }
        listener.listening.store(false);
        listener.failed.store(false);
        listener.sent = false;
    } else if (listener.socket == nullptr) {
        listener.socket = udp_new();
        const bool bound =
            listener.socket != nullptr && udp_bind(listener.socket, IP_ADDR_ANY, 123) == ERR_OK;
        if (bound) {
            udp_recv(listener.socket, received, nullptr);
        } else if (listener.socket != nullptr) {
            udp_remove(listener.socket);
            listener.socket = nullptr;
        }
        listener.listening.store(bound);
        listener.failed.store(!bound);
    }
    listener.queued.store(false);
}
} // namespace

void ntp_server_poll(const NtpServerClock& clock, bool connected) {
    listener.enabled.store(connected);
    if (listener.queued.load())
        return;
    listener.staging = clock;
    listener.queued.store(true);
#if defined(PICO_ON_DEVICE)
    cyw43_arch_lwip_begin();
    advance(nullptr);
    cyw43_arch_lwip_end();
#else
    if (tcpip_callback_with_block(advance, nullptr, 0) != ERR_OK)
        listener.queued.store(false);
#endif
}

NtpServerSnapshot ntp_server_snapshot() {
    return {listener.listening.load(), listener.failed.load(), listener.requests.load(),
        listener.replies.load(), listener.dropped.load()};
}
} // namespace newmota::platform
