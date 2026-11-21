/*
  dhcp_server.cpp - Pico SDK SoftAP DHCP service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/*
  dhcp_server.cpp - Pico SDK SoftAP DHCP service

  Adapted from the DHCP server in raspberrypi/pico-examples, which is based
  on the MicroPython DHCP server and distributed under the MIT License.
  Copyright (c) 2018-2019 Damien P. George
  Copyright (C) 2026 Christian Baars and Newmota contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

#include "dhcp_server.h"

extern "C" {
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
}

#include "pico/time.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace newmota::platform::pico {
namespace {

constexpr std::uint8_t kDhcpDiscover = 1U;
constexpr std::uint8_t kDhcpOffer = 2U;
constexpr std::uint8_t kDhcpRequest = 3U;
constexpr std::uint8_t kDhcpNak = 6U;
constexpr std::uint8_t kDhcpRelease = 7U;
constexpr std::uint8_t kDhcpAck = 5U;
constexpr std::uint8_t kOptionPad = 0U;
constexpr std::uint8_t kOptionSubnetMask = 1U;
constexpr std::uint8_t kOptionRouter = 3U;
constexpr std::uint8_t kOptionRequestedIp = 50U;
constexpr std::uint8_t kOptionLeaseTime = 51U;
constexpr std::uint8_t kOptionMessageType = 53U;
constexpr std::uint8_t kOptionServerId = 54U;
constexpr std::uint8_t kOptionEnd = 255U;
constexpr std::uint16_t kDhcpServerPort = 67U;
constexpr std::uint16_t kDhcpClientPort = 68U;
constexpr std::size_t kDhcpFixedSize = 236U;
constexpr std::size_t kMaximumLeases = 8U;
constexpr std::uint8_t kFirstLeaseHost = 16U;
constexpr std::uint32_t kLeaseDurationMilliseconds = 24U * 60U * 60U * 1000U;
constexpr std::uint32_t kOfferDurationMilliseconds = 60U * 1000U;
constexpr std::array<std::uint8_t, 4> kMagicCookie{99U, 130U, 83U, 99U};

struct DhcpPacket final {
    std::uint8_t operation{};
    std::uint8_t hardware_type{};
    std::uint8_t hardware_length{};
    std::uint8_t hops{};
    std::array<std::uint8_t, 4> transaction{};
    std::array<std::uint8_t, 2> seconds{};
    std::array<std::uint8_t, 2> flags{};
    std::array<std::uint8_t, 4> client_address{};
    std::array<std::uint8_t, 4> offered_address{};
    std::array<std::uint8_t, 4> server_address{};
    std::array<std::uint8_t, 4> relay_address{};
    std::array<std::uint8_t, 16> client_hardware_address{};
    std::array<std::uint8_t, 64> server_name{};
    std::array<std::uint8_t, 128> boot_file{};
    std::array<std::uint8_t, 312> options{};
};

static_assert(sizeof(DhcpPacket) == 548U, "DHCP BOOTP packet layout changed");

struct Lease final {
    std::array<std::uint8_t, 6> hardware_address{};
    std::uint32_t expires_at{};
    bool occupied{};
};

struct Server final {
    udp_pcb* pcb{};
    netif* interface{};
    std::array<std::uint8_t, 4> address{};
    std::array<std::uint8_t, 4> netmask{};
    std::array<Lease, kMaximumLeases> leases{};
};

Server server;

struct Option final {
    const std::uint8_t* value{};
    std::size_t size{};
};

/** @brief Finds one option after checking the DHCP cookie and every boundary. */
bool find_option(const std::uint8_t* const options, const std::size_t size,
    const std::uint8_t wanted, Option& result) {
    result = {};
    if (options == nullptr || size < kMagicCookie.size() + 1U ||
        std::memcmp(options, kMagicCookie.data(), kMagicCookie.size()) != 0) {
        return false;
    }
    std::size_t offset = kMagicCookie.size();
    while (offset < size) {
        const auto code = options[offset++];
        if (code == kOptionEnd)
            return false;
        if (code == kOptionPad)
            continue;
        if (offset >= size)
            return false;
        const auto option_size = static_cast<std::size_t>(options[offset++]);
        if (option_size > size - offset)
            return false;
        if (code == wanted) {
            result = {options + offset, option_size};
            return true;
        }
        offset += option_size;
    }
    return false;
}

/** @brief Rejects a malformed option stream before callers inspect fields. */
bool valid_options(const std::uint8_t* const options, const std::size_t size) {
    if (options == nullptr || size < kMagicCookie.size() + 1U ||
        std::memcmp(options, kMagicCookie.data(), kMagicCookie.size()) != 0) {
        return false;
    }
    std::size_t offset = kMagicCookie.size();
    while (offset < size) {
        const auto code = options[offset++];
        if (code == kOptionEnd)
            return true;
        if (code == kOptionPad)
            continue;
        if (offset >= size)
            return false;
        const auto option_size = static_cast<std::size_t>(options[offset++]);
        if (option_size > size - offset)
            return false;
        offset += option_size;
    }
    return false;
}

/** @brief Adds one bounded DHCP option to the response packet. */
bool append_option(DhcpPacket& packet, std::size_t& offset, const std::uint8_t code,
    const std::uint8_t* const value, const std::size_t size) {
    if (size > 255U || offset > packet.options.size() ||
        size + 2U > packet.options.size() - offset) {
        return false;
    }
    packet.options[offset++] = code;
    packet.options[offset++] = static_cast<std::uint8_t>(size);
    if (size != 0U) {
        std::memcpy(packet.options.data() + offset, value, size);
        offset += size;
    }
    return true;
}

/** @brief Expires stale leases using wrapping-safe Pico monotonic time. */
std::uint32_t expire_leases() {
    const auto now = static_cast<std::uint32_t>(to_ms_since_boot(get_absolute_time()));
    for (auto& lease : server.leases) {
        if (lease.occupied && static_cast<std::int32_t>(now - lease.expires_at) >= 0) {
            lease = {};
        }
    }
    return now;
}

/** @brief Returns the active lease owned by one client, when present. */
int lease_for_client(const std::uint8_t* const hardware_address) {
    for (std::size_t index = 0U; index < server.leases.size(); ++index) {
        const auto& lease = server.leases[index];
        if (lease.occupied &&
            std::memcmp(lease.hardware_address.data(), hardware_address, 6U) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

/** @brief Maps a requested address into this server's small AP address pool. */
int requested_lease(const std::uint8_t* const address) {
    if (std::memcmp(address, server.address.data(), 3U) != 0 || address[3] < kFirstLeaseHost ||
        address[3] >= kFirstLeaseHost + kMaximumLeases) {
        return -1;
    }
    return static_cast<int>(address[3] - kFirstLeaseHost);
}

int free_lease() {
    for (std::size_t index = 0U; index < server.leases.size(); ++index) {
        if (!server.leases[index].occupied)
            return static_cast<int>(index);
    }
    return -1;
}

/** @brief Commits an address only after its slot has been validated. */
void assign_lease(const int index, const std::uint8_t* const hardware_address,
    const std::uint32_t now, const bool offered) {
    auto& lease = server.leases[static_cast<std::size_t>(index)];
    std::copy_n(hardware_address, lease.hardware_address.size(), lease.hardware_address.begin());
    lease.expires_at = now + (offered ? kOfferDurationMilliseconds : kLeaseDurationMilliseconds);
    lease.occupied = true;
}

/** @brief Reuses a client's current lease before considering its requested IP. */
int offer_lease(const std::uint8_t* const hardware_address,
    const std::uint8_t* const requested_address, const std::uint32_t now) {
    int selected = lease_for_client(hardware_address);
    if (selected < 0 && requested_address != nullptr) {
        const auto requested = requested_lease(requested_address);
        if (requested >= 0 && !server.leases[static_cast<std::size_t>(requested)].occupied) {
            selected = requested;
        }
    }
    if (selected < 0)
        selected = free_lease();
    if (selected < 0)
        return -1;
    assign_lease(selected, hardware_address, now, true);
    return selected;
}

/** @brief Validates the requested slot before committing a DHCP lease. */
int acknowledge_lease(const std::uint8_t* const hardware_address,
    const std::uint8_t* const requested_address, const std::uint32_t now) {
    const auto existing = lease_for_client(hardware_address);
    int selected = existing;
    if (requested_address != nullptr) {
        selected = requested_lease(requested_address);
        if (selected < 0 || (existing >= 0 && existing != selected))
            return -1;
        const auto& lease = server.leases[static_cast<std::size_t>(selected)];
        if (lease.occupied &&
            std::memcmp(lease.hardware_address.data(), hardware_address, 6U) != 0) {
            return -1;
        }
    } else if (selected < 0) {
        return -1;
    }
    assign_lease(selected, hardware_address, now, false);
    return selected;
}

/** @brief Frees only an address currently leased to the releasing client. */
void release_lease(
    const std::uint8_t* const hardware_address, const std::uint8_t* const client_address) {
    const auto selected = lease_for_client(hardware_address);
    if (selected < 0)
        return;
    if (std::any_of(client_address, client_address + 4U,
            [](const std::uint8_t byte) { return byte != 0U; })) {
        auto expected = server.address;
        expected[3] =
            static_cast<std::uint8_t>(kFirstLeaseHost + static_cast<std::size_t>(selected));
        if (std::memcmp(expected.data(), client_address, expected.size()) != 0)
            return;
    }
    server.leases[static_cast<std::size_t>(selected)] = {};
}

/** @brief Encodes the small fixed set of options used by this AP. */
bool make_reply_options(
    DhcpPacket& packet, const std::uint8_t reply_type, std::size_t& option_offset) {
    packet.options.fill(0U);
    std::copy(kMagicCookie.begin(), kMagicCookie.end(), packet.options.begin());
    option_offset = kMagicCookie.size();
    const std::array<std::uint8_t, 1> message_value{reply_type};
    constexpr std::array<std::uint8_t, 4> lease_value{0U, 1U, 81U, 128U};
    if (!append_option(packet, option_offset, kOptionMessageType, message_value.data(),
            message_value.size()) ||
        !append_option(
            packet, option_offset, kOptionServerId, server.address.data(), server.address.size())) {
        return false;
    }
    if (reply_type != kDhcpNak && (!append_option(packet, option_offset, kOptionSubnetMask,
                                       server.netmask.data(), server.netmask.size()) ||
                                      !append_option(packet, option_offset, kOptionRouter,
                                          server.address.data(), server.address.size()) ||
                                      !append_option(packet, option_offset, kOptionLeaseTime,
                                          lease_value.data(), lease_value.size()))) {
        return false;
    }
    if (option_offset >= packet.options.size())
        return false;
    packet.options[option_offset++] = kOptionEnd;
    return true;
}

/** @brief Replies through the interface to which the DHCP server was bound. */
void send_reply(Server& state, DhcpPacket& packet, const std::size_t option_offset) {
    if (state.pcb == nullptr || state.interface == nullptr)
        return;
    const auto packet_size = static_cast<std::uint16_t>(kDhcpFixedSize + option_offset);
    auto* reply = pbuf_alloc(PBUF_TRANSPORT, packet_size, PBUF_RAM);
    if (reply == nullptr)
        return;
    if (pbuf_take(reply, &packet, packet_size) == ERR_OK) {
        ip_addr_t broadcast{};
        IP_ADDR4(&broadcast, 255U, 255U, 255U, 255U);
        (void)udp_sendto_if(state.pcb, reply, &broadcast, kDhcpClientPort, state.interface);
    }
    pbuf_free(reply);
}

/** @brief Answers the bounded DHCP exchange received on the AP netif. */
void receive_dhcp(
    void* const context, udp_pcb*, pbuf* const payload, const ip_addr_t*, const u16_t) {
    auto& state = *static_cast<Server*>(context);
    if (payload == nullptr)
        return;
    DhcpPacket packet{};
    const auto copied = pbuf_copy_partial(payload, &packet, sizeof(packet), 0U);
    if (copied < kDhcpFixedSize + kMagicCookie.size() + 3U || packet.operation != 1U ||
        packet.hardware_type != 1U || packet.hardware_length != 6U) {
        pbuf_free(payload);
        return;
    }

    const auto option_bytes = copied - kDhcpFixedSize;
    Option message_type{};
    if (!valid_options(packet.options.data(), option_bytes) ||
        !find_option(packet.options.data(), option_bytes, kOptionMessageType, message_type) ||
        message_type.size != 1U) {
        pbuf_free(payload);
        return;
    }

    const auto now = expire_leases();
    int lease_index = -1;
    std::uint8_t reply_type = 0U;
    if (message_type.value[0] == kDhcpDiscover) {
        Option requested{};
        const auto found =
            find_option(packet.options.data(), option_bytes, kOptionRequestedIp, requested);
        if (found && requested.size != 4U) {
            pbuf_free(payload);
            return;
        }
        lease_index = offer_lease(
            packet.client_hardware_address.data(), found ? requested.value : nullptr, now);
        reply_type = kDhcpOffer;
    } else if (message_type.value[0] == kDhcpRequest || message_type.value[0] == kDhcpRelease) {
        Option server_id{};
        if (find_option(packet.options.data(), option_bytes, kOptionServerId, server_id) &&
            (server_id.size != state.address.size() ||
                std::memcmp(server_id.value, state.address.data(), state.address.size()) != 0)) {
            pbuf_free(payload);
            return; // The client selected another DHCP server.
        }

        if (message_type.value[0] == kDhcpRelease) {
            release_lease(packet.client_hardware_address.data(), packet.client_address.data());
            pbuf_free(payload);
            return;
        }

        Option requested{};
        const auto has_requested =
            find_option(packet.options.data(), option_bytes, kOptionRequestedIp, requested);
        if (has_requested && requested.size != 4U) {
            pbuf_free(payload);
            return;
        }
        const auto has_client_address = std::any_of(packet.client_address.begin(),
            packet.client_address.end(), [](const std::uint8_t byte) { return byte != 0U; });
        const auto* requested_address = has_requested        ? requested.value
                                        : has_client_address ? packet.client_address.data()
                                                             : nullptr;
        lease_index =
            acknowledge_lease(packet.client_hardware_address.data(), requested_address, now);
        reply_type = lease_index >= 0 ? kDhcpAck : kDhcpNak;
    } else {
        pbuf_free(payload);
        return;
    }
    pbuf_free(payload);
    if (reply_type == kDhcpOffer && lease_index < 0)
        return;

    packet.operation = 2U;
    packet.offered_address.fill(0U);
    packet.server_address.fill(0U);
    if (lease_index >= 0) {
        std::copy(state.address.begin(), state.address.end(), packet.offered_address.begin());
        packet.offered_address[3] =
            static_cast<std::uint8_t>(kFirstLeaseHost + static_cast<std::size_t>(lease_index));
    } else {
        packet.client_address.fill(0U);
    }
    std::size_t option_offset = 0U;
    if (make_reply_options(packet, reply_type, option_offset)) {
        send_reply(state, packet, option_offset);
    }
}

} // namespace

/** @brief Creates one DHCP server PCB bound exclusively to the Pico AP netif. */
bool start_dhcp_server(struct netif* const access_point) {
    if (access_point == nullptr || server.pcb != nullptr)
        return false;
    server.interface = access_point;
    std::memcpy(server.address.data(), &netif_ip4_addr(access_point)->addr, server.address.size());
    std::memcpy(
        server.netmask.data(), &netif_ip4_netmask(access_point)->addr, server.netmask.size());
    if (std::all_of(server.address.begin(), server.address.end(),
            [](const std::uint8_t byte) { return byte == 0U; })) {
        server.interface = nullptr;
        return false;
    }

    server.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (server.pcb == nullptr || udp_bind(server.pcb, IP_ADDR_ANY, kDhcpServerPort) != ERR_OK) {
        if (server.pcb != nullptr)
            udp_remove(server.pcb);
        server.pcb = nullptr;
        server.interface = nullptr;
        return false;
    }
    udp_bind_netif(server.pcb, access_point);
    udp_recv(server.pcb, receive_dhcp, &server);
    return true;
}

/** @brief Removes the DHCP PCB while its AP netif is still installed. */
void stop_dhcp_server() {
    if (server.pcb != nullptr)
        udp_remove(server.pcb);
    server = {};
}

} // namespace newmota::platform::pico
