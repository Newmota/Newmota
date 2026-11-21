/*
  tcp_stream.cpp - common raw-lwIP TCP stream adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/tcp_stream.h"
#include "newmota_features.h"
#if USE_I2S && USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
#include "newmota/platform/audio_network.h"
#endif

#include "newmota/platform/platform.h"

// Vendor lwIP headers do not consistently provide their own C++ linkage
// guards. Keep all native declarations in one common adapter.
extern "C" {
#include "lwip/err.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#if !defined(PICO_ON_DEVICE)
#include "lwip/tcpip.h"
#endif
}

#if defined(PICO_ON_DEVICE)
#include "pico/cyw43_arch.h"
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

namespace newmota::platform {
namespace {

constexpr std::size_t kHostCapacity = 128U;
constexpr std::size_t kReceiveCapacity = 4096U;
constexpr std::size_t kTransmitCapacity = 2048U;
constexpr std::size_t kApplicationWriteChunk = 1024U;
constexpr std::size_t kCoreOperationLimit = 4U;
constexpr std::uint32_t kConnectionTimeoutMilliseconds = 15'000U;
constexpr std::uint8_t kPollInterval = 2U; // 1 second in lwIP's 500 ms units.

/** @brief Owns one serialized, plaintext IPv4 connection attempt. */
enum class Phase : std::uint8_t {
    idle,
    starting,
    resolving,
    connecting,
    connected,
    closing,
    failed,
};

/** @brief Stable context retained until the corresponding DNS callback drains. */
struct DnsContext final {
    std::uint32_t generation{};
};

/**
 * @brief Bridges the application loop and the lwIP core with SPSC rings.
 *
 * The lwIP core is the producer of rx_head and consumer of tx_tail. The
 * application loop is the consumer of rx_tail and producer of tx_head. Each
 * side only writes its own index, so the handoff needs atomic loads/stores but
 * no byte-sized atomic read-modify-write operation on constrained targets.
 */
struct Adapter final {
    std::atomic<Phase> phase{Phase::idle};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> core_work_queued{false};
    std::atomic<bool> dns_pending{false};
    std::atomic<bool> remote_closed{false};
    std::atomic<std::uint32_t> generation{0U};
    std::atomic<std::uint32_t> rx_head{0U};
    std::atomic<std::uint32_t> rx_tail{0U};
    std::atomic<std::uint32_t> tx_head{0U};
    std::atomic<std::uint32_t> tx_tail{0U};
    std::atomic<std::uint32_t> pending_ack{0U};
    std::array<char, kHostCapacity> host{};

    /** @brief Both rings, allocated by open() and freed once state() sees idle. The core
     * reaches them only while connected; its last store of a connection is phase idle. */
    struct Rings {
        std::array<std::uint8_t, kReceiveCapacity> receive{};
        std::array<std::uint8_t, kTransmitCapacity> transmit{};
    };

#if defined(NEWMOTA_IMAGE_SAFEBOOT)
    // Safeboot keeps them static: its binary size matters more than its free heap.
    Rings rings_storage{};
#else
    std::unique_ptr<Rings> rings{};
#endif
    DnsContext dns{};
    ip_addr_t address{};
    tcp_pcb* pcb{};
    std::uint16_t port{};
    std::uint32_t deadline{};
    std::uint32_t rx_credit{};
    bool have_address{};
};

/** @brief One callback/ring context per native connection owner. */
template <unsigned Slot> struct Implementation {
    static inline Adapter adapter{};

    /** @brief The rings: static in Safeboot, otherwise the block open() allocated. */
    [[nodiscard]] static Adapter::Rings& rings() {
#if defined(NEWMOTA_IMAGE_SAFEBOOT)
        return adapter.rings_storage;
#else
        return *adapter.rings;
#endif
    }

    /** @brief Avoids vendor C-library collisions in older C++ SDK headers. */
    template <typename T> [[nodiscard]] static constexpr T minimum(const T left, const T right) {
        return left < right ? left : right;
    }

    /** @brief Tests a wrapping millisecond deadline. */
    [[nodiscard]] static bool deadline_reached(
        const std::uint32_t now, const std::uint32_t deadline) {
        return static_cast<std::int32_t>(now - deadline) >= 0;
    }

    /** @brief Returns the number of bytes currently held by one SPSC ring. */
    [[nodiscard]] static std::size_t ring_size(const std::uint32_t head, const std::uint32_t tail) {
        return static_cast<std::size_t>(head - tail);
    }

    /** @brief Copies a pbuf chain into the receive ring without retaining it. */
    static void copy_received(const pbuf* packet, const std::uint32_t head) {
        const auto offset = static_cast<std::size_t>(head % kReceiveCapacity);
        const auto total = static_cast<std::size_t>(packet->tot_len);
        const auto first = minimum(total, kReceiveCapacity - offset);
        (void)pbuf_copy_partial(
            packet, rings().receive.data() + offset, static_cast<u16_t>(first), 0U);
        if (first < total) {
            (void)pbuf_copy_partial(packet, rings().receive.data(),
                static_cast<u16_t>(total - first), static_cast<u16_t>(first));
        }
    }

    /** @brief Copies one bounded application fragment from the receive ring. */
    static void copy_to_application(
        std::uint8_t* output, const std::uint32_t tail, const std::size_t count) {
        const auto offset = static_cast<std::size_t>(tail % kReceiveCapacity);
        const auto first = minimum(count, kReceiveCapacity - offset);
        std::memcpy(output, rings().receive.data() + offset, first);
        if (first < count) {
            std::memcpy(output + first, rings().receive.data(), count - first);
        }
    }

    /** @brief Copies one bounded application fragment into the transmit ring. */
    static void copy_from_application(
        const std::uint8_t* input, const std::uint32_t head, const std::size_t count) {
        const auto offset = static_cast<std::size_t>(head % kTransmitCapacity);
        const auto first = minimum(count, kTransmitCapacity - offset);
        std::memcpy(rings().transmit.data() + offset, input, first);
        if (first < count) {
            std::memcpy(rings().transmit.data(), input + first, count - first);
        }
    }

    /** @brief Detaches callbacks and aborts one raw-lwIP PCB on the lwIP core. */
    static void abort_pcb() {
        auto* pcb = adapter.pcb;
        if (pcb == nullptr)
            return;
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_sent(pcb, nullptr);
        tcp_poll(pcb, nullptr, 0U);
        tcp_err(pcb, nullptr);
        adapter.pcb = nullptr;
        tcp_abort(pcb);
    }

    /** @brief Clears the application rings after native objects are gone. */
    static void reset_buffers() {
        adapter.rx_head.store(0U);
        adapter.rx_tail.store(0U);
        adapter.tx_head.store(0U);
        adapter.tx_tail.store(0U);
        adapter.pending_ack.store(0U);
        adapter.rx_credit = 0U;
        adapter.remote_closed.store(false);
    }

    /** @brief Completes cancellation once no DNS callback can reference the slot. */
    static void finish_close_on_core() {
        abort_pcb();
        if (adapter.dns_pending.load()) {
            adapter.phase.store(Phase::closing);
            return;
        }
        reset_buffers();
        adapter.have_address = false;
        adapter.phase.store(Phase::idle);
    }

    /** @brief Fails one attempt while preserving DNS-slot lifetime rules. */
    static void fail_on_core() {
        abort_pcb();
        adapter.remote_closed.store(true);
        if (adapter.dns_pending.load()) {
            adapter.cancelled.store(true);
            adapter.phase.store(Phase::closing);
            return;
        }
        adapter.phase.store(Phase::failed);
    }

    /** @brief Applies bytes consumed by the application to the TCP receive window. */
    static void acknowledge_received_on_core() {
        if (adapter.pcb == nullptr)
            return;
        const auto consumed = adapter.rx_tail.load();
        auto pending = consumed - adapter.rx_credit;
        while (pending != 0U) {
            const auto amount = minimum<std::uint32_t>(
                pending, static_cast<std::uint32_t>(std::numeric_limits<u16_t>::max()));
            tcp_recved(adapter.pcb, static_cast<u16_t>(amount));
            adapter.rx_credit += amount;
            pending -= amount;
        }
    }

    /** @brief Flushes bounded transmit-ring fragments through raw TCP. */
    static void flush_transmit_on_core() {
        if (adapter.pcb == nullptr || adapter.phase.load() != Phase::connected) {
            return;
        }
        auto tail = adapter.tx_tail.load();
        const auto head = adapter.tx_head.load();
        std::size_t operations = 0U;
        while (tail != head && operations < kCoreOperationLimit) {
            const auto pending = ring_size(head, tail);
            const auto available = static_cast<std::size_t>(tcp_sndbuf(adapter.pcb));
            if (available == 0U)
                break;
            const auto offset = static_cast<std::size_t>(tail % kTransmitCapacity);
            const auto amount = minimum(minimum(pending, available),
                minimum(kTransmitCapacity - offset, kApplicationWriteChunk));
            const auto result = tcp_write(adapter.pcb, rings().transmit.data() + offset,
                static_cast<u16_t>(amount), TCP_WRITE_FLAG_COPY);
            if (result == ERR_MEM)
                break;
            if (result != ERR_OK) {
                fail_on_core();
                return;
            }
            adapter.pending_ack.store(
                adapter.pending_ack.load() + static_cast<std::uint32_t>(amount));
            tail += static_cast<std::uint32_t>(amount);
            adapter.tx_tail.store(tail);
            ++operations;
        }
        (void)tcp_output(adapter.pcb);
    }

    /** @brief Checks cancellation/deadline and services queued TCP work. */
    static void maintain_on_core() {
        const auto phase = adapter.phase.load();
        if (adapter.cancelled.load() || phase == Phase::closing) {
            finish_close_on_core();
            return;
        }
        if ((phase == Phase::starting || phase == Phase::resolving || phase == Phase::connecting) &&
            deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
            adapter.cancelled.store(true);
            adapter.phase.store(Phase::closing);
            finish_close_on_core();
            return;
        }
        acknowledge_received_on_core();
        flush_transmit_on_core();
    }

    /** @brief Starts a raw TCP connection after DNS has produced an IPv4 address. */

    /** @brief Receives one DNS result and releases the retained callback slot. */
    static void dns_found(const char*, const ip_addr_t* address, void* context) {
        const auto* dns = static_cast<const DnsContext*>(context);
        adapter.dns_pending.store(false);
        if (dns == nullptr || dns->generation != adapter.generation.load() ||
            adapter.cancelled.load() || adapter.phase.load() == Phase::closing) {
            finish_close_on_core();
            return;
        }
        if (address == nullptr || !IP_IS_V4(address) ||
            deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
            fail_on_core();
            return;
        }
        adapter.address = *address;
        adapter.have_address = true;
        start_connection_on_core();
    }

    /** @brief Starts one asynchronous DNS lookup on the lwIP core. */
    static void resolve_on_core() {
        if (adapter.cancelled.load() || adapter.phase.load() != Phase::starting ||
            deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
            adapter.cancelled.store(true);
            adapter.phase.store(Phase::closing);
            finish_close_on_core();
            return;
        }
        adapter.phase.store(Phase::resolving);
        adapter.dns.generation = adapter.generation.load();
        adapter.dns_pending.store(true);
        ip_addr_t resolved{};
        const auto result =
            dns_gethostbyname(adapter.host.data(), &resolved, dns_found, &adapter.dns);
        if (result == ERR_OK) {
            dns_found(adapter.host.data(), &resolved, &adapter.dns);
        } else if (result != ERR_INPROGRESS) {
            adapter.dns_pending.store(false);
            fail_on_core();
        }
    }

    /** @brief Completes one raw TCP connection callback. */
    static err_t connected_on_core(void*, tcp_pcb* pcb, const err_t error) {
        if (pcb != adapter.pcb)
            return ERR_ABRT;
        if (error != ERR_OK || adapter.cancelled.load() ||
            deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
            fail_on_core();
            return ERR_ABRT;
        }
        adapter.phase.store(Phase::connected);
        acknowledge_received_on_core();
        flush_transmit_on_core();
        return ERR_OK;
    }

    /** @brief Handles inbound TCP data with bounded pbuf-chain backpressure. */
    static err_t received_on_core(void*, tcp_pcb* pcb, pbuf* packet, const err_t error) {
        if (pcb != adapter.pcb) {
            if (packet != nullptr)
                pbuf_free(packet);
            return ERR_ABRT;
        }
        if (packet == nullptr) {
            adapter.remote_closed.store(true);
            return ERR_OK;
        }
        if (error != ERR_OK) {
            pbuf_free(packet);
            fail_on_core();
            return ERR_ABRT;
        }
        if (adapter.phase.load() != Phase::connected) {
            tcp_recved(pcb, packet->tot_len);
            pbuf_free(packet);
            return ERR_OK;
        }
        const auto head = adapter.rx_head.load();
        const auto tail = adapter.rx_tail.load();
        const auto total = static_cast<std::size_t>(packet->tot_len);
        const auto used = ring_size(head, tail);
        if (total > kReceiveCapacity) {
            // A single pbuf chain larger than the bounded ring cannot make
            // progress. Fail the attempt rather than retaining it forever.
            pbuf_free(packet);
            fail_on_core();
            return ERR_ABRT;
        }
        if (total > kReceiveCapacity - used) {
            // Keep the pbuf owned by lwIP. Application reads will return window
            // credit and cause this callback to be retried later.
            return ERR_MEM;
        }
        copy_received(packet, head);
        adapter.rx_head.store(head + static_cast<std::uint32_t>(total));
        pbuf_free(packet);
        return ERR_OK;
    }

    /** @brief Flushes queued application bytes after TCP acknowledges output. */
    static err_t sent_on_core(void*, tcp_pcb* pcb, u16_t acknowledged) {
        if (pcb != adapter.pcb)
            return ERR_ABRT;
        const auto pending = adapter.pending_ack.load();
        adapter.pending_ack.store(pending > acknowledged ? pending - acknowledged : 0U);
        flush_transmit_on_core();
        return adapter.pcb == nullptr ? ERR_ABRT : ERR_OK;
    }

    /** @brief Marks a native error after lwIP has released its PCB. */
    static void error_on_core(void*, err_t) {
        adapter.pcb = nullptr;
        adapter.remote_closed.store(true);
        adapter.phase.store(Phase::failed);
    }

    /** @brief Enforces the connection deadline without blocking the core. */
    static err_t poll_on_core(void*, tcp_pcb* pcb) {
        if (pcb != adapter.pcb)
            return ERR_ABRT;
        maintain_on_core();
        return adapter.pcb == nullptr ? ERR_ABRT : ERR_OK;
    }

    /** @brief Creates and starts one raw TCP PCB. */
    static void start_connection_on_core() {
        if (adapter.cancelled.load() || !adapter.have_address ||
            deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
            adapter.cancelled.store(true);
            adapter.phase.store(Phase::closing);
            finish_close_on_core();
            return;
        }
        adapter.pcb = tcp_new();
        if (adapter.pcb == nullptr) {
            fail_on_core();
            return;
        }
        tcp_arg(adapter.pcb, nullptr);
        tcp_recv(adapter.pcb, received_on_core);
        tcp_sent(adapter.pcb, sent_on_core);
        tcp_poll(adapter.pcb, poll_on_core, kPollInterval);
        tcp_err(adapter.pcb, error_on_core);
        adapter.phase.store(Phase::connecting);
        const auto result =
            tcp_connect(adapter.pcb, &adapter.address, adapter.port, connected_on_core);
        if (result != ERR_OK)
            fail_on_core();
    }

    /** @brief Advances all native work submitted by the application context. */
    static void advance_on_core(void*) {
        const auto phase = adapter.phase.load();
        if (adapter.cancelled.load() || phase == Phase::closing) {
            finish_close_on_core();
        } else if (phase == Phase::starting) {
            if (adapter.have_address)
                start_connection_on_core();
            else
                resolve_on_core();
        } else {
            maintain_on_core();
        }
        adapter.core_work_queued.store(false);
    }

    /** @brief Schedules one bounded callback in the platform's lwIP context. */
    static bool schedule_core() {
        if (adapter.core_work_queued.load())
            return true;
        adapter.core_work_queued.store(true);
#if defined(PICO_ON_DEVICE)
        cyw43_arch_lwip_begin();
        advance_on_core(nullptr);
        cyw43_arch_lwip_end();
        return true;
#else
        if (tcpip_callback_with_block(advance_on_core, nullptr, 0) != ERR_OK) {
            adapter.core_work_queued.store(false);
            return false;
        }
        return true;
#endif
    }

    /** @brief Reports state and submits deadline/core maintenance work. */
    static TcpStreamState state(void*) {
        auto phase = adapter.phase.load();
        if (phase == Phase::starting || phase == Phase::resolving || phase == Phase::connecting ||
            phase == Phase::closing) {
            if ((phase == Phase::starting || phase == Phase::resolving ||
                    phase == Phase::connecting) &&
                deadline_reached(monotonic_milliseconds(), adapter.deadline)) {
                adapter.cancelled.store(true);
                adapter.phase.store(Phase::closing);
                phase = Phase::closing;
            }
            (void)schedule_core();
            phase = adapter.phase.load();
        }
        switch (phase) {
        case Phase::idle:
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
            if (!keep_rings())
                adapter.rings.reset();
#endif
            return TcpStreamState::idle;
        case Phase::starting:
        case Phase::resolving:
        case Phase::connecting:
            return TcpStreamState::connecting;
        case Phase::connected:
            return TcpStreamState::connected;
        case Phase::closing:
        case Phase::failed:
            return TcpStreamState::closing;
        }
        return TcpStreamState::unavailable;
    }

    /** @brief Starts plaintext TCP or rejects secure mode without downgrade. */
    static TcpStreamOpenResult open(
        void*, const std::string_view host, const std::uint16_t port, const bool secure) {
        if (secure)
            return TcpStreamOpenResult::unavailable;
        if (adapter.phase.load() != Phase::idle || adapter.dns_pending.load() ||
            adapter.core_work_queued.load()) {
            return TcpStreamOpenResult::busy;
        }
        if (host.empty() || host.size() >= adapter.host.size() || port == 0U ||
            host.find('\0') != std::string_view::npos) {
            return TcpStreamOpenResult::invalid;
        }
#if !defined(NEWMOTA_IMAGE_SAFEBOOT)
        if (!adapter.rings)
            adapter.rings.reset(new (std::nothrow) Adapter::Rings{});
        if (!adapter.rings)
            return TcpStreamOpenResult::failed;
#endif
        adapter.host.fill('\0');
        std::memcpy(adapter.host.data(), host.data(), host.size());
        adapter.port = port;
        adapter.deadline = monotonic_milliseconds() + kConnectionTimeoutMilliseconds;
        adapter.cancelled.store(false);
        adapter.remote_closed.store(false);
        adapter.have_address = false;
        adapter.address = {};
        reset_buffers();
        adapter.generation.store(adapter.generation.load() + 1U);

        ip4_addr_t numeric{};
        if (ip4addr_aton(adapter.host.data(), &numeric) != 0) {
            ip_addr_set_ip4_u32(&adapter.address, numeric.addr);
            adapter.have_address = true;
        }
        adapter.phase.store(Phase::starting);
        if (!schedule_core()) {
            adapter.phase.store(Phase::failed);
            return TcpStreamOpenResult::failed;
        }
        return TcpStreamOpenResult::accepted;
    }

    /** @brief Reads one bounded fragment, including buffered bytes after FIN. */
    static TcpStreamIoResult read(void*, std::uint8_t* output, const std::size_t capacity) {
        if (output == nullptr || capacity == 0U) {
            return {TcpStreamIoState::unavailable, 0U};
        }
        const auto phase = adapter.phase.load();
        if (phase != Phase::connected && phase != Phase::closing && phase != Phase::failed) {
            return {TcpStreamIoState::unavailable, 0U};
        }
        const auto head = adapter.rx_head.load();
        const auto tail = adapter.rx_tail.load();
        const auto available = ring_size(head, tail);
        if (available != 0U) {
            const auto amount = minimum(available, capacity);
            copy_to_application(output, tail, amount);
            adapter.rx_tail.store(tail + static_cast<std::uint32_t>(amount));
            (void)schedule_core();
            return {TcpStreamIoState::progressed, amount};
        }
        if (adapter.remote_closed.load()) {
            return {TcpStreamIoState::closed, 0U};
        }
        return {TcpStreamIoState::would_block, 0U};
    }

    /** @brief Queues one bounded fragment without retaining caller memory. */
    static TcpStreamIoResult write(void*, const std::uint8_t* input, const std::size_t size) {
        if (input == nullptr || size == 0U) {
            return {TcpStreamIoState::unavailable, 0U};
        }
        if (adapter.phase.load() != Phase::connected) {
            return {TcpStreamIoState::unavailable, 0U};
        }
        const auto head = adapter.tx_head.load();
        const auto tail = adapter.tx_tail.load();
        const auto used = ring_size(head, tail);
        if (used >= kTransmitCapacity) {
            return {TcpStreamIoState::would_block, 0U};
        }
        const auto amount =
            minimum(size, minimum(kTransmitCapacity - used, kApplicationWriteChunk));
        copy_from_application(input, head, amount);
        adapter.tx_head.store(head + static_cast<std::uint32_t>(amount));
        (void)schedule_core();
        return {TcpStreamIoState::progressed, amount};
    }

    /** @brief Requests asynchronous, idempotent cleanup of the native attempt. */
    static void close(void*) {
        if (adapter.phase.load() == Phase::idle && !adapter.dns_pending.load()) {
            return;
        }
        adapter.cancelled.store(true);
        adapter.phase.store(Phase::closing);
        (void)schedule_core();
    }

#if !(USE_I2S && USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS))
    [[nodiscard]] static constexpr bool keep_rings() { return false; }
#endif
    /** @brief Exposes only this instance's bounded stream operations. */
    static TcpStream stream() { return {&adapter, &state, &open, &read, &write, &close}; }
#if USE_I2S && USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
    static inline tcp_pcb* listener{};
    static inline std::atomic<bool> listener_wanted{false}, listener_queued{false};

    /** @brief A listening slot keeps its rings until the core has closed the listener:
     * accepted_on_core attaches them on the core. */
    [[nodiscard]] static bool keep_rings() {
        return listener_wanted.load() || listener_state.load() != AudioListenerState::closed;
    }

    static inline std::atomic<AudioListenerState> listener_state{AudioListenerState::closed};

    /** @brief Attaches a newly accepted client to this instance's private rings. */
    static err_t accepted_on_core(void*, tcp_pcb* pcb, err_t error) {
        if (error != ERR_OK || !listener_wanted.load() || adapter.phase.load() != Phase::idle ||
            adapter.core_work_queued.load() || !adapter.rings) {
            if (pcb)
                tcp_abort(pcb);
            return ERR_ABRT;
        }
        reset_buffers();
        adapter.cancelled.store(false);
        adapter.generation.store(adapter.generation.load() + 1U);
        adapter.pcb = pcb;
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, received_on_core);
        tcp_sent(pcb, sent_on_core);
        tcp_poll(pcb, poll_on_core, kPollInterval);
        tcp_err(pcb, error_on_core);
        adapter.phase.store(Phase::connected);
        tcp_accepted(listener);
        return ERR_OK;
    }

    /** @brief Creates/releases the listening PCB exclusively on the native network core. */
    static void change_listener_on_core(void*) {
        if (!listener_wanted.load()) {
            if (listener) {
                tcp_arg(listener, nullptr);
                tcp_accept(listener, nullptr);
                (void)tcp_close(listener);
                listener = nullptr;
            }
            adapter.cancelled.store(true);
            finish_close_on_core();
            listener_state.store(AudioListenerState::closed);
        } else if (!listener) {
            auto* pcb = tcp_new();
            if (!pcb || tcp_bind(pcb, IP_ADDR_ANY, 81) != ERR_OK) {
                if (pcb)
                    tcp_abort(pcb);
                listener_state.store(AudioListenerState::failed);
            } else {
                auto* listening = tcp_listen_with_backlog(pcb, 1);
                if (!listening) {
                    tcp_abort(pcb);
                    listener_state.store(AudioListenerState::failed);
                } else {
                    listener = listening;
                    tcp_arg(listener, nullptr);
                    tcp_accept(listener, accepted_on_core);
                    listener_state.store(AudioListenerState::listening);
                }
            }
        }
        listener_queued.store(false);
    }

    /** @brief Schedules one listener state transition without blocking either task. */
    static bool schedule_listener() {
        if (listener_queued.load())
            return true;
        listener_queued.store(true);
#if defined(PICO_ON_DEVICE)
        cyw43_arch_lwip_begin();
        change_listener_on_core(nullptr);
        cyw43_arch_lwip_end();
        return true;
#else
        if (tcpip_callback_with_block(change_listener_on_core, nullptr, 0) != ERR_OK) {
            listener_queued.store(false);
            return false;
        }
        return true;
#endif
    }
#endif
};

} // namespace

/** @brief Returns the common raw-lwIP plaintext TCP stream. */
TcpStream tcp_stream() {
    return Implementation<0>::stream();
}

#if USE_I2S && USE_WIFI && (USE_AUDIO_MP3 || USE_AUDIO_AAC || USE_AUDIO_OPUS)
TcpStream audio_tcp_stream() {
    return Implementation<1>::stream();
}

TcpStream audio_listener_stream() {
    auto stream = Implementation<2>::stream();
    stream.open = nullptr;
    return stream;
}

bool audio_listener_idle() {
    auto& adapter = Implementation<2>::adapter;
    return adapter.tx_head.load() == adapter.tx_tail.load() && adapter.pending_ack.load() == 0U;
}

bool audio_listen(bool enabled) {
    using Server = Implementation<2>;
    // Allocate on the application side before the core may accept into the rings.
    if (enabled && !Server::adapter.rings)
        Server::adapter.rings.reset(new (std::nothrow) Adapter::Rings{});
    if (enabled && !Server::adapter.rings)
        return false;
    Server::listener_wanted.store(enabled);
    if (enabled && Server::listener_state.load() != AudioListenerState::listening)
        Server::listener_state.store(AudioListenerState::starting);
    return Server::schedule_listener();
}

AudioListenerState audio_listener_state() {
    using Server = Implementation<2>;
    if (Server::listener_state.load() == AudioListenerState::starting)
        (void)Server::schedule_listener();
    return Server::listener_state.load();
}
#endif

} // namespace newmota::platform
