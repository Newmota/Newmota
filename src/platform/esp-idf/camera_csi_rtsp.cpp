/*
  camera_csi_rtsp.cpp - RTSP, RTP and WebSocket streaming of the CSI camera for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_rtsp.cpp
 * @brief Ported from Tasmota's xdrv_81_2_esp32_webcam_CSI_h264.ino on lwIP sockets.
 *
 * One network task owns the RTSP listener (554) and the WebSocket listener (82) and serves
 * one client of each. The processing task sends every access unit as a binary WebSocket
 * message `[key flag][Annex B bytes]` and, after RTSP PLAY, as RFC 6184 RTP over UDP with
 * FU-A fragmentation.
 */
#include "camera_csi_internal.h"

#include "newmota/log.h"

#include "bearssl.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace newmota::platform::camera_csi::detail {
namespace {
constexpr std::uint16_t kRtspPort = 554;
constexpr std::uint16_t kWebSocketPort = 82;
constexpr std::uint16_t kRtpServerPort = 5004;
constexpr std::size_t kRtpPayload = 1400; // MTU margin
constexpr std::uint8_t kRtpPayloadType = 96;
constexpr std::uint32_t kSendTimeoutMs = 2000;

struct Net final {
    int rtsp_listener{-1}, rtsp_client{-1};
    int ws_listener{-1}, ws_client{-1};
    int rtp{-1};
    sockaddr_in rtp_peer{};
    bool playing{};
    std::uint32_t session_id{};
    std::uint16_t sequence{};
    std::uint32_t timestamp{}, ssrc{};
    char request[1024]{};
    std::size_t request_size{};
    TaskHandle_t task{};
    SemaphoreHandle_t lock{};
};

Net net{};
std::atomic<bool> net_running{};

int open_listener(const std::uint16_t port) {
    const int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int yes = 1;
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        lwip_listen(fd, 1) != 0) {
        lwip_close(fd);
        return -1;
    }
    return fd;
}

void close_socket(int& fd) {
    if (fd >= 0)
        lwip_close(fd);
    fd = -1;
}

void set_send_timeout(const int fd) {
    timeval timeout{};
    timeout.tv_sec = kSendTimeoutMs / 1000;
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool send_all(const int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size > 0U) {
        const auto sent = lwip_send(fd, bytes, size, 0);
        if (sent <= 0)
            return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool send_text(const int fd, const char* text) {
    return send_all(fd, text, std::strlen(text));
}

/** @brief Answers one RTSP request with optional extra headers and body. */
void rtsp_reply(const unsigned cseq, const char* extra = nullptr, const char* body = nullptr) {
    char head[160];
    std::snprintf(head, sizeof(head), "RTSP/1.0 200 OK\r\nCSeq: %u\r\n", cseq);
    bool ok = send_text(net.rtsp_client, head) && (!extra || send_text(net.rtsp_client, extra));
    if (ok && body) {
        std::snprintf(
            head, sizeof(head), "Content-Length: %u\r\n\r\n", unsigned(std::strlen(body)));
        ok = send_text(net.rtsp_client, head) && send_text(net.rtsp_client, body);
    } else if (ok) {
        ok = send_text(net.rtsp_client, "\r\n");
    }
    if (!ok)
        close_socket(net.rtsp_client);
}

/** @brief Handles one complete RTSP request in net.request. */
void rtsp_request() {
    const char* request = net.request;
    unsigned cseq{};
    if (const char* line = strcasestr(request, "\nCSeq:"))
        cseq = static_cast<unsigned>(std::strtoul(line + 6, nullptr, 10));
    int client_port{};
    if (const char* port = std::strstr(request, "client_port="))
        client_port = std::atoi(port + 12);
    char buffer[512];
    if (std::strncmp(request, "OPTIONS", 7) == 0) {
        rtsp_reply(cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");
    } else if (std::strncmp(request, "DESCRIBE", 8) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        (void)lwip_getsockname(net.rtsp_client, reinterpret_cast<sockaddr*>(&local), &length);
        char ip[16];
        (void)inet_ntoa_r(local.sin_addr, ip, sizeof(ip));
        std::snprintf(buffer, sizeof(buffer),
            "v=0\r\no=- 0 0 IN IP4 %s\r\ns=Newmota Stream\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;profile-level-id=42001E\r\na=control:track0\r\n",
            ip);
        rtsp_reply(cseq, "Content-Type: application/sdp\r\n", buffer);
    } else if (std::strncmp(request, "SETUP", 5) == 0 && client_port > 0) {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        (void)lwip_getpeername(net.rtsp_client, reinterpret_cast<sockaddr*>(&peer), &length);
        xSemaphoreTake(net.lock, portMAX_DELAY);
        net.rtp_peer = peer;
        net.rtp_peer.sin_port = htons(static_cast<std::uint16_t>(client_port));
        xSemaphoreGive(net.lock);
        net.session_id = 100000U + esp_random() % 900000U;
        std::snprintf(buffer, sizeof(buffer),
            "Session: %u\r\nTransport: RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u\r\n",
            unsigned(net.session_id), client_port, client_port + 1, unsigned(kRtpServerPort),
            unsigned(kRtpServerPort + 1));
        rtsp_reply(cseq, buffer);
    } else if (std::strncmp(request, "PLAY", 4) == 0) {
        net.playing = true;
        std::snprintf(buffer, sizeof(buffer), "Session: %u\r\nRange: npt=0.000-\r\n",
            unsigned(net.session_id));
        rtsp_reply(cseq, buffer);
    } else if (std::strncmp(request, "TEARDOWN", 8) == 0) {
        net.playing = false;
        std::snprintf(buffer, sizeof(buffer), "Session: %u\r\n", unsigned(net.session_id));
        rtsp_reply(cseq, buffer);
        close_socket(net.rtsp_client);
    }
}

/** @brief Reads RTSP bytes and handles each request ending in an empty line. */
void rtsp_receive() {
    const auto space = sizeof(net.request) - 1U - net.request_size;
    const auto received = lwip_recv(net.rtsp_client, net.request + net.request_size, space, 0);
    if (received <= 0) {
        net.playing = false;
        close_socket(net.rtsp_client);
        net.request_size = 0;
        return;
    }
    net.request_size += static_cast<std::size_t>(received);
    net.request[net.request_size] = '\0';
    while (char* end = std::strstr(net.request, "\r\n\r\n")) {
        *end = '\0';
        rtsp_request();
        if (net.rtsp_client < 0) {
            net.request_size = 0;
            return;
        }
        const auto consumed = static_cast<std::size_t>(end + 4 - net.request);
        std::memmove(net.request, net.request + consumed, net.request_size - consumed + 1U);
        net.request_size -= consumed;
    }
    if (net.request_size == sizeof(net.request) - 1U)
        net.request_size = 0; // oversized request: drop it
}

/** @brief Upgrades a fresh connection to a WebSocket (RFC 6455) within one second. */
bool websocket_handshake(const int fd) {
    timeval timeout{1, 0};
    (void)lwip_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char header[1024];
    std::size_t size{};
    while (size < sizeof(header) - 1U) {
        const auto received = lwip_recv(fd, header + size, sizeof(header) - 1U - size, 0);
        if (received <= 0)
            return false;
        size += static_cast<std::size_t>(received);
        header[size] = '\0';
        if (std::strstr(header, "\r\n\r\n"))
            break;
    }
    const char* key = strcasestr(header, "Sec-WebSocket-Key:");
    if (!strcasestr(header, "Upgrade: websocket") || !key)
        return false;
    key += 18;
    while (*key == ' ')
        ++key;
    const auto key_size = std::strcspn(key, "\r\n");
    if (key_size == 0U || key_size > 64U)
        return false;
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::uint8_t hash[20];
    br_sha1_context sha;
    br_sha1_init(&sha);
    br_sha1_update(&sha, key, key_size);
    br_sha1_update(&sha, kGuid, sizeof(kGuid) - 1U);
    br_sha1_out(&sha, hash);
    static constexpr char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char accept[29]{};
    for (std::size_t i = 0, j = 0; i < 21; i += 3, j += 4) {
        const std::uint32_t n = (std::uint32_t(hash[i]) << 16) |
                                (i + 1 < 20 ? std::uint32_t(hash[i + 1]) << 8 : 0U) |
                                (i + 2 < 20 ? hash[i + 2] : 0U);
        accept[j] = kBase64[(n >> 18) & 63U];
        accept[j + 1] = kBase64[(n >> 12) & 63U];
        accept[j + 2] = i + 1 < 20 ? kBase64[(n >> 6) & 63U] : '=';
        accept[j + 3] = i + 2 < 20 ? kBase64[n & 63U] : '=';
    }
    char reply[160];
    std::snprintf(reply, sizeof(reply),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    return send_text(fd, reply);
}

/** @brief Serves the RTSP and WebSocket listeners until stopped. */
void net_task(void*) {
    while (net_running.load()) {
        fd_set read{};
        FD_ZERO(&read);
        int highest = -1;
        for (const int fd : {net.rtsp_listener, net.rtsp_client, net.ws_listener, net.ws_client})
            if (fd >= 0) {
                FD_SET(fd, &read);
                highest = std::max(highest, fd);
            }
        timeval timeout{0, 100000};
        if (highest < 0 || lwip_select(highest + 1, &read, nullptr, nullptr, &timeout) <= 0)
            continue;
        if (net.rtsp_listener >= 0 && FD_ISSET(net.rtsp_listener, &read)) {
            const int fd = lwip_accept(net.rtsp_listener, nullptr, nullptr);
            if (fd >= 0 && net.rtsp_client >= 0) {
                lwip_close(fd); // busy: one RTSP client
            } else if (fd >= 0) {
                set_send_timeout(fd);
                net.rtsp_client = fd;
                net.request_size = 0;
                AddLog(LogLevel::info, "RTSP: client connected");
            }
        }
        if (net.rtsp_client >= 0 && FD_ISSET(net.rtsp_client, &read))
            rtsp_receive();
        if (net.ws_listener >= 0 && FD_ISSET(net.ws_listener, &read)) {
            const int fd = lwip_accept(net.ws_listener, nullptr, nullptr);
            if (fd >= 0 && websocket_handshake(fd)) {
                set_send_timeout(fd);
                xSemaphoreTake(net.lock, portMAX_DELAY);
                close_socket(net.ws_client); // the newest viewer wins, as in Tasmota
                net.ws_client = fd;
                xSemaphoreGive(net.lock);
                AddLog(LogLevel::info, "CAM: WebSocket client connected");
            } else if (fd >= 0) {
                lwip_close(fd);
            }
        }
        if (net.ws_client >= 0 && FD_ISSET(net.ws_client, &read)) {
            std::uint8_t discard[64];
            if (lwip_recv(net.ws_client, discard, sizeof(discard), 0) <= 0) {
                xSemaphoreTake(net.lock, portMAX_DELAY);
                close_socket(net.ws_client);
                xSemaphoreGive(net.lock);
            }
        }
    }
    net.task = nullptr;
    vTaskDelete(nullptr);
}

/** @brief Sends one RTP packet; @p fu prefixes the FU-A indicator and header. */
void rtp_send(const bool marker, const std::uint8_t* payload, const std::size_t size,
    const std::uint8_t* fu) {
    std::uint8_t packet[12 + 2 + kRtpPayload];
    packet[0] = 0x80;
    packet[1] = static_cast<std::uint8_t>(kRtpPayloadType | (marker ? 0x80U : 0U));
    packet[2] = static_cast<std::uint8_t>(net.sequence >> 8);
    packet[3] = static_cast<std::uint8_t>(net.sequence);
    for (int i = 0; i < 4; ++i) {
        packet[4 + i] = static_cast<std::uint8_t>(net.timestamp >> (24 - 8 * i));
        packet[8 + i] = static_cast<std::uint8_t>(net.ssrc >> (24 - 8 * i));
    }
    std::size_t length = 12;
    if (fu) {
        packet[length++] = fu[0];
        packet[length++] = fu[1];
    }
    std::memcpy(packet + length, payload, size);
    length += size;
    (void)lwip_sendto(net.rtp, packet, length, 0, reinterpret_cast<const sockaddr*>(&net.rtp_peer),
        sizeof(net.rtp_peer));
    ++net.sequence;
}

/** @brief Sends one NAL unit whole or as FU-A fragments (RFC 6184). */
void rtp_nal(const std::uint8_t* nal, const std::size_t size) {
    const auto type = static_cast<std::uint8_t>(nal[0] & 0x1FU);
    const bool picture = type == 1U || type == 5U;
    if (size <= kRtpPayload) {
        rtp_send(picture, nal, size, nullptr);
        return;
    }
    const std::uint8_t indicator = static_cast<std::uint8_t>((nal[0] & 0x60U) | 28U);
    for (std::size_t offset = 1; offset < size;) {
        const auto chunk = std::min(size - offset, kRtpPayload - 2U);
        const bool last = offset + chunk >= size;
        const std::uint8_t fu[2]{indicator,
            static_cast<std::uint8_t>(type | (offset == 1 ? 0x80U : 0U) | (last ? 0x40U : 0U))};
        rtp_send(last && picture, nal + offset, chunk, fu);
        offset += chunk;
    }
}
} // namespace

bool h264_net_start() {
    if (net_running.load())
        return true;
    net = {};
    net.lock = xSemaphoreCreateMutex();
    net.rtsp_listener = open_listener(kRtspPort);
    net.ws_listener = open_listener(kWebSocketPort);
    net.rtp = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (net.rtp >= 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kRtpServerPort);
        (void)lwip_bind(net.rtp, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    net.sequence = static_cast<std::uint16_t>(esp_random());
    net.timestamp = esp_random();
    net.ssrc = esp_random();
    if (!net.lock || net.rtsp_listener < 0 || net.ws_listener < 0 || net.rtp < 0) {
        h264_net_stop();
        return false;
    }
    net_running.store(true);
    if (xTaskCreatePinnedToCore(net_task, "camera-net", 6144, nullptr, 4, &net.task, 0) != pdPASS) {
        net.task = nullptr;
        h264_net_stop();
        return false;
    }
    AddLog(LogLevel::info, "CAM: RTSP on %u, WebSocket on %u", unsigned(kRtspPort),
        unsigned(kWebSocketPort));
    return true;
}

void h264_net_stop() {
    net_running.store(false);
    for (int i = 0; i < 20 && net.task; ++i)
        vTaskDelay(pdMS_TO_TICKS(25));
    if (net.task) {
        vTaskDelete(net.task);
        net.task = nullptr;
    }
    close_socket(net.rtsp_client);
    close_socket(net.rtsp_listener);
    close_socket(net.ws_client);
    close_socket(net.ws_listener);
    close_socket(net.rtp);
    if (net.lock)
        vSemaphoreDelete(net.lock);
    net = {};
}

void h264_net_send(const Encoded& frame) {
    if (!net_running.load())
        return;
    xSemaphoreTake(net.lock, portMAX_DELAY);
    if (net.ws_client >= 0) {
        // Binary frame (FIN, opcode 2) with the key flag byte before the stream.
        const std::size_t payload = frame.size + 1U;
        std::uint8_t header[11];
        std::size_t length = 2;
        header[0] = 0x82;
        if (payload < 126U) {
            header[1] = static_cast<std::uint8_t>(payload);
        } else if (payload < 65536U) {
            header[1] = 126;
            header[2] = static_cast<std::uint8_t>(payload >> 8);
            header[3] = static_cast<std::uint8_t>(payload);
            length = 4;
        } else {
            header[1] = 127;
            std::memset(header + 2, 0, 4);
            for (int i = 0; i < 4; ++i)
                header[6 + i] = static_cast<std::uint8_t>(payload >> (24 - 8 * i));
            length = 10;
        }
        header[length++] = frame.key ? 1U : 0U;
        if (!send_all(net.ws_client, header, length) ||
            !send_all(net.ws_client, frame.data, frame.size))
            close_socket(net.ws_client);
    }
    xSemaphoreGive(net.lock);
    if (net.playing && net.rtp >= 0 && net.rtp_peer.sin_port != 0) {
        for_each_nal(frame.data, frame.size, rtp_nal);
        const auto fps = detail::mode().fps ? detail::mode().fps : 30U;
        net.timestamp += 90000U / fps; // 90 kHz clock
    }
}

} // namespace newmota::platform::camera_csi::detail
