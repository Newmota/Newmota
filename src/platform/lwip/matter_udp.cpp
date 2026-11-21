/*
  matter_udp.cpp - lwIP IPv6 UDP sockets for Matter and DNS-SD

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/matter_udp.h"

extern "C" {
#include "lwip/err.h"
#include "lwip/ip6_addr.h"
#include "lwip/mld6.h"
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

#if !defined(LWIP_IPV6_SCOPES)
// lwIP 2.0 (the Beken BDK) predates IPv6 zones. Peers carry zone 0, and sends
// leave through the active station link instead of the default route.
#define ip6_addr_zone(address) 0U
#define ip6_addr_set_zone(address, zone) ((void)(address), (void)(zone))
#define ip6_addr_assign_zone(address, type, link) ((void)(address), (void)(link))
#define NETIF_FOREACH(link) for ((link) = netif_list; (link) != nullptr; (link) = (link)->next)
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace newmota::platform {
namespace {
constexpr std::uint16_t kMatterPort = 5540;
constexpr std::uint16_t kMdnsPort = 5353;
constexpr std::size_t kMaxMatterDatagram = 1232;
constexpr std::size_t kMaxMdnsDatagram = 512;
constexpr std::size_t kQueueSlots = 6;
constexpr std::size_t kMaxQueuedBytes = 4096;

udp_pcb* matter_socket{};
udp_pcb* mdns_socket{};
netif* joined_link{};
std::atomic<bool> socket_ready{false};
std::atomic<bool> maintenance_queued{false};

// Single producer on the lwIP core, single consumer in the application context.
std::atomic<UdpDatagram*> queue[kQueueSlots]{};
std::size_t queue_write{};
std::size_t queue_read{};
std::atomic<std::size_t> queued_bytes{0};

// A sequence lock: odd while the lwIP core rewrites the snapshot.
std::atomic<std::uint32_t> address_sequence{0};
std::array<std::array<std::uint8_t, 16>, kMatterAddressCount> address_snapshot{};
std::size_t address_count{};
std::uint32_t address_generation{};

/** @brief Runs a function on the lwIP core: queued on RTOS targets, under its lock on Pico. */
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

/** @brief Returns the group address as an lwIP value, scoped to the link. */
ip_addr_t mdns_group(netif* const link) {
    ip_addr_t group{};
    IP_ADDR6(&group, PP_HTONL(0xFF020000UL), 0, 0, PP_HTONL(0xFBUL));
    ip6_addr_assign_zone(ip_2_ip6(&group), IP6_MULTICAST, link);
    return group;
}

/** @brief Finds the first link with a valid link-local address. */
netif* active_link() {
    netif* link = nullptr;
    NETIF_FOREACH(link) {
        if (!netif_is_up(link) || !netif_is_link_up(link) || (link->flags & NETIF_FLAG_MLD6) == 0U)
            continue;
        if (ip6_addr_isvalid(netif_ip6_addr_state(link, 0)))
            return link;
    }
    return nullptr;
}

/** @brief Copies a datagram out of the stack; a full queue drops it, as UDP allows. */
void received(void* const context, udp_pcb*, pbuf* const input, const ip_addr_t* const address,
    const u16_t port) {
    const bool mdns = context != nullptr;
    const std::size_t size = input->tot_len;
    std::atomic<UdpDatagram*>& slot = queue[queue_write];
    if (!IP_IS_V6(address) || size == 0U || size > (mdns ? kMaxMdnsDatagram : kMaxMatterDatagram) ||
        queued_bytes.load() + size > kMaxQueuedBytes || slot.load() != nullptr) {
        pbuf_free(input);
        return;
    }
    auto* const datagram = static_cast<UdpDatagram*>(std::malloc(sizeof(UdpDatagram) + size));
    if (datagram == nullptr) {
        pbuf_free(input);
        return;
    }
    new (datagram) UdpDatagram{};
    std::memcpy(datagram->peer.address.data(), ip_2_ip6(address)->addr, 16);
    datagram->peer.port = port;
    datagram->peer.zone = ip6_addr_zone(ip_2_ip6(address));
    datagram->mdns = mdns;
    datagram->size = static_cast<std::uint16_t>(size);
    pbuf_copy_partial(input, datagram->data, static_cast<u16_t>(size), 0);
    pbuf_free(input);
    queued_bytes.fetch_add(size);
    slot.store(datagram);
    queue_write = (queue_write + 1U) % kQueueSlots;
}

/** @brief Publishes the link's addresses for the application context. */
void refresh_addresses(netif* const link) {
    std::array<std::array<std::uint8_t, 16>, kMatterAddressCount> current{};
    std::size_t count = 0;
    if (link != nullptr) {
        for (int index = 0; index < LWIP_IPV6_NUM_ADDRESSES && count < current.size(); ++index) {
            if (ip6_addr_isvalid(netif_ip6_addr_state(link, index)))
                std::memcpy(current[count++].data(), netif_ip6_addr(link, index)->addr, 16);
        }
    }
    if (count == address_count && current == address_snapshot)
        return;
    address_sequence.fetch_add(1U);
    address_snapshot = current;
    address_count = count;
    ++address_generation;
    address_sequence.fetch_add(1U);
}

/** @brief Binds one socket on the lwIP core. */
udp_pcb* bind_socket(const std::uint16_t port, void* const context) {
    udp_pcb* const socket = udp_new_ip_type(IPADDR_TYPE_V6);
    if (socket == nullptr)
        return nullptr;
    ip_set_option(socket, SOF_REUSEADDR);
    if (udp_bind(socket, IP6_ADDR_ANY, port) != ERR_OK) {
        udp_remove(socket);
        return nullptr;
    }
    udp_recv(socket, received, context);
    return socket;
}

/** @brief Reconciles sockets and the DNS-SD group with the current link. */
void maintain(void*) {
    maintenance_queued.store(false);
    if (matter_socket == nullptr)
        matter_socket = bind_socket(kMatterPort, nullptr);
    if (mdns_socket == nullptr) {
        mdns_socket = bind_socket(kMdnsPort, &mdns_socket);
        if (mdns_socket != nullptr)
            mdns_socket->ttl = 255;
#if LWIP_MULTICAST_TX_OPTIONS
        if (mdns_socket != nullptr)
            udp_set_multicast_ttl(mdns_socket, 255);
#endif
    }
    netif* const link = active_link();
    if (link != joined_link) {
        if (joined_link != nullptr) {
            const ip_addr_t group = mdns_group(joined_link);
            (void)mld6_leavegroup_netif(joined_link, ip_2_ip6(&group));
        }
        joined_link = nullptr;
        if (link != nullptr) {
            const ip_addr_t group = mdns_group(link);
            if (mld6_joingroup_netif(link, ip_2_ip6(&group)) == ERR_OK)
                joined_link = link;
        }
    }
    refresh_addresses(link);
    socket_ready.store(
        matter_socket != nullptr && mdns_socket != nullptr && joined_link != nullptr);
}

/** @brief Removes both sockets on the lwIP core. */
void close_sockets(void*) {
    if (joined_link != nullptr) {
        const ip_addr_t group = mdns_group(joined_link);
        (void)mld6_leavegroup_netif(joined_link, ip_2_ip6(&group));
        joined_link = nullptr;
    }
    for (udp_pcb** socket : {&matter_socket, &mdns_socket}) {
        if (*socket != nullptr) {
            udp_remove(*socket);
            *socket = nullptr;
        }
    }
    socket_ready.store(false);
    refresh_addresses(nullptr);
}

/** @brief A copied outgoing datagram, freed on the lwIP core. */
struct SendJob {
    ip_addr_t address{};
    std::uint16_t port{};
    bool mdns{};
    bool multicast{};
    std::uint16_t size{};
    std::uint8_t data[1]{};
};

/** @brief Sends one copied datagram on the lwIP core. */
void send_on_core(void* const context) {
    auto* const job = static_cast<SendJob*>(context);
    udp_pcb* const socket = job->mdns ? mdns_socket : matter_socket;
    netif* const link = active_link();
    if (socket != nullptr && link != nullptr) {
        pbuf* const output = pbuf_alloc(PBUF_TRANSPORT, job->size, PBUF_RAM);
        if (output != nullptr) {
            std::memcpy(output->payload, job->data, job->size);
            if (job->multicast) {
                const ip_addr_t group = mdns_group(link);
                (void)udp_sendto_if(socket, output, &group, kMdnsPort, link);
            } else {
#if defined(LWIP_IPV6_SCOPES)
                (void)udp_sendto(socket, output, &job->address, job->port);
#else
                (void)udp_sendto_if(socket, output, &job->address, job->port, link);
#endif
            }
            pbuf_free(output);
        }
    }
    std::free(job);
}

/** @brief Copies a datagram and hands it to the lwIP core. */
bool queue_send(
    const UdpPeer* const peer, const void* const data, const std::size_t size, const bool mdns) {
    if (!socket_ready.load() || size == 0U || size > kMaxMatterDatagram)
        return false;
    auto* const job = static_cast<SendJob*>(std::malloc(sizeof(SendJob) + size));
    if (job == nullptr)
        return false;
    new (job) SendJob{};
    job->mdns = mdns;
    job->multicast = peer == nullptr;
    job->size = static_cast<std::uint16_t>(size);
    std::memcpy(job->data, data, size);
    if (peer != nullptr) {
        IP_SET_TYPE_VAL(job->address, IPADDR_TYPE_V6);
        std::memcpy(ip_2_ip6(&job->address)->addr, peer->address.data(), 16);
        ip6_addr_set_zone(ip_2_ip6(&job->address), peer->zone);
        job->port = peer->port;
    }
    if (!on_core(send_on_core, job)) {
        std::free(job);
        return false;
    }
    return true;
}
} // namespace

void matter_udp_poll() {
    if (maintenance_queued.exchange(true))
        return;
    if (!on_core(maintain, nullptr))
        maintenance_queued.store(false);
}

bool matter_udp_ready() {
    return socket_ready.load();
}

void matter_udp_close() {
    (void)on_core(close_sockets, nullptr);
    while (UdpDatagram* const datagram = matter_udp_receive())
        matter_udp_release(datagram);
}

UdpDatagram* matter_udp_receive() {
    UdpDatagram* const datagram = queue[queue_read].load();
    if (datagram == nullptr)
        return nullptr;
    queued_bytes.fetch_sub(datagram->size);
    queue[queue_read].store(nullptr);
    queue_read = (queue_read + 1U) % kQueueSlots;
    return datagram;
}

void matter_udp_release(UdpDatagram* const datagram) {
    std::free(datagram);
}

bool matter_udp_send(const UdpPeer& peer, const void* const data, const std::size_t size) {
    return queue_send(&peer, data, size, false);
}

bool matter_mdns_send(const UdpPeer* const peer, const void* const data, const std::size_t size) {
    return queue_send(peer, data, size, true);
}

std::size_t matter_ipv6_addresses(
    std::array<std::array<std::uint8_t, 16>, kMatterAddressCount>& addresses,
    std::uint32_t& generation) {
    std::size_t count = 0;
    std::uint32_t before = 0;
    do {
        before = address_sequence.load();
        addresses = address_snapshot;
        count = address_count;
        generation = address_generation;
    } while ((before & 1U) != 0U || before != address_sequence.load());
    return count;
}

} // namespace newmota::platform
