/*
  web.cpp - Raspberry Pi Pico SDK raw lwIP HTTP adapter for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "newmota/platform/web.h"
#include "newmota/platform/web_http.h"

#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/time.h"

extern "C" {
#if USE_WEB_HTTPS
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/pbuf.h"
#else
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#endif
}

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace newmota::platform {
namespace {

#if USE_WEB_HTTPS
using TcpPcb = struct altcp_pcb;
#define tcp_arg altcp_arg
#define tcp_recv altcp_recv
#define tcp_sent altcp_sent
#define tcp_poll altcp_poll
#define tcp_err altcp_err
#define tcp_abort altcp_abort
#define tcp_close altcp_close
#define tcp_sndbuf altcp_sndbuf
#define tcp_write altcp_write
#define tcp_output altcp_output
#define tcp_recved altcp_recved
#define tcp_setprio altcp_setprio
#define tcp_bind altcp_bind
#define tcp_listen_with_backlog altcp_listen_with_backlog
#define tcp_accept altcp_accept
#else
using TcpPcb = struct tcp_pcb;
#endif
using Pbuf = struct pbuf;

// A Pico raw-lwIP connection retains the bounded header before the application
// can own the exchange, so receive storage is exactly header plus configured body.
constexpr std::size_t kHeaderLimit = 2048;
constexpr std::size_t kReceiveCapacity = kHeaderLimit + WebRequest{}.body.size();
constexpr std::size_t kTransmitChunk = 1024;
constexpr std::uint16_t kHttpPort = 80;
constexpr std::uint16_t kHttpsPort = 443;
constexpr std::uint32_t kConnectionTimeoutMilliseconds = 6000;
constexpr std::uint8_t kPollInterval = 4; // 2 seconds in lwIP's 500 ms units.

enum class Phase : std::uint8_t {
    idle,
    waiting,
    receiving,
    pending,
    processing,
    sending,
};

std::atomic<Phase> phase{Phase::idle};
std::atomic<bool> cancelled{false};
std::atomic<bool> aborted{false};
std::atomic<TcpPcb*> client{nullptr};
TcpPcb* listener{};
#if USE_WEB_HTTPS
struct altcp_tls_config* tls_config{};
#endif
WebExchange exchange;
/** @brief Static: the asynchronous send reads it after reject() returns. */
std::array<char, kWebRejectionCapacity> rejected_body{};
std::array<char, kReceiveCapacity> receive_buffer{};
std::size_t receive_size{};
std::uint32_t connection_deadline{};
WebResponseView in_flight{};
const WebResponseView* response_in_flight{};
std::array<char, 768> response_header{};
std::size_t response_header_size{};
std::size_t response_header_offset{};
std::size_t response_body_offset{};
bool close_pending{};
bool request_close{true};
std::size_t response_acknowledged{};

/** @brief Tests a wrapping millisecond deadline. */
[[nodiscard]] bool deadline_reached(const std::uint32_t now, const std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}

/** @brief Returns the Pico SDK monotonic clock without entering lwIP. */
[[nodiscard]] std::uint32_t now_milliseconds() {
    return static_cast<std::uint32_t>(to_ms_since_boot(get_absolute_time()));
}

using web_http::ParseResult;

std::size_t header_end() {
    return web_http::header_end(receive_buffer.data(), receive_size);
}

ParseResult parse_request(const std::size_t headers_end, unsigned int& rejection_status) {
    const auto result = web_http::parse_request(receive_buffer.data(), receive_size,
        exchange.request, request_close, headers_end, rejection_status);
    if (result == ParseResult::ready)
        exchange.request.secure = USE_WEB_HTTPS != 0;
    return result;
}

/** @brief Detaches callbacks and abandons one raw-lwIP connection. */
void abort_connection(TcpPcb* pcb, const bool preserve_application = false) {
    if (pcb == nullptr)
        return;
    const auto current = phase.load();
    aborted.store(true);
    if (client.load() == pcb)
        client.store(nullptr);
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    tcp_err(pcb, nullptr);
    tcp_abort(pcb);
    receive_size = 0;
    response_in_flight = nullptr;
    close_pending = false;
    if (preserve_application && (current == Phase::pending || current == Phase::processing)) {
        cancelled.store(true);
        return;
    }
    phase.store(Phase::idle);
    exchange.request.body.fill(0);
    exchange.response.body.fill(0);
}

/** @brief Releases state after a successful graceful TCP close. */
void closed_connection() {
    client.store(nullptr);
    receive_size = 0;
    response_in_flight = nullptr;
    close_pending = false;
    aborted.store(false);
    phase.store(Phase::idle);
    exchange.request.body.fill(0);
    exchange.response.body.fill(0);
}

void process_received(TcpPcb* pcb);
err_t sent(void*, TcpPcb*, u16_t);
void error_callback(void*, err_t);
err_t poll(void*, TcpPcb*);

/** @brief Closes a response connection after all queued bytes are acknowledged. */
void close_after_send(TcpPcb* pcb) {
    if (pcb == nullptr || client.load() != pcb)
        return;

    // Detach callbacks before close: the TLS altcp implementation frees its
    // wrapper PCB on success. If lwIP cannot close yet, restore the callbacks
    // needed to retry after an acknowledgement or timeout.
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    tcp_err(pcb, nullptr);
    const auto result = tcp_close(pcb);
    if (result == ERR_MEM) {
        phase.store(Phase::sending);
        close_pending = true;
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_sent(pcb, sent);
        tcp_poll(pcb, poll, kPollInterval);
        tcp_err(pcb, error_callback);
        return;
    }
    if (result != ERR_OK) {
        abort_connection(pcb);
        return;
    }
    closed_connection();
}

/** @brief Closes a peer half-close, aborting if graceful close cannot queue. */
[[nodiscard]] err_t close_peer(TcpPcb* pcb) {
    if (pcb == nullptr)
        return ERR_ABRT;
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
    tcp_err(pcb, nullptr);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

/** @brief Queues bounded HTTP header and body chunks without blocking. */
void send_pending(TcpPcb* pcb) {
    if (pcb == nullptr || client.load() != pcb || response_in_flight == nullptr)
        return;
    while (response_header_offset < response_header_size) {
        const auto available = static_cast<std::size_t>(tcp_sndbuf(pcb));
        if (available == 0)
            break;
        const auto remaining = response_header_size - response_header_offset;
        const auto amount = std::min({kTransmitChunk, available, remaining});
        const auto result = tcp_write(pcb, response_header.data() + response_header_offset,
            static_cast<u16_t>(amount), TCP_WRITE_FLAG_COPY);
        if (result == ERR_MEM)
            break;
        if (result != ERR_OK) {
            abort_connection(pcb);
            return;
        }
        response_header_offset += amount;
    }
    const auto* body = response_in_flight->body;
    while (response_header_offset == response_header_size &&
           response_body_offset < response_in_flight->size) {
        const auto available = static_cast<std::size_t>(tcp_sndbuf(pcb));
        if (available == 0)
            break;
        const auto remaining = response_in_flight->size - response_body_offset;
        const auto amount = std::min({kTransmitChunk, available, remaining});
        const auto result = tcp_write(
            pcb, body + response_body_offset, static_cast<u16_t>(amount), TCP_WRITE_FLAG_COPY);
        if (result == ERR_MEM)
            break;
        if (result != ERR_OK) {
            abort_connection(pcb);
            return;
        }
        response_body_offset += amount;
    }
    (void)tcp_output(pcb);
    if (response_header_offset == response_header_size &&
        response_body_offset == response_in_flight->size &&
        response_acknowledged == response_header_size + response_in_flight->size) {
        if (request_close) {
            close_after_send(pcb);
        } else {
            response_in_flight = nullptr;
            exchange.request.reset();
            exchange.response.reset();
            phase.store(Phase::waiting);
            connection_deadline = now_milliseconds() + 2000;
            process_received(pcb);
        }
    }
}

/** @brief Builds the fixed HTTP response headers shared by all Pico replies. */
[[nodiscard]] bool build_response_header(const WebResponseView& response) {
    const auto* content_type =
        response.content_type != nullptr ? response.content_type : "application/octet-stream";
    const auto length = static_cast<unsigned long>(response.size);
    int written = std::snprintf(response_header.data(), response_header.size(),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Connection: %s\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; "
        "style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; "
        "form-action 'self'; frame-ancestors 'none'\r\n",
        web_status_line(response.status), content_type, length,
        request_close ? "close" : "keep-alive");
    if (written < 0 || static_cast<std::size_t>(written) >= response_header.size()) {
        return false;
    }
    response_header_size = static_cast<std::size_t>(written);
    if (response.content_encoding != nullptr) {
        const auto suffix = std::snprintf(response_header.data() + response_header_size,
            response_header.size() - response_header_size, "Content-Encoding: %s\r\n",
            response.content_encoding);
        if (suffix < 0 ||
            static_cast<std::size_t>(suffix) >= response_header.size() - response_header_size) {
            return false;
        }
        response_header_size += static_cast<std::size_t>(suffix);
    }
    if (response.cookie[0] != '\0') {
        const auto suffix = std::snprintf(response_header.data() + response_header_size,
            response_header.size() - response_header_size, "Set-Cookie: %s\r\n", response.cookie);
        if (suffix < 0 ||
            static_cast<std::size_t>(suffix) >= response_header.size() - response_header_size) {
            return false;
        }
        response_header_size += static_cast<std::size_t>(suffix);
    }
    if (response_header_size + 2 >= response_header.size())
        return false;
    response_header[response_header_size++] = '\r';
    response_header[response_header_size++] = '\n';
    return true;
}

/** @brief Starts a response from either the application or a parser rejection. */
void queue_response(TcpPcb* pcb, const WebResponseView& response) {
    if (pcb == nullptr || client.load() != pcb || !build_response_header(response)) {
        if (pcb != nullptr)
            abort_connection(pcb);
        return;
    }
    in_flight = response;
    response_in_flight = &in_flight;
    response_header_offset = 0;
    response_body_offset = 0;
    response_acknowledged = 0;
    close_pending = false;
    phase.store(Phase::sending);
    connection_deadline = now_milliseconds() + kConnectionTimeoutMilliseconds;
    send_pending(pcb);
}

/** @brief Sends a short parser or mailbox rejection without entering the app. */
void reject(TcpPcb* pcb, const unsigned int status, const char* message) {
    request_close = true;
    const auto written =
        std::snprintf(rejected_body.data(), rejected_body.size(), "{\"error\":\"%s\"}", message);
    WebResponseView response{};
    response.status = status;
    response.body = rejected_body.data();
    if (written > 0 && static_cast<std::size_t>(written) < rejected_body.size())
        response.size = static_cast<std::size_t>(written);
    queue_response(pcb, response);
}

/** @brief Marks a disconnected connection while preserving an app-owned request. */
void connection_lost() {
    client.store(nullptr);
    receive_size = 0;
    cancelled.store(true);
    const auto current = phase.load();
    if (current == Phase::pending || current == Phase::processing)
        return;
    phase.store(Phase::idle);
    response_in_flight = nullptr;
    close_pending = false;
}

/** @brief Dispatches only after the preceding exchange has relinquished its storage. */
void process_received(TcpPcb* pcb) {
    const auto current = phase.load();
    if ((current != Phase::waiting && current != Phase::receiving) || receive_size == 0)
        return;
    if (current == Phase::waiting) {
        phase.store(Phase::receiving);
        connection_deadline = now_milliseconds() + kConnectionTimeoutMilliseconds;
    }
    const auto end = header_end();
    if (end == 0) {
        if (receive_size >= kHeaderLimit)
            reject(pcb, 431, "Header too large");
        return;
    }
    unsigned int rejection_status = 400;
    const auto parsed = parse_request(end, rejection_status);
    if (parsed == ParseResult::incomplete)
        return;
    if (parsed == ParseResult::rejected) {
        reject(pcb, rejection_status, "Bad request");
        return;
    }
    cancelled.store(false);
    phase.store(Phase::pending);
}

/** @brief Retains bounded stream bytes even while the application owns its exchange. */
err_t receive(void*, TcpPcb* pcb, Pbuf* packet, const err_t error) {
    if (client.load() != pcb) {
        if (packet != nullptr)
            pbuf_free(packet);
        return ERR_OK;
    }
    if (packet == nullptr) {
        connection_lost();
        return close_peer(pcb);
    }
    if (error != ERR_OK) {
        pbuf_free(packet);
        abort_connection(pcb, true);
        return ERR_ABRT;
    }
    const auto total = static_cast<std::size_t>(packet->tot_len);
    if (total > receive_buffer.size() - receive_size) {
        // End the stream rather than discard or dispatch a partial next request.
        pbuf_free(packet);
        abort_connection(pcb, true);
        return ERR_ABRT;
    }
    (void)pbuf_copy_partial(packet, receive_buffer.data() + receive_size, packet->tot_len, 0);
    receive_size += total;
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    process_received(pcb);
    return aborted.load() ? ERR_ABRT : ERR_OK;
}

/** @brief Flushes a response when lwIP acknowledges queued bytes. */
err_t sent(void*, TcpPcb* pcb, u16_t amount) {
    if (client.load() != pcb)
        return ERR_OK;
    if (phase.load() == Phase::sending) {
        response_acknowledged += amount;
        if (close_pending) {
            close_after_send(pcb);
        } else {
            send_pending(pcb);
        }
    }
    return aborted.load() ? ERR_ABRT : ERR_OK;
}

/** @brief Aborts an unexpected TCP error without touching a newer request. */
void error_callback(void*, err_t) {
    connection_lost();
}

/** @brief Enforces receive, application, and response deadlines without blocking. */
err_t poll(void*, TcpPcb* pcb) {
    if (client.load() != pcb)
        return ERR_OK;
    if (deadline_reached(now_milliseconds(), connection_deadline)) {
        const auto current = phase.load();
        if (current == Phase::waiting) {
            close_after_send(pcb);
        } else if (current == Phase::receiving) {
            reject(pcb, 408, "Request timeout");
        } else if (current == Phase::pending || current == Phase::processing) {
            cancelled.store(true);
            abort_connection(pcb, true);
        } else if (current == Phase::sending) {
            abort_connection(pcb);
        }
    } else if (phase.load() == Phase::sending) {
        if (close_pending)
            close_after_send(pcb);
        else
            send_pending(pcb);
    }
    return aborted.load() ? ERR_ABRT : ERR_OK;
}

/** @brief Keeps an accepted HTTP exchange on the interface it addressed. */
void bind_local_interface(TcpPcb* const pcb) {
#if USE_WEB_HTTPS
    // altcp does not expose tcp_bind_netif. lwIP routes replies through the
    // interface that owns the local destination address.
    (void)pcb;
#else
    auto& access_point = cyw43_state.netif[CYW43_ITF_AP];
    if (netif_is_up(&access_point) && ip_addr_cmp(&pcb->local_ip, &access_point.ip_addr)) {
        tcp_bind_netif(pcb, &access_point);
        return;
    }
    auto& station = cyw43_state.netif[CYW43_ITF_STA];
    if (netif_is_up(&station) && ip_addr_cmp(&pcb->local_ip, &station.ip_addr)) {
        tcp_bind_netif(pcb, &station);
    }
#endif
}

/** @brief Accepts exactly one bounded connection and rejects overlap safely. */
err_t accept(void*, TcpPcb* pcb, const err_t error) {
    if (error != ERR_OK || pcb == nullptr)
        return ERR_VAL;
    if (phase.load() == Phase::waiting && client.load() != nullptr)
        abort_connection(client.load());
    if (phase.load() != Phase::idle || client.load() != nullptr) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    client.store(pcb);
    exchange.request.reset();
    exchange.response.reset();
    receive_size = 0;
    cancelled.store(false);
    aborted.store(false);
    phase.store(Phase::receiving);
    bind_local_interface(pcb);
    connection_deadline = now_milliseconds() + kConnectionTimeoutMilliseconds;
    tcp_setprio(pcb, TCP_PRIO_MIN);
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, receive);
    tcp_sent(pcb, sent);
    tcp_err(pcb, error_callback);
    tcp_poll(pcb, poll, kPollInterval);
    return ERR_OK;
}

} // namespace

/** @brief Starts one bounded lwIP HTTP/1.1 listener after Wi-Fi joins. */
bool web_start(const WebTlsIdentity* identity) {
    if (listener != nullptr)
        return true;
#if USE_WEB_HTTPS
    if (identity == nullptr || identity->certificate_size == 0U || identity->private_key_size == 0U)
        return false;
#endif
    cyw43_arch_lwip_begin();
#if USE_WEB_HTTPS
    tls_config = altcp_tls_create_config_server_privkey_cert(
        reinterpret_cast<const u8_t*>(identity->private_key_pem.data()),
        identity->private_key_size + 1U, nullptr, 0U,
        reinterpret_cast<const u8_t*>(identity->certificate_pem.data()),
        identity->certificate_size + 1U);
    if (tls_config == nullptr) {
        cyw43_arch_lwip_end();
        return false;
    }
    auto* pcb = altcp_tls_new(tls_config, IPADDR_TYPE_ANY);
#else
    (void)identity;
    auto* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
#endif
    if (pcb == nullptr) {
#if USE_WEB_HTTPS
        altcp_tls_free_config(tls_config);
        tls_config = nullptr;
#endif
        cyw43_arch_lwip_end();
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, USE_WEB_HTTPS ? kHttpsPort : kHttpPort) != ERR_OK) {
        tcp_abort(pcb);
#if USE_WEB_HTTPS
        altcp_tls_free_config(tls_config);
        tls_config = nullptr;
#endif
        cyw43_arch_lwip_end();
        return false;
    }
    auto* listening = tcp_listen_with_backlog(pcb, 1);
    if (listening == nullptr) {
        tcp_abort(pcb);
#if USE_WEB_HTTPS
        altcp_tls_free_config(tls_config);
        tls_config = nullptr;
#endif
        cyw43_arch_lwip_end();
        return false;
    }
    tcp_accept(listening, accept);
    listener = listening;
    cyw43_arch_lwip_end();
    return true;
}

/** @brief Acquires application ownership after raw lwIP publishes a request. */
WebExchange* web_request() {
    if (phase.load() != Phase::pending)
        return nullptr;
    if (cancelled.load()) {
        phase.store(Phase::idle);
        exchange.request.body.fill(0);
        return nullptr;
    }
    phase.store(Phase::processing);
    auto& output = exchange.response;
    output.reset();
    return &exchange;
}

/** @brief Publishes the application response and queues it through raw lwIP. */
void web_complete() {
    if (phase.load() != Phase::processing)
        return;
    auto* pcb = client.load();
    if (cancelled.load() || pcb == nullptr) {
        phase.store(Phase::idle);
        exchange.request.body.fill(0);
        exchange.response.body.fill(0);
        return;
    }
    cyw43_arch_lwip_begin();
    if (!cancelled.load() && client.load() == pcb && phase.load() == Phase::processing) {
        queue_response(pcb, view(exchange.response));
    }
    cyw43_arch_lwip_end();
}

/** @brief Uses Pico's entropy-backed random generator for either interface. */
bool web_random(void* destination, const std::size_t size) {
    if (destination == nullptr)
        return false;
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;
    while (offset < size) {
        const auto random = get_rand_32();
        const auto amount = std::min(sizeof(random), size - offset);
        std::memcpy(bytes + offset, &random, amount);
        offset += amount;
    }
    return true;
}

} // namespace newmota::platform
