/*
  matter_stack.cpp - Matter sessions, reliable messaging and transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_stack.h"

#include "services/matter/matter_wire.h"

#include "newmota_features.h"
#if USE_MATTER_BLE
#include "services/matter/matter_ble.h"
#endif

#include "newmota/log.h"
#include "newmota/platform/crypto_random.h"
#include "newmota/platform/matter_udp.h"
#if USE_MATTER_TCP
#include "newmota/platform/matter_tcp.h"
#endif

#include "bearssl.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace newmota::matter {
namespace {
constexpr std::size_t kSessions = 16; // Three CASE per fabric for five fabrics, plus PASE.
constexpr std::size_t kUnsecured = 4;
constexpr std::size_t kReliable = 4;
constexpr std::size_t kPendingAcks = 4;
constexpr std::size_t kFailures = 4;
constexpr unsigned kSessionsPerFabric = 3;
constexpr unsigned kMaxRetransmissions = 4; // MRP_MAX_TRANSMISSIONS 5, first send included.
constexpr std::uint32_t kRetransmitMilliseconds = 330; // Active interval with the spec's margin.
constexpr std::uint32_t kAckDelayMilliseconds = 200;
constexpr std::uint32_t kNetworkPollMilliseconds = 1000;
constexpr std::uint32_t kAnnounceSecondMilliseconds = 2000;
constexpr std::uint32_t kUnknownSessionLogMilliseconds = 10000;
constexpr std::size_t kMessageOverhead = 8U + kNodeIdSize + 10U + kMicSize;
constexpr int kUnsecuredBase = static_cast<int>(kSessions);
constexpr int kNoContext = -1;
constexpr std::uint16_t kStandaloneAck = 0x10;
constexpr std::uint32_t kMaxInitialCounter = 0x0FFFFFFFU;
// A stored session resumes above this many sends, so a restart can never reuse a nonce.
constexpr std::uint32_t kCounterReserve = 4096;
constexpr std::size_t kNodeSize = kNodeIdSize;

/** @brief Transports are data, not classes: each adds a value and its message limit. */
enum class Transport : std::uint8_t { udp, btp, tcp };

/** @brief Sliding replay window for one peer. */
struct Replay {
    std::uint32_t highest{};
    std::uint32_t seen{};
    bool started{};
};

struct Peer {
    platform::UdpPeer address{};
    Transport transport{Transport::udp};
    int connection{-1}; // the TCP connection, for Transport::tcp
    std::uint32_t last_used{};
    std::uint32_t send_counter{};
    std::uint32_t retransmit{kRetransmitMilliseconds};
    Replay replay{};
};

struct Session {
    bool used{};
    std::uint8_t generation{};
    Peer peer{};
    SessionParameters keys{};
    std::uint32_t reserved{};
};

struct Unsecured {
    bool used{};
    std::uint8_t generation{};
    Peer peer{};
    std::uint8_t node[kNodeIdSize]{};
    // An exchange this device started: node is then its own ephemeral id, sent as the source.
    bool initiator{};
};

struct ReliableSend {
    std::uint8_t* frame{};
    std::uint16_t size{};
    int context{kNoContext};
    std::uint16_t exchange{};
    std::uint32_t counter{};
    std::uint32_t deadline{};
    std::uint32_t interval{};
    std::uint8_t sends{};
};

struct PendingAck {
    bool used{};
    int context{kNoContext};
    std::uint16_t exchange{};
    bool initiator{};
    std::uint32_t counter{};
    std::uint32_t deadline{};
};

struct Failure {
    int context{kNoContext};
    std::uint16_t exchange{};
};

struct State {
    Session sessions[kSessions]{};
    Unsecured unsecured[kUnsecured]{};
    ReliableSend reliable[kReliable]{};
    PendingAck acks[kPendingAcks]{};
    Failure failures[kFailures]{};
    std::size_t failure_count{};
    std::uint16_t next_session_id{};
    std::uint32_t next_network_poll{};
    std::uint32_t address_generation{};
    std::uint32_t next_announce{};
    std::uint32_t next_unknown_log{};
    std::uint32_t dirty_fabrics{};
    std::uint8_t announces_left{};
    bool announce_now{};
    DnsRecords dns{};
    platform::UdpDatagram* current{};
    std::uint8_t transmit[kMaxFrameSize]{};
};

State* state{};

[[nodiscard]] bool reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

[[nodiscard]] std::uint32_t random_counter() {
    std::uint32_t value = 0;
    (void)platform::crypto_random(&value, sizeof(value));
    return (value & kMaxInitialCounter) + 1U;
}

/** @brief Adds the MRP backoff jitter of 0 to 25 percent. */
[[nodiscard]] std::uint32_t jittered(const std::uint32_t milliseconds) {
    std::uint8_t value = 0;
    (void)platform::crypto_random(&value, sizeof(value));
    return milliseconds + milliseconds * value / 1020U;
}

/** @brief Sends a finished message on the transport the peer is reached through. */
void transmit(const Peer& peer, const std::uint8_t* const data, const std::size_t size) {
    switch (peer.transport) {
    case Transport::udp:
        (void)platform::matter_udp_send(peer.address, data, size);
        break;
    case Transport::btp:
#if USE_MATTER_BLE
        (void)ble_link_send(data, size);
#endif
        break;
    case Transport::tcp:
#if USE_MATTER_TCP
        (void)platform::matter_tcp_send(peer.connection, data, size);
#endif
        break;
    }
}

/** @brief The largest frame a transport carries. */
[[nodiscard]] std::size_t frame_limit(const Transport transport) {
#if USE_MATTER_TCP
    if (transport == Transport::tcp)
        return platform::kMatterTcpMaxMessage;
#endif
    (void)transport;
    return kMaxFrameSize;
}

/** @brief Encodes slot and generation, so a stale handle cannot reach a reused slot. */
[[nodiscard]] int encode_context(const std::size_t slot, const std::uint8_t generation) {
    return static_cast<int>(slot) | (static_cast<int>(generation) << 8U);
}

[[nodiscard]] Session* find_session(const int context) {
    const auto slot = static_cast<std::size_t>(context & 0xFF);
    if (context < 0 || slot >= kSessions)
        return nullptr;
    Session& session = state->sessions[slot];
    return session.used && session.generation == (context >> 8) ? &session : nullptr;
}

[[nodiscard]] Unsecured* find_unsecured(const int context) {
    if (context < 0 || (context & 0xFF) < kUnsecuredBase)
        return nullptr;
    const auto slot = static_cast<std::size_t>((context & 0xFF) - kUnsecuredBase);
    if (slot >= kUnsecured)
        return nullptr;
    Unsecured& entry = state->unsecured[slot];
    return entry.used && entry.generation == (context >> 8) ? &entry : nullptr;
}

[[nodiscard]] Peer* find_peer(const int context) {
    if (Session* const session = find_session(context))
        return &session->peer;
    if (Unsecured* const entry = find_unsecured(context))
        return &entry->peer;
    return nullptr;
}

/** @brief Tests a counter without recording it. */
[[nodiscard]] bool replay_is_new(
    const Replay& replay, const std::uint32_t counter, const bool encrypted) {
    if (!replay.started || static_cast<std::int32_t>(counter - replay.highest) > 0)
        return true;
    const std::uint32_t behind = replay.highest - counter;
    if (behind == 0U)
        return false;
    if (behind > 32U)
        return !encrypted; // An unsecured peer may restart its counter.
    return (replay.seen & (1UL << (behind - 1U))) == 0U;
}

void replay_record(Replay& replay, const std::uint32_t counter) {
    if (!replay.started) {
        replay = {counter, 0, true};
        return;
    }
    const auto ahead = static_cast<std::int32_t>(counter - replay.highest);
    if (ahead > 0) {
        replay.seen = ahead >= 32 ? 0 : replay.seen << static_cast<unsigned>(ahead);
        if (ahead <= 32)
            replay.seen |= 1UL << (static_cast<unsigned>(ahead) - 1U);
        replay.highest = counter;
    } else if (const std::uint32_t behind = replay.highest - counter;
        behind >= 1U && behind <= 32U) {
        replay.seen |= 1UL << (behind - 1U);
    } else if (behind > 32U) {
        replay = {counter, 0, true};
    }
}

/** @brief Builds the nonce from the security flags, counter, and sender. */
void make_nonce(std::uint8_t (&nonce)[13], const std::uint8_t flags, const std::uint32_t counter,
    const std::uint8_t* const node) {
    nonce[0] = flags;
    for (unsigned index = 0; index < 4U; ++index)
        nonce[1U + index] = static_cast<std::uint8_t>(counter >> (index * 8U));
    std::memcpy(nonce + 5, node, kNodeIdSize);
}

/** @brief AES-CCM in place over a frame with 16-byte tag. */
[[nodiscard]] bool crypt(const std::uint8_t* const key, const std::uint8_t (&nonce)[13],
    const std::uint8_t* const aad, const std::size_t aad_size, std::uint8_t* const data,
    const std::size_t size, std::uint8_t* const tag, const bool encrypt) {
    br_aes_small_ctrcbc_keys aes;
    br_ccm_context ccm;
    br_aes_small_ctrcbc_init(&aes, key, 16);
    br_ccm_init(&ccm, &aes.vtable);
    if (br_ccm_reset(&ccm, nonce, sizeof(nonce), aad_size, size, kMicSize) == 0)
        return false;
    br_ccm_aad_inject(&ccm, aad, aad_size);
    br_ccm_flip(&ccm);
    br_ccm_run(&ccm, encrypt ? 1 : 0, data, size);
    if (encrypt) {
        br_ccm_get_tag(&ccm, tag);
        return true;
    }
    return br_ccm_check_tag(&ccm, tag) != 0;
}

/** @brief Builds and encrypts one message into an output buffer; returns its size or 0. */
[[nodiscard]] std::size_t build_message(const int context, ProtocolHeader& protocol,
    const std::uint8_t* const payload, const std::size_t size, std::uint32_t& counter,
    std::uint8_t* const output, const std::size_t capacity) {
    Session* const session = find_session(context);
    Unsecured* const entry = session == nullptr ? find_unsecured(context) : nullptr;
    if (session == nullptr && entry == nullptr)
        return 0;
    Peer& peer = session != nullptr ? session->peer : entry->peer;
    FrameHeader frame;
    frame.counter = peer.send_counter;
    if (session != nullptr) {
        frame.session_id = session->keys.peer_id;
    } else if (entry->initiator) {
        frame.has_source = true;
        std::memcpy(frame.source, entry->node, kNodeIdSize);
    } else {
        frame.has_destination = true;
        std::memcpy(frame.destination, entry->node, kNodeIdSize);
    }
    const std::size_t head = write_frame(output, capacity, frame);
    if (head == 0U)
        return 0;
    const std::size_t protocol_size = write_protocol(output + head, capacity - head, protocol);
    if (protocol_size == 0U)
        return 0;
    const std::size_t body = protocol_size + size;
    const std::size_t total = head + body + (session != nullptr ? kMicSize : 0U);
    if (total > capacity)
        return 0;
    if (size != 0U)
        std::memcpy(output + head + protocol_size, payload, size);
    if (session != nullptr) {
        std::uint8_t nonce[13];
        make_nonce(nonce, frame.security_flags, frame.counter, session->keys.local_node);
        if (!crypt(session->keys.encrypt_key, nonce, output, head, output + head, body,
                output + head + body, true))
            return 0;
    }
    counter = peer.send_counter++;
    // Asks for a new record while the stored range still covers this session.
    if (session != nullptr && session->keys.fabric != 0U &&
        static_cast<std::int32_t>(session->reserved - peer.send_counter) <
            static_cast<std::int32_t>(kCounterReserve / 2U))
        state->dirty_fabrics |= 1UL << (session->keys.fabric & 31U);
    return total;
}

void send_ack(const int context, const std::uint16_t exchange, const bool initiator,
    const std::uint32_t acknowledged) {
    Peer* const peer = find_peer(context);
    if (peer == nullptr)
        return;
    ProtocolHeader header;
    header.flags = static_cast<std::uint8_t>(kExchangeAck | (initiator ? kExchangeInitiator : 0U));
    header.opcode = kStandaloneAck;
    header.exchange = exchange;
    header.protocol = kProtocolSecureChannel;
    header.ack_counter = acknowledged;
    std::uint32_t counter = 0;
    const std::size_t size =
        build_message(context, header, nullptr, 0, counter, state->transmit, kMaxFrameSize);
    if (size != 0U)
        transmit(*peer, state->transmit, size);
}

void queue_ack(const int context, const std::uint16_t exchange, const bool initiator,
    const std::uint32_t counter, const std::uint32_t now) {
    PendingAck* slot = nullptr;
    for (PendingAck& ack : state->acks) {
        if (ack.used && ack.context == context && ack.exchange == exchange) {
            // A newer message on the exchange: the earlier one must not stay unacknowledged.
            send_ack(ack.context, ack.exchange, ack.initiator, ack.counter);
            slot = &ack;
            break;
        }
        if (!ack.used && slot == nullptr)
            slot = &ack;
    }
    if (slot == nullptr) {
        PendingAck& oldest = state->acks[0];
        send_ack(oldest.context, oldest.exchange, oldest.initiator, oldest.counter);
        slot = &oldest;
    }
    *slot = {true, context, exchange, initiator, counter, now + kAckDelayMilliseconds};
}

void release_reliable(ReliableSend& entry) {
    std::free(entry.frame);
    entry = {};
}

void forget_context(const int context) {
    for (ReliableSend& entry : state->reliable) {
        if (entry.frame != nullptr && entry.context == context)
            release_reliable(entry);
    }
    for (PendingAck& ack : state->acks) {
        if (ack.used && ack.context == context)
            ack = {};
    }
}

/** @brief Finds the slot of a repeat initiator, a free one, or the oldest. */
[[nodiscard]] int unsecured_context(const FrameHeader& header, const platform::UdpPeer& address,
    const Transport transport, const int connection, const std::uint32_t now) {
    Unsecured* chosen = nullptr;
    for (Unsecured& entry : state->unsecured) {
        if (entry.used && !entry.initiator && entry.peer.address.address == address.address &&
            std::memcmp(entry.node, header.source, kNodeIdSize) == 0) {
            chosen = &entry;
            break;
        }
        if (chosen == nullptr ||
            (chosen->used &&
                (!entry.used ||
                    static_cast<std::int32_t>(entry.peer.last_used - chosen->peer.last_used) < 0)))
            chosen = &entry;
    }
    const bool repeat = chosen->used && !chosen->initiator &&
                        chosen->peer.address.address == address.address &&
                        std::memcmp(chosen->node, header.source, kNodeIdSize) == 0;
    if (!repeat) {
        const int stale = encode_context(
            static_cast<std::size_t>(chosen - state->unsecured) + kSessions, chosen->generation);
        if (chosen->used)
            forget_context(stale);
        const auto generation = static_cast<std::uint8_t>(chosen->generation + 1U);
        *chosen = {};
        chosen->used = true;
        chosen->generation = generation;
        chosen->peer.send_counter = random_counter();
        std::memcpy(chosen->node, header.source, kNodeIdSize);
    }
    chosen->peer.address = address;
    chosen->peer.transport = transport;
    chosen->peer.connection = connection;
    chosen->peer.last_used = now;
    return encode_context(
        static_cast<std::size_t>(chosen - state->unsecured) + kSessions, chosen->generation);
}

/** @brief Finds the handshake this device started that a responder's message addresses. */
[[nodiscard]] int initiator_context(const FrameHeader& header) {
    for (std::size_t slot = 0; slot < kUnsecured; ++slot) {
        const Unsecured& entry = state->unsecured[slot];
        if (entry.used && entry.initiator &&
            std::memcmp(entry.node, header.destination, kNodeIdSize) == 0)
            return encode_context(slot + kSessions, entry.generation);
    }
    return kNoContext;
}

[[nodiscard]] Session* session_by_local_id(const std::uint16_t id, int& context) {
    for (std::size_t slot = 0; slot < kSessions; ++slot) {
        Session& session = state->sessions[slot];
        if (session.used && session.keys.local_id == id) {
            context = encode_context(slot, session.generation);
            return &session;
        }
    }
    return nullptr;
}

/** @brief Authenticates and delivers one Matter datagram. */
[[nodiscard]] bool decode(platform::UdpDatagram& datagram, const Transport transport,
    const int connection, const std::uint32_t now, Inbound& message) {
    FrameHeader frame;
    if (!parse_frame(datagram.data, datagram.size, frame))
        return false;
    std::uint8_t* body = datagram.data + frame.size;
    std::size_t body_size = datagram.size - frame.size;
    int context = kNoContext;
    Peer* peer = nullptr;
    bool fresh = false;
    if (frame.session_id == 0U) {
        // Handshakes need the initiator's ephemeral node id, which identifies the peer slot. A
        // responder to this device addresses that id instead.
        if (frame.has_source)
            context = unsecured_context(frame, datagram.peer, transport, connection, now);
        else if (frame.has_destination)
            context = initiator_context(frame);
        else
            return false;
        peer = find_peer(context);
        if (peer == nullptr)
            return false;
        fresh = replay_is_new(peer->replay, frame.counter, false);
        if (fresh)
            replay_record(peer->replay, frame.counter);
    } else {
        Session* const session = session_by_local_id(frame.session_id, context);
        if (session == nullptr && reached(now, state->next_unknown_log)) {
            // A peer still using a session from before a restart, at most once per interval.
            state->next_unknown_log = now + kUnknownSessionLogMilliseconds;
            AddLog(LogLevel::info, "MTR: message for an unknown session");
        }
        if (session == nullptr || body_size < kMicSize)
            return false;
        peer = &session->peer;
        body_size -= kMicSize;
        std::uint8_t nonce[13];
        make_nonce(nonce, frame.security_flags, frame.counter, session->keys.peer_node);
        if (!crypt(session->keys.decrypt_key, nonce, datagram.data, frame.size, body, body_size,
                body + body_size, false))
            return false;
        fresh = replay_is_new(peer->replay, frame.counter, true);
        if (fresh)
            replay_record(peer->replay, frame.counter);
        peer->address = datagram.peer;
        peer->transport = transport;
        peer->connection = connection;
        peer->last_used = now;
    }

    ProtocolHeader protocol;
    if (!parse_protocol(body, body_size, protocol))
        return false;
    if (frame.session_id == 0U && protocol.protocol != kProtocolSecureChannel)
        return false;
    if ((protocol.flags & kExchangeAck) != 0U) {
        for (ReliableSend& entry : state->reliable) {
            if (entry.frame != nullptr && entry.context == context &&
                entry.counter == protocol.ack_counter)
                release_reliable(entry);
        }
    }
    const bool peer_initiator = (protocol.flags & kExchangeInitiator) != 0U;
    if ((protocol.flags & kExchangeReliable) != 0U) {
        if (fresh)
            queue_ack(context, protocol.exchange, !peer_initiator, frame.counter, now);
        else
            send_ack(context, protocol.exchange, !peer_initiator, frame.counter);
    }
    if (!fresh ||
        (protocol.protocol == kProtocolSecureChannel && protocol.opcode == kStandaloneAck))
        return false;
    message.context = context;
    message.exchange = protocol.exchange;
    message.peer_initiator = peer_initiator;
    message.protocol = protocol.protocol;
    message.opcode = protocol.opcode;
    message.payload = body + protocol.size;
    message.size = body_size - protocol.size;
    return true;
}

/** @brief Answers one query with a multicast, or unicast to a legacy resolver. */
void answer_query(const platform::UdpDatagram& datagram) {
    constexpr std::uint16_t kMdnsPort = 5353;
    std::uint8_t* const output = state->transmit;
    const std::size_t size =
        dns_sd_respond(datagram.data, datagram.size, state->dns, output, kMaxFrameSize);
    if (size == 0U)
        return;
    if (datagram.peer.port == kMdnsPort) {
        (void)platform::matter_mdns_send(nullptr, output, size);
    } else {
        output[0] = datagram.data[0];
        output[1] = datagram.data[1];
        (void)platform::matter_mdns_send(&datagram.peer, output, size);
    }
}

/** @brief Ends what travelled on a TCP connection that closed; its sessions die with it. */
void drop_closed_connections() {
#if USE_MATTER_TCP
    for (std::size_t slot = 0; slot < kSessions; ++slot) {
        Session& session = state->sessions[slot];
        if (session.used && session.peer.transport == Transport::tcp &&
            !platform::matter_tcp_alive(session.peer.connection))
            stack_session_remove(encode_context(slot, session.generation));
    }
    for (std::size_t slot = 0; slot < kUnsecured; ++slot) {
        Unsecured& entry = state->unsecured[slot];
        if (entry.used && entry.peer.transport == Transport::tcp &&
            !platform::matter_tcp_alive(entry.peer.connection)) {
            forget_context(encode_context(slot + kSessions, entry.generation));
            entry.used = false;
        }
    }
#endif
}

void poll_network(const std::uint32_t now) {
    if (reached(now, state->next_network_poll)) {
        state->next_network_poll = now + kNetworkPollMilliseconds;
        platform::matter_udp_poll();
#if USE_MATTER_TCP
        if (platform::matter_udp_ready())
            platform::matter_tcp_poll();
#endif
        drop_closed_connections();
    }
    std::array<std::array<std::uint8_t, 16>, platform::kMatterAddressCount> addresses{};
    std::uint32_t generation = 0;
    const std::size_t count = platform::matter_ipv6_addresses(addresses, generation);
    if (generation != state->address_generation) {
        state->address_generation = generation;
        state->dns.address_count = count;
        state->dns.addresses = addresses;
        state->announces_left = 2;
        state->announce_now = true;
    }
    if (state->announces_left != 0U &&
        (state->announce_now || reached(now, state->next_announce)) &&
        platform::matter_udp_ready()) {
        state->announce_now = false;
        const std::size_t size = dns_sd_announce(state->dns, state->transmit, kMaxFrameSize);
        if (size != 0U)
            (void)platform::matter_mdns_send(nullptr, state->transmit, size);
        --state->announces_left;
        state->next_announce = now + kAnnounceSecondMilliseconds;
    }
}
} // namespace

bool stack_start() {
    if (state != nullptr)
        return true;
    state = new (std::nothrow) State{};
    if (state == nullptr)
        return false;
    state->next_session_id = static_cast<std::uint16_t>(random_counter());
    return true;
}

void stack_poll(const std::uint32_t now) {
    if (state == nullptr)
        return;
    poll_network(now);
#if USE_MATTER_BLE
    ble_link_poll(now);
#endif
    for (PendingAck& ack : state->acks) {
        if (ack.used && reached(now, ack.deadline)) {
            send_ack(ack.context, ack.exchange, ack.initiator, ack.counter);
            ack = {};
        }
    }
    for (ReliableSend& entry : state->reliable) {
        if (entry.frame == nullptr || !reached(now, entry.deadline))
            continue;
        Peer* const peer = find_peer(entry.context);
        if (peer == nullptr || entry.sends >= kMaxRetransmissions) {
            if (peer != nullptr && state->failure_count < kFailures)
                state->failures[state->failure_count++] = {entry.context, entry.exchange};
            release_reliable(entry);
            continue;
        }
        transmit(*peer, entry.frame, entry.size);
        // Backoff 1.6^(sendCount - MRP_BACKOFF_THRESHOLD 1): the first retransmission keeps it.
        if (++entry.sends > 1U)
            entry.interval += entry.interval / 2U + entry.interval / 10U;
        entry.deadline = now + jittered(entry.interval);
    }
}

bool stack_next(const std::uint32_t now, Inbound& message) {
    if (state == nullptr)
        return false;
    if (state->current != nullptr) {
        platform::matter_udp_release(state->current);
        state->current = nullptr;
    }
    if (state->failure_count != 0U) {
        --state->failure_count;
        message = {};
        message.context = state->failures[state->failure_count].context;
        message.exchange = state->failures[state->failure_count].exchange;
        message.protocol = kDeliveryFailed;
        return true;
    }
#if USE_MATTER_BLE
    while (platform::UdpDatagram* const datagram = ble_link_take()) {
        if (decode(*datagram, Transport::btp, -1, now, message)) {
            state->current = datagram;
            return true;
        }
        platform::matter_udp_release(datagram);
    }
#endif
#if USE_MATTER_TCP
    int connection = -1;
    while (platform::UdpDatagram* const datagram = platform::matter_tcp_receive(connection)) {
        if (decode(*datagram, Transport::tcp, connection, now, message)) {
            state->current = datagram;
            return true;
        }
        platform::matter_udp_release(datagram);
    }
#endif
    while (platform::UdpDatagram* const datagram = platform::matter_udp_receive()) {
        if (datagram->mdns) {
            answer_query(*datagram);
        } else if (decode(*datagram, Transport::udp, -1, now, message)) {
            state->current = datagram;
            return true;
        }
        platform::matter_udp_release(datagram);
    }
    return false;
}

bool stack_send(const int context, const std::uint16_t exchange, const bool initiator,
    const std::uint16_t protocol, const std::uint8_t opcode, const std::uint8_t* const payload,
    const std::size_t size, bool reliable, const std::uint32_t now) {
    if (state == nullptr)
        return false;
    Peer* const peer = find_peer(context);
    if (peer == nullptr)
        return false;
    // BTP delivers reliably by itself, so MRP stays off it; TCP does too and must not ask for it.
    const bool retransmit = reliable && peer->transport == Transport::udp;
    if (peer->transport == Transport::tcp)
        reliable = false;
    ReliableSend* slot = nullptr;
    if (retransmit) {
        for (ReliableSend& entry : state->reliable) {
            if (entry.frame == nullptr) {
                slot = &entry;
                break;
            }
        }
        if (slot == nullptr)
            return false;
    }
    ProtocolHeader header;
    header.flags = static_cast<std::uint8_t>(
        (initiator ? kExchangeInitiator : 0U) | (reliable ? kExchangeReliable : 0U));
    header.opcode = opcode;
    header.exchange = exchange;
    header.protocol = protocol;
    PendingAck* acknowledged = nullptr;
    for (PendingAck& ack : state->acks) {
        if (ack.used && ack.context == context && ack.exchange == exchange) {
            header.flags |= kExchangeAck;
            header.ack_counter = ack.counter;
            acknowledged = &ack;
            break;
        }
    }
    // A large message gets its own buffer; the fixed one holds a datagram.
    const std::size_t capacity = frame_limit(peer->transport);
    std::uint8_t* const large =
        capacity > kMaxFrameSize ? static_cast<std::uint8_t*>(std::malloc(capacity)) : nullptr;
    if (capacity > kMaxFrameSize && large == nullptr)
        return false;
    std::uint8_t* const output = large != nullptr ? large : state->transmit;
    std::uint32_t counter = 0;
    const std::size_t total =
        build_message(context, header, payload, size, counter, output, capacity);
    bool sent = total != 0U;
    if (sent && slot != nullptr) {
        auto* const copy = static_cast<std::uint8_t*>(std::malloc(total));
        sent = copy != nullptr;
        if (sent) {
            std::memcpy(copy, output, total);
            *slot = {copy, static_cast<std::uint16_t>(total), context, exchange, counter,
                now + jittered(peer->retransmit), peer->retransmit, 0};
        }
    }
    if (sent) {
        if (acknowledged != nullptr)
            *acknowledged = {};
        peer->last_used = now;
        transmit(*peer, output, total);
    }
    std::free(large);
    return sent;
}

std::size_t stack_max_message(const int context) {
    Peer* const peer = state != nullptr ? find_peer(context) : nullptr;
    if (peer == nullptr)
        return 0;
    return frame_limit(peer->transport) - kMessageOverhead;
}

int stack_transport(const int context) {
    Peer* const peer = state != nullptr ? find_peer(context) : nullptr;
    return peer != nullptr ? static_cast<int>(peer->transport) : -1;
}

int stack_connect(const int via_context) {
#if USE_MATTER_TCP
    Peer* const via = state != nullptr ? find_peer(via_context) : nullptr;
    if (via == nullptr || via->transport == Transport::btp)
        return kNoContext;
    // A controller's TCP listener shares the port number of its UDP socket.
    const int connection = platform::matter_tcp_connect(via->address);
    if (connection < 0)
        return kNoContext;
    // A free slot, or the one used longest ago.
    Unsecured* chosen = nullptr;
    for (Unsecured& entry : state->unsecured) {
        if (!entry.used) {
            chosen = &entry;
            break;
        }
        if (chosen == nullptr ||
            static_cast<std::int32_t>(entry.peer.last_used - chosen->peer.last_used) < 0)
            chosen = &entry;
    }
    const auto slot = static_cast<std::size_t>(chosen - state->unsecured);
    if (chosen->used)
        forget_context(encode_context(slot + kSessions, chosen->generation));
    const auto generation = static_cast<std::uint8_t>(chosen->generation + 1U);
    const platform::UdpPeer address = via->address;
    const std::uint32_t last_used = via->last_used;
    *chosen = {};
    chosen->used = true;
    chosen->generation = generation;
    chosen->initiator = true;
    (void)platform::crypto_random(chosen->node, kNodeIdSize);
    chosen->peer.address = address;
    chosen->peer.transport = Transport::tcp;
    chosen->peer.connection = connection;
    chosen->peer.last_used = last_used;
    chosen->peer.send_counter = random_counter();
    return encode_context(slot + kSessions, generation);
#else
    (void)via_context;
    return kNoContext;
#endif
}

void stack_peer_active_interval(const int context, const std::uint32_t milliseconds) {
    constexpr std::uint32_t kLowest = 50;
    constexpr std::uint32_t kHighest = 30000; // A peer's value must not stall delivery for long.
    Peer* const peer = state != nullptr ? find_peer(context) : nullptr;
    if (peer != nullptr) {
        const std::uint32_t bounded =
            milliseconds < kLowest ? kLowest : (milliseconds > kHighest ? kHighest : milliseconds);
        peer->retransmit = bounded + bounded / 10U; // The specification's margin.
    }
}

std::uint16_t stack_new_session_id() {
    if (state == nullptr)
        return 0;
    while (true) {
        const std::uint16_t candidate = ++state->next_session_id;
        int unused = 0;
        if (candidate != 0U && session_by_local_id(candidate, unused) == nullptr)
            return candidate;
    }
}

int stack_session_new(const int from_context, const SessionParameters& parameters) {
    Peer* const origin = state != nullptr ? find_peer(from_context) : nullptr;
    if (origin == nullptr)
        return kNoContext;
    Session* chosen = nullptr;
    unsigned same_fabric = 0;
    Session* oldest_in_fabric = nullptr;
    Session* oldest = nullptr;
    const auto older = [](const Session* const left, const Session& right) {
        return left == nullptr ||
               static_cast<std::int32_t>(right.peer.last_used - left->peer.last_used) < 0;
    };
    for (Session& session : state->sessions) {
        if (!session.used) {
            if (chosen == nullptr)
                chosen = &session;
            continue;
        }
        if (older(oldest, session))
            oldest = &session;
        if (parameters.fabric != 0U && session.keys.fabric == parameters.fabric) {
            ++same_fabric;
            if (older(oldest_in_fabric, session))
                oldest_in_fabric = &session;
        }
    }
    if (same_fabric >= kSessionsPerFabric)
        chosen = oldest_in_fabric;
    else if (chosen == nullptr)
        chosen = oldest;
    const platform::UdpPeer address = origin->address;
    const Transport transport = origin->transport;
    const int connection = origin->connection;
    const std::uint32_t last_used = origin->last_used;
    const std::uint32_t retransmit = origin->retransmit;
    const auto slot = static_cast<std::size_t>(chosen - state->sessions);
    if (chosen->used)
        forget_context(encode_context(slot, chosen->generation));
    const auto generation = static_cast<std::uint8_t>(chosen->generation + 1U);
    *chosen = {};
    chosen->used = true;
    chosen->generation = generation;
    chosen->keys = parameters;
    chosen->peer.address = address;
    chosen->peer.transport = transport;
    chosen->peer.connection = connection;
    chosen->peer.last_used = last_used;
    chosen->peer.retransmit = retransmit;
    chosen->peer.send_counter = random_counter();
    chosen->reserved = chosen->peer.send_counter + kCounterReserve;
    return encode_context(slot, generation);
}

std::size_t stack_session_export(const int context, std::uint8_t* const output) {
    Session* const session = state != nullptr ? find_session(context) : nullptr;
    if (session == nullptr || session->peer.transport != Transport::udp)
        return 0;
    session->reserved = session->peer.send_counter + kCounterReserve;
    std::uint8_t* cursor = output;
    const auto put = [&cursor](const void* const data, const std::size_t size) {
        std::memcpy(cursor, data, size);
        cursor += size;
    };
    put(&session->keys.local_id, 2);
    put(&session->keys.peer_id, 2);
    put(session->keys.decrypt_key, 16);
    put(session->keys.encrypt_key, 16);
    put(session->keys.peer_node, kNodeSize);
    put(session->keys.local_node, kNodeSize);
    put(&session->keys.fabric, 1);
    put(session->peer.address.address.data(), 16);
    put(&session->peer.address.port, 2);
    put(&session->peer.address.zone, 1);
    put(&session->reserved, 4);
    put(&session->peer.replay.highest, 4);
    put(&session->peer.retransmit, 4);
    static_assert(2U + 2U + 16U + 16U + kNodeSize + kNodeSize + 1U + 16U + 2U + 1U + 4U + 4U + 4U ==
                      kSessionRecordSize,
        "the record size matches its fields");
    return kSessionRecordSize;
}

int stack_session_import(
    const std::uint8_t* const record, const std::size_t size, const std::uint32_t now) {
    if (state == nullptr || size != kSessionRecordSize)
        return kNoContext;
    std::size_t slot = 0;
    while (slot < kSessions && state->sessions[slot].used)
        ++slot;
    if (slot == kSessions)
        return kNoContext;
    Session& session = state->sessions[slot];
    const auto generation = static_cast<std::uint8_t>(session.generation + 1U);
    session = {};
    session.used = true;
    session.generation = generation;
    const std::uint8_t* cursor = record;
    const auto get = [&cursor](void* const data, const std::size_t count) {
        std::memcpy(data, cursor, count);
        cursor += count;
    };
    get(&session.keys.local_id, 2);
    get(&session.keys.peer_id, 2);
    get(session.keys.decrypt_key, 16);
    get(session.keys.encrypt_key, 16);
    get(session.keys.peer_node, kNodeSize);
    get(session.keys.local_node, kNodeSize);
    get(&session.keys.fabric, 1);
    get(session.peer.address.address.data(), 16);
    get(&session.peer.address.port, 2);
    get(&session.peer.address.zone, 1);
    get(&session.peer.send_counter, 4);
    get(&session.peer.replay.highest, 4);
    get(&session.peer.retransmit, 4);
    session.peer.replay.started = true;
    session.peer.last_used = now;
    session.reserved = session.peer.send_counter + kCounterReserve;
    return encode_context(slot, generation);
}

std::uint32_t stack_take_dirty_fabrics() {
    if (state == nullptr)
        return 0;
    const std::uint32_t dirty = state->dirty_fabrics;
    state->dirty_fabrics = 0;
    return dirty;
}

const DnsRecords& stack_addresses() {
    return state->dns;
}

bool stack_network_ready() {
    return state != nullptr && platform::matter_udp_ready() && state->dns.address_count != 0U;
}

void stack_session_remove(const int context) {
    if (state == nullptr)
        return;
    if (Session* const session = find_session(context)) {
        forget_context(context);
        session->keys = {};
        session->used = false;
    }
}

void stack_fabric_remove(const std::uint8_t fabric) {
    if (state == nullptr)
        return;
    for (std::size_t slot = 0; slot < kSessions; ++slot) {
        Session& session = state->sessions[slot];
        if (session.used && session.keys.fabric == fabric)
            stack_session_remove(encode_context(slot, session.generation));
    }
}

DnsRecords& stack_dns_records() {
    return state->dns;
}

void stack_dns_changed() {
    if (state != nullptr) {
        state->announces_left = 2;
        state->announce_now = true;
    }
}

} // namespace newmota::matter
