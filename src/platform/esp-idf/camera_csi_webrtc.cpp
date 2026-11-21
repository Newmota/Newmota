/*
  camera_csi_webrtc.cpp - WebRTC (ICE-lite, DTLS-SRTP) camera streaming for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file camera_csi_webrtc.cpp
 * @brief Ported from Tasmota's xdrv_81_4_esp32_webcam_CSI_webrtc.ino on lwIP and BearSSL.
 *
 * LAN only: the device is an ICE-lite host candidate on UDP 5005 and a passive DTLS 1.2
 * server with one cipher suite, TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, the extended
 * master secret and use_srtp with SRTP_AES128_CM_HMAC_SHA1_80. A fixed self-signed P-256
 * certificate is pinned by its fingerprint in the SDP. Video is H.264 (RFC 6184), audio Opus or
 * G.711 A-law. Each track sends an RTCP sender report every few seconds, and a picture loss or
 * full intra request from the viewer asks the encoder for an IDR. The device either answers
 * an offer or, for Matter, makes one with setup:actpass and stays the DTLS server. A
 * handshake task owns the socket's receive side; senders share it under a mutex.
 */
#include "camera_csi_internal.h"

#include "newmota_features.h"
#include "newmota/log.h"

#include "bearssl.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include <sys/time.h>

namespace newmota::platform::camera_csi {
namespace detail {
namespace {
constexpr std::uint16_t kPort = 5005;
constexpr std::size_t kAuthTag = 10;
constexpr std::size_t kMasterKey = 16;
constexpr std::size_t kMasterSalt = 14;
constexpr std::size_t kMaxPayload = 1100;
constexpr std::uint8_t kLabelEncryption = 0x00, kLabelAuthentication = 0x01, kLabelSalt = 0x02;
constexpr std::uint8_t kRtcpLabels = 0x03; // SRTCP's labels follow SRTP's
constexpr std::uint32_t kReportMilliseconds = 5000;
constexpr std::size_t kSrtcpTrailer = 4; // E flag and the 31-bit index

constexpr std::uint16_t kDtlsVersion = 0xFEFD; // DTLS 1.2
constexpr std::uint8_t kChangeCipherSpec = 20, kAlert = 21, kHandshake = 22;
constexpr std::size_t kRecordHeader = 13, kHandshakeHeader = 12;
constexpr std::uint8_t kClientHello = 1, kServerHello = 2, kHelloVerify = 3, kCertificate = 11,
                       kServerKeyExchange = 12, kServerHelloDone = 14, kClientKeyExchange = 16,
                       kFinished = 20;
constexpr std::uint16_t kCipherSuite = 0xC02B; // ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
constexpr std::uint16_t kNamedCurveP256 = 0x0017;
constexpr std::uint16_t kExtUseSrtp = 0x000E, kExtPointFormats = 0x000B,
                        kExtExtendedMasterSecret = 0x0017;
constexpr std::uint16_t kSrtpAes128Sha1_80 = 0x0001;
constexpr std::size_t kGcmTag = 16, kGcmExplicitIv = 8, kCookie = 32;

enum class State : std::uint8_t { idle, offer, answered, handshaking, connected, streaming };
enum class Handshake : std::uint8_t { idle, wait_hello, wait_cookie, wait_client, done };

struct Srtp final {
    std::uint8_t master_key[kMasterKey], master_salt[kMasterSalt];
    std::uint8_t encryption_key[16], salt_key[14], auth_key[20];
    br_aes_big_ctr_keys aes;
    br_hmac_key_context hmac;
};

struct Track final {
    bool active;
    std::uint8_t payload_type;
    std::uint32_t ssrc, timestamp, roc;
    std::uint16_t sequence;
    Srtp srtp, srtcp;
    std::uint32_t packets, octets, report_index, next_report;
};

struct Session final {
    State state;
    Handshake hs;
    char ice_ufrag[10], ice_pwd[34];
    std::uint8_t fingerprint[32];
    char fingerprint_text[96];
    std::uint8_t client_random[32], server_random[32];
    br_sha256_context transcript;
    std::uint8_t cookie_secret[32], expected_cookie[kCookie];
    std::uint16_t server_message_seq;
    std::uint16_t epoch_in, epoch_out;
    std::uint64_t seq_out;
    br_ec_private_key certificate_key;
    std::uint8_t certificate_key_bytes[32];
    std::uint8_t ecdhe_private[32];
    br_ec_private_key ecdhe_key;
    std::uint8_t ecdhe_public[65];
    std::size_t ecdhe_public_size;
    std::uint8_t master_secret[48];
    std::uint8_t client_write_key[16], server_write_key[16];
    std::uint8_t client_write_iv[4], server_write_iv[4];
    br_aes_big_ctr_keys encrypt_aes, decrypt_aes;
    br_gcm_context encrypt, decrypt;
    std::uint8_t last_flight[1400];
    std::size_t last_flight_size;
    Track video, audio;
    Srtp peer_srtcp; // the viewer's reports and feedback
    std::uint32_t peer_srtcp_index;
    sockaddr_in peer;
    bool client_key_exchange_done, use_srtp;
    bool waiting_for_idr;
};

Session* rtc{};
int udp = -1;
SemaphoreHandle_t udp_lock{};
TaskHandle_t handshake_task{};
std::atomic<bool> task_running{};

// Self-signed P-256 certificate and private key, Tasmota's fixed DTLS identity.
const std::uint8_t kCertificateDer[] = {0x30, 0x82, 0x01, 0x7d, 0x30, 0x82, 0x01, 0x23, 0xa0, 0x03,
    0x02, 0x01, 0x02, 0x02, 0x14, 0x28, 0xf0, 0xc1, 0xb2, 0x32, 0x73, 0xeb, 0x5f, 0x42, 0xfe, 0x73,
    0xef, 0xd7, 0xbb, 0x55, 0x3c, 0xfa, 0x33, 0xf7, 0x7e, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48,
    0xce, 0x3d, 0x04, 0x03, 0x02, 0x30, 0x14, 0x31, 0x12, 0x30, 0x10, 0x06, 0x03, 0x55, 0x04, 0x03,
    0x0c, 0x09, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x68, 0x6f, 0x73, 0x74, 0x30, 0x1e, 0x17, 0x0d, 0x32,
    0x36, 0x30, 0x32, 0x31, 0x30, 0x31, 0x34, 0x33, 0x31, 0x32, 0x37, 0x5a, 0x17, 0x0d, 0x33, 0x36,
    0x30, 0x32, 0x30, 0x38, 0x31, 0x34, 0x33, 0x31, 0x32, 0x37, 0x5a, 0x30, 0x14, 0x31, 0x12, 0x30,
    0x10, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x09, 0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x68, 0x6f, 0x73,
    0x74, 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08,
    0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04, 0x99, 0xf3, 0x76, 0x36,
    0x7d, 0xeb, 0x8d, 0xc2, 0xdd, 0xb8, 0xbb, 0x4a, 0x15, 0xff, 0xf9, 0xe2, 0x2c, 0x2e, 0x13, 0xfb,
    0xba, 0x5e, 0xe9, 0xfd, 0x90, 0xea, 0xae, 0x2d, 0x75, 0x50, 0x60, 0x5e, 0xe9, 0x5c, 0xb8, 0x6f,
    0x61, 0xec, 0xf3, 0x06, 0xaf, 0xb2, 0xfc, 0x93, 0xe2, 0xe3, 0xe5, 0x61, 0x0a, 0x0a, 0x9b, 0x62,
    0x0c, 0x77, 0xee, 0x7b, 0x75, 0xac, 0xe7, 0x46, 0xe0, 0xf8, 0xc6, 0xf0, 0xa3, 0x53, 0x30, 0x51,
    0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0xa1, 0x10, 0xf5, 0x8a, 0x18,
    0x1a, 0xba, 0x0f, 0x8d, 0x2b, 0x19, 0x09, 0xd3, 0xf4, 0xcb, 0x95, 0x73, 0x8f, 0xca, 0x8e, 0x30,
    0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14, 0xa1, 0x10, 0xf5, 0x8a,
    0x18, 0x1a, 0xba, 0x0f, 0x8d, 0x2b, 0x19, 0x09, 0xd3, 0xf4, 0xcb, 0x95, 0x73, 0x8f, 0xca, 0x8e,
    0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff, 0x04, 0x05, 0x30, 0x03, 0x01, 0x01,
    0xff, 0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02, 0x03, 0x48, 0x00,
    0x30, 0x45, 0x02, 0x21, 0x00, 0xa0, 0x14, 0x0a, 0x5e, 0x9a, 0x69, 0xc9, 0x86, 0x75, 0xa7, 0x1b,
    0x0e, 0xef, 0xf6, 0xe4, 0x95, 0x8b, 0x3d, 0x7d, 0xc6, 0xfe, 0xc0, 0x87, 0xf5, 0xef, 0x2b, 0x8f,
    0xe1, 0xa9, 0x39, 0xb7, 0x19, 0x02, 0x20, 0x57, 0x28, 0xfc, 0x0f, 0x89, 0xb2, 0x63, 0x9f, 0xf9,
    0xe9, 0x27, 0xcf, 0x4c, 0x01, 0xb1, 0x04, 0xf9, 0x26, 0x7b, 0xae, 0x4c, 0xa1, 0xe2, 0xb0, 0x82,
    0x07, 0xfc, 0xa8, 0xfe, 0x03, 0xe4, 0x9f};
const std::uint8_t kCertificateKey[32] = {0xab, 0x12, 0x37, 0x06, 0x41, 0x58, 0x47, 0x5b, 0x7f,
    0x9c, 0x90, 0x46, 0x6f, 0x27, 0x94, 0xc9, 0x78, 0x8c, 0x6c, 0x63, 0xf0, 0xb8, 0xad, 0x90, 0xd6,
    0xbf, 0xa5, 0x79, 0xb4, 0xec, 0x1d, 0x72};

void put16(std::uint8_t* p, const std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void put24(std::uint8_t* p, const std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    put16(p + 1, v);
}

void put48(std::uint8_t* p, const std::uint64_t v) {
    for (int i = 0; i < 6; ++i)
        p[i] = static_cast<std::uint8_t>(v >> (40 - 8 * i));
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t get24(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
}

std::size_t record_header(std::uint8_t* buffer, const std::uint8_t type, const std::uint16_t epoch,
    const std::uint64_t sequence, const std::uint16_t size) {
    buffer[0] = type;
    put16(buffer + 1, kDtlsVersion);
    put16(buffer + 3, epoch);
    put48(buffer + 5, sequence);
    put16(buffer + 11, size);
    return kRecordHeader;
}

std::size_t handshake_header(std::uint8_t* buffer, const std::uint8_t type,
    const std::uint32_t size, const std::uint16_t sequence) {
    buffer[0] = type;
    put24(buffer + 1, size);
    put16(buffer + 4, sequence);
    put24(buffer + 6, 0);
    put24(buffer + 9, size);
    return kHandshakeHeader;
}

void send_to(const sockaddr_in& peer, const std::uint8_t* data, const std::size_t size) {
    if (udp < 0 || peer.sin_port == 0)
        return;
    xSemaphoreTake(udp_lock, portMAX_DELAY);
    (void)lwip_sendto(udp, data, size, 0, reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    xSemaphoreGive(udp_lock);
}

void send_raw(const std::uint8_t* data, const std::size_t size) {
    send_to(rtc->peer, data, size);
}

void remember_flight(const std::uint8_t* data, const std::size_t size) {
    std::memcpy(rtc->last_flight, data, size);
    rtc->last_flight_size = size;
}

/** @brief AES-128-GCM record with the 8-byte explicit nonce (RFC 5288). */
std::size_t encrypt_record(std::uint8_t* out, const std::uint8_t type,
    const std::uint8_t* plaintext, const std::size_t size) {
    std::uint8_t nonce[12], aad[13];
    std::memcpy(nonce, rtc->server_write_iv, 4);
    put16(nonce + 4, rtc->epoch_out);
    put48(nonce + 6, rtc->seq_out);
    put16(aad, rtc->epoch_out);
    put48(aad + 2, rtc->seq_out);
    aad[8] = type;
    put16(aad + 9, kDtlsVersion);
    put16(aad + 11, static_cast<std::uint32_t>(size));
    std::size_t position = record_header(out, type, rtc->epoch_out, rtc->seq_out,
        static_cast<std::uint16_t>(kGcmExplicitIv + size + kGcmTag));
    std::memcpy(out + position, nonce + 4, kGcmExplicitIv);
    position += kGcmExplicitIv;
    std::memcpy(out + position, plaintext, size);
    br_gcm_reset(&rtc->encrypt, nonce, 12);
    br_gcm_aad_inject(&rtc->encrypt, aad, 13);
    br_gcm_flip(&rtc->encrypt);
    br_gcm_run(&rtc->encrypt, 1, out + position, size);
    position += size;
    br_gcm_get_tag(&rtc->encrypt, out + position);
    ++rtc->seq_out;
    return position + kGcmTag;
}

bool decrypt_record(const std::uint8_t* record, const std::size_t size, std::uint8_t* out,
    std::size_t capacity, std::size_t& out_size) {
    if (size < kRecordHeader + kGcmExplicitIv + kGcmTag)
        return false;
    const std::uint16_t payload = get16(record + 11);
    if (kRecordHeader + payload > size || payload < kGcmExplicitIv + kGcmTag)
        return false;
    const std::size_t length = payload - kGcmExplicitIv - kGcmTag;
    if (length > capacity)
        return false;
    const std::uint8_t* explicit_iv = record + kRecordHeader;
    const std::uint8_t* ciphertext = explicit_iv + kGcmExplicitIv;
    std::uint8_t nonce[12], aad[13];
    std::memcpy(nonce, rtc->client_write_iv, 4);
    std::memcpy(nonce + 4, explicit_iv, 8);
    std::memcpy(aad, record + 3, 8); // epoch and sequence
    aad[8] = record[0];
    std::memcpy(aad + 9, record + 1, 2);
    put16(aad + 11, static_cast<std::uint32_t>(length));
    std::memcpy(out, ciphertext, length);
    br_gcm_reset(&rtc->decrypt, nonce, 12);
    br_gcm_aad_inject(&rtc->decrypt, aad, 13);
    br_gcm_flip(&rtc->decrypt);
    br_gcm_run(&rtc->decrypt, 0, out, length);
    if (!br_gcm_check_tag(&rtc->decrypt, ciphertext + length)) {
        AddLog(LogLevel::error, "WebRTC: GCM tag check failed");
        return false;
    }
    out_size = length;
    return true;
}

/** @brief Answers an ICE connectivity check (RFC 5389 Binding, RFC 8445 ICE-lite). */
void handle_stun(const std::uint8_t* message, const std::size_t size, const sockaddr_in& from) {
    if (size < 20 || message[0] != 0 || message[1] != 1 || message[4] != 0x21 ||
        message[5] != 0x12 || message[6] != 0xA4 || message[7] != 0x42)
        return;
    rtc->peer = from; // latch, or follow an ICE restart
    std::uint8_t response[100]{};
    response[0] = 0x01; // Binding success
    response[1] = 0x01;
    std::memcpy(response + 4, message + 4, 16); // magic cookie and transaction ID
    std::size_t position = 20;
    // XOR-MAPPED-ADDRESS
    put16(response + position, 0x0020);
    put16(response + position + 2, 8);
    put16(response + position + 4, 0x0001);
    put16(response + position + 6, ntohs(from.sin_port) ^ 0x2112U);
    const auto* ip = reinterpret_cast<const std::uint8_t*>(&from.sin_addr.s_addr);
    static constexpr std::uint8_t kMagic[4] = {0x21, 0x12, 0xA4, 0x42};
    for (int i = 0; i < 4; ++i)
        response[position + 8 + i] = ip[i] ^ kMagic[i];
    position += 12;
    // MESSAGE-INTEGRITY over the message whose length already counts this attribute.
    put16(response + 2, static_cast<std::uint32_t>(position - 20 + 24));
    br_hmac_key_context key;
    br_hmac_key_init(&key, &br_sha1_vtable, rtc->ice_pwd, std::strlen(rtc->ice_pwd));
    br_hmac_context hmac;
    br_hmac_init(&hmac, &key, 0);
    br_hmac_update(&hmac, response, position);
    put16(response + position, 0x0008);
    put16(response + position + 2, 20);
    br_hmac_out(&hmac, response + position + 4);
    position += 24;
    // FINGERPRINT: CRC-32 of everything before it, XOR 0x5354554E.
    put16(response + 2, static_cast<std::uint32_t>(position - 20 + 8));
    put16(response + position, 0x8028);
    put16(response + position + 2, 4);
    const std::uint32_t crc = esp_rom_crc32_le(0, response, position) ^ 0x5354554EU;
    for (int i = 0; i < 4; ++i)
        response[position + 4 + i] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
    position += 8;
    send_to(from, response, position);
}

void compute_cookie(std::uint8_t* cookie) {
    br_hmac_key_context key;
    br_hmac_key_init(&key, &br_sha256_vtable, rtc->cookie_secret, sizeof(rtc->cookie_secret));
    br_hmac_context hmac;
    br_hmac_init(&hmac, &key, kCookie);
    br_hmac_update(&hmac, rtc->client_random, 32);
    br_hmac_update(&hmac, &rtc->peer.sin_addr.s_addr, 4);
    br_hmac_update(&hmac, &rtc->peer.sin_port, 2);
    br_hmac_out(&hmac, cookie);
}

void send_hello_verify() {
    compute_cookie(rtc->expected_cookie);
    std::uint8_t packet[kRecordHeader + kHandshakeHeader + 3 + kCookie];
    std::uint8_t* body = packet + kRecordHeader + kHandshakeHeader;
    put16(body, kDtlsVersion);
    body[2] = kCookie;
    std::memcpy(body + 3, rtc->expected_cookie, kCookie);
    handshake_header(packet + kRecordHeader, kHelloVerify, 3 + kCookie, rtc->server_message_seq++);
    record_header(packet, kHandshake, 0, rtc->seq_out++, kHandshakeHeader + 3 + kCookie);
    remember_flight(packet, sizeof(packet));
    send_raw(packet, sizeof(packet));
}

/** @brief Appends one handshake message as its own epoch-0 record and hashes it. */
bool append_message(std::uint8_t* flight, std::size_t& position, const std::size_t capacity,
    const std::uint8_t type, const std::uint8_t* body, const std::size_t size) {
    if (position + kRecordHeader + kHandshakeHeader + size > capacity)
        return false;
    record_header(flight + position, kHandshake, 0, rtc->seq_out++,
        static_cast<std::uint16_t>(kHandshakeHeader + size));
    position += kRecordHeader;
    handshake_header(
        flight + position, type, static_cast<std::uint32_t>(size), rtc->server_message_seq++);
    if (size)
        std::memcpy(flight + position + kHandshakeHeader, body, size);
    br_sha256_update(&rtc->transcript, flight + position, kHandshakeHeader + size);
    position += kHandshakeHeader + size;
    return true;
}

/** @brief ServerHello, Certificate, ServerKeyExchange and ServerHelloDone in one datagram. */
bool send_server_flight() {
    const br_ec_impl* ec = br_ec_get_default();
    esp_fill_random(rtc->server_random, 32);
    br_hmac_drbg_context rng;
    std::uint8_t seed[48];
    esp_fill_random(seed, sizeof(seed));
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof(seed));
    if (br_ec_keygen(&rng.vtable, ec, &rtc->ecdhe_key, rtc->ecdhe_private, BR_EC_secp256r1) == 0)
        return false;
    rtc->ecdhe_public_size = br_ec_compute_pub(ec, nullptr, rtc->ecdhe_public, &rtc->ecdhe_key);
    if (rtc->ecdhe_public_size == 0)
        return false;

    std::uint8_t flight[1400];
    std::size_t position{};
    // ServerHello with use_srtp, uncompressed points and extended_master_secret.
    std::uint8_t hello[64];
    std::size_t h{};
    put16(hello, kDtlsVersion);
    h += 2;
    std::memcpy(hello + h, rtc->server_random, 32);
    h += 32;
    hello[h++] = 0; // no session ID
    put16(hello + h, kCipherSuite);
    h += 2;
    hello[h++] = 0; // no compression
    const std::size_t extensions = h;
    h += 2;
    put16(hello + h, kExtUseSrtp);
    put16(hello + h + 2, 5);
    put16(hello + h + 4, 2);
    put16(hello + h + 6, kSrtpAes128Sha1_80);
    hello[h + 8] = 0; // no MKI
    h += 9;
    put16(hello + h, kExtPointFormats);
    put16(hello + h + 2, 2);
    hello[h + 4] = 1;
    hello[h + 5] = 0;
    h += 6;
    put16(hello + h, kExtExtendedMasterSecret);
    put16(hello + h + 2, 0);
    h += 4;
    put16(hello + extensions, static_cast<std::uint32_t>(h - extensions - 2));
    if (!append_message(flight, position, sizeof(flight), kServerHello, hello, h))
        return false;

    std::uint8_t certificate[6 + sizeof(kCertificateDer)];
    put24(certificate, 3 + sizeof(kCertificateDer));
    put24(certificate + 3, sizeof(kCertificateDer));
    std::memcpy(certificate + 6, kCertificateDer, sizeof(kCertificateDer));
    if (!append_message(
            flight, position, sizeof(flight), kCertificate, certificate, sizeof(certificate)))
        return false;

    // ServerKeyExchange: ECParameters, the ephemeral key and its ECDSA-SHA256 signature.
    std::uint8_t exchange[4 + 65 + 4 + 80];
    std::size_t e{};
    exchange[e++] = 3; // named_curve
    put16(exchange + e, kNamedCurveP256);
    e += 2;
    exchange[e++] = static_cast<std::uint8_t>(rtc->ecdhe_public_size);
    std::memcpy(exchange + e, rtc->ecdhe_public, rtc->ecdhe_public_size);
    e += rtc->ecdhe_public_size;
    br_sha256_context hash;
    br_sha256_init(&hash);
    br_sha256_update(&hash, rtc->client_random, 32);
    br_sha256_update(&hash, rtc->server_random, 32);
    br_sha256_update(&hash, exchange, e);
    std::uint8_t digest[32];
    br_sha256_out(&hash, digest);
    std::uint8_t signature[80];
    const auto signature_size = br_ecdsa_sign_asn1_get_default()(
        ec, &br_sha256_vtable, digest, &rtc->certificate_key, signature);
    if (signature_size == 0)
        return false;
    exchange[e++] = 4; // SHA-256
    exchange[e++] = 3; // ECDSA
    put16(exchange + e, static_cast<std::uint32_t>(signature_size));
    e += 2;
    std::memcpy(exchange + e, signature, signature_size);
    e += signature_size;
    if (!append_message(flight, position, sizeof(flight), kServerKeyExchange, exchange, e) ||
        !append_message(flight, position, sizeof(flight), kServerHelloDone, nullptr, 0))
        return false;
    remember_flight(flight, position);
    send_raw(flight, position);
    return true;
}

/** @brief Extended master secret (RFC 7627) and the GCM keys. */
void derive_keys(const std::uint8_t* premaster, const std::size_t size) {
    std::uint8_t session_hash[32];
    br_sha256_context copy = rtc->transcript;
    br_sha256_out(&copy, session_hash);
    br_tls_prf_seed_chunk chunk{session_hash, sizeof(session_hash)};
    br_tls12_sha256_prf(
        rtc->master_secret, 48, premaster, size, "extended master secret", 1, &chunk);
    std::uint8_t seed[64];
    std::memcpy(seed, rtc->server_random, 32);
    std::memcpy(seed + 32, rtc->client_random, 32);
    chunk = {seed, sizeof(seed)};
    std::uint8_t block[40];
    br_tls12_sha256_prf(block, sizeof(block), rtc->master_secret, 48, "key expansion", 1, &chunk);
    std::memcpy(rtc->client_write_key, block, 16);
    std::memcpy(rtc->server_write_key, block + 16, 16);
    std::memcpy(rtc->client_write_iv, block + 32, 4);
    std::memcpy(rtc->server_write_iv, block + 36, 4);
    br_aes_big_ctr_init(&rtc->encrypt_aes, rtc->server_write_key, 16);
    br_gcm_init(&rtc->encrypt, &rtc->encrypt_aes.vtable, br_ghash_ctmul32);
    br_aes_big_ctr_init(&rtc->decrypt_aes, rtc->client_write_key, 16);
    br_gcm_init(&rtc->decrypt, &rtc->decrypt_aes.vtable, br_ghash_ctmul32);
}

void finished_data(const char* label, std::uint8_t* out) {
    std::uint8_t digest[32];
    br_sha256_context copy = rtc->transcript;
    br_sha256_out(&copy, digest);
    br_tls_prf_seed_chunk chunk{digest, sizeof(digest)};
    br_tls12_sha256_prf(out, 12, rtc->master_secret, 48, label, 1, &chunk);
}

bool send_server_finished() {
    std::uint8_t flight[128];
    std::size_t position = record_header(flight, kChangeCipherSpec, 0, rtc->seq_out++, 1);
    flight[position++] = 1;
    rtc->epoch_out = 1;
    rtc->seq_out = 0;
    std::uint8_t finished[kHandshakeHeader + 12];
    handshake_header(finished, kFinished, 12, rtc->server_message_seq++);
    finished_data("server finished", finished + kHandshakeHeader);
    position += encrypt_record(flight + position, kHandshake, finished, sizeof(finished));
    remember_flight(flight, position);
    send_raw(flight, position);
    return true;
}

/** @brief SRTP session keys (RFC 3711 §4.3, AES-CM PRF). */
void srtp_kdf(Srtp& srtp, const std::uint8_t label) {
    std::uint8_t iv[16]{};
    std::memcpy(iv, srtp.master_salt, kMasterSalt);
    iv[7] ^= label;
    std::uint8_t out[32]{};
    br_aes_big_ctr_keys kdf;
    br_aes_big_ctr_init(&kdf, srtp.master_key, kMasterKey);
    // BearSSL's counter covers bytes 12..15; SRTP's 16-bit block counter is 14..15.
    const std::uint32_t counter = (std::uint32_t(iv[12]) << 24) | (std::uint32_t(iv[13]) << 16);
    const std::uint8_t kind = label % kRtcpLabels;
    const std::size_t size = kind == kLabelAuthentication ? 20 : 16;
    br_aes_big_ctr_run(&kdf, iv, counter, out, size);
    if (kind == kLabelEncryption)
        std::memcpy(srtp.encryption_key, out, 16);
    else if (kind == kLabelSalt)
        std::memcpy(srtp.salt_key, out, 14);
    else
        std::memcpy(srtp.auth_key, out, 20);
}

/** @brief Session keys of SRTP (label base 0) or SRTCP (base 3) from a master key and salt. */
void derive(
    Srtp& srtp, const std::uint8_t* key, const std::uint8_t* salt, const std::uint8_t base) {
    std::memcpy(srtp.master_key, key, kMasterKey);
    std::memcpy(srtp.master_salt, salt, kMasterSalt);
    srtp_kdf(srtp, static_cast<std::uint8_t>(base + kLabelEncryption));
    srtp_kdf(srtp, static_cast<std::uint8_t>(base + kLabelSalt));
    srtp_kdf(srtp, static_cast<std::uint8_t>(base + kLabelAuthentication));
    br_aes_big_ctr_init(&srtp.aes, srtp.encryption_key, 16);
    br_hmac_key_init(&srtp.hmac, &br_sha1_vtable, srtp.auth_key, 20);
}

/** @brief Both sides' keys from the exporter "EXTRACTOR-dtls_srtp" (RFC 5764). */
void export_srtp_keys() {
    std::uint8_t seed[64];
    std::memcpy(seed, rtc->client_random, 32);
    std::memcpy(seed + 32, rtc->server_random, 32);
    br_tls_prf_seed_chunk chunk{seed, sizeof(seed)};
    std::uint8_t material[60];
    br_tls12_sha256_prf(
        material, sizeof(material), rtc->master_secret, 48, "EXTRACTOR-dtls_srtp", 1, &chunk);
    // Client key, server key, client salt, server salt.
    for (Track* track : {&rtc->video, &rtc->audio}) {
        derive(track->srtp, material + 16, material + 46, 0);
        derive(track->srtcp, material + 16, material + 46, kRtcpLabels);
        track->roc = 0;
    }
    derive(rtc->peer_srtcp, material, material + 32, kRtcpLabels);
}

/** @brief Reads the client random, the cookie and whether the client offers our profile. */
bool parse_client_hello(const std::uint8_t* body, const std::size_t size, const bool retry) {
    if (size < 38)
        return false;
    std::size_t p = 2;
    std::memcpy(rtc->client_random, body + p, 32);
    p += 32;
    p += 1U + body[p];
    if (p >= size)
        return false;
    const std::uint8_t cookie = body[p++];
    if (p + cookie > size)
        return false;
    if (!retry) // the first hello only provides the random for the cookie
        return true;
    if (cookie != kCookie || std::memcmp(body + p, rtc->expected_cookie, kCookie) != 0)
        return false;
    p += cookie;
    rtc->use_srtp = false;
    if (p + 2 > size)
        return true;
    p += 2U + get16(body + p); // cipher suites
    if (p + 1 > size)
        return true;
    p += 1U + body[p]; // compression methods
    if (p + 2 > size)
        return true;
    const std::size_t end = std::min(size, p + 2U + get16(body + p));
    p += 2;
    while (p + 4 <= end) {
        const std::uint16_t type = get16(body + p), length = get16(body + p + 2);
        p += 4;
        if (p + length > end)
            break;
        if (type == kExtUseSrtp && length >= 4) {
            const std::uint16_t profiles = get16(body + p);
            for (std::size_t i = 0; i + 1 < profiles && 2 + i + 1 < length; i += 2)
                if (get16(body + p + 2 + i) == kSrtpAes128Sha1_80)
                    rtc->use_srtp = true;
        }
        p += length;
    }
    return true;
}

void handshake_complete() {
    rtc->hs = Handshake::done;
    rtc->state = State::connected;
    if (!rtc->use_srtp) {
        AddLog(LogLevel::error, "WebRTC: DTLS done, but no SRTP profile agreed");
        return;
    }
    export_srtp_keys();
    rtc->waiting_for_idr = true;
    rtc->video.active = rtc->audio.active = true;
    rtc->state = State::streaming;
    AddLog(LogLevel::info, "WebRTC: DTLS-SRTP established");
}

void handle_handshake_record(const std::uint8_t* record, const std::size_t size) {
    const std::uint16_t epoch = get16(record + 3);
    const std::uint16_t payload_size = get16(record + 11);
    const std::uint8_t* payload = record + kRecordHeader;
    if (kRecordHeader + payload_size > size)
        return;
    if (epoch == 0 && rtc->epoch_in == 0) {
        if (payload_size < kHandshakeHeader)
            return;
        const std::uint8_t type = payload[0];
        const std::uint32_t length = get24(payload + 1);
        if (get24(payload + 6) != 0 || get24(payload + 9) != length ||
            kHandshakeHeader + length > payload_size)
            return; // fragmented handshake messages are not supported
        const std::uint8_t* body = payload + kHandshakeHeader;
        switch (rtc->hs) {
        case Handshake::wait_hello:
            if (type == kClientHello && parse_client_hello(body, length, false)) {
                send_hello_verify();
                rtc->hs = Handshake::wait_cookie;
                rtc->state = State::handshaking;
            }
            break;
        case Handshake::wait_cookie: {
            if (type != kClientHello || length < 38)
                break;
            const std::size_t cookie_at = 35U + body[34];
            if (cookie_at >= length)
                break;
            if (body[cookie_at] == 0) { // retransmitted first hello: repeat the same HVR
                send_raw(rtc->last_flight, rtc->last_flight_size);
                break;
            }
            if (!parse_client_hello(body, length, true)) {
                send_hello_verify();
                break;
            }
            br_sha256_init(&rtc->transcript);
            br_sha256_update(&rtc->transcript, payload, kHandshakeHeader + length);
            rtc->client_key_exchange_done = false;
            if (send_server_flight())
                rtc->hs = Handshake::wait_client;
            else
                AddLog(LogLevel::error, "WebRTC: server flight failed");
            break;
        }
        case Handshake::wait_client:
            if (type == kClientHello) {
                send_raw(rtc->last_flight, rtc->last_flight_size);
            } else if (type == kClientKeyExchange && !rtc->client_key_exchange_done &&
                       length >= 1 && 1U + body[0] <= length && body[0] >= 2) {
                const br_ec_impl* ec = br_ec_get_default();
                std::uint8_t point[65];
                const std::size_t point_size = body[0];
                if (point_size > sizeof(point))
                    break;
                std::memcpy(point, body + 1, point_size);
                if (!ec->mul(point, point_size, rtc->ecdhe_key.x, rtc->ecdhe_key.xlen,
                        BR_EC_secp256r1)) {
                    AddLog(LogLevel::error, "WebRTC: ECDH failed");
                    break;
                }
                std::size_t x_size{};
                const std::size_t x_at = ec->xoff(BR_EC_secp256r1, &x_size);
                br_sha256_update(&rtc->transcript, payload, kHandshakeHeader + length);
                derive_keys(point + x_at, x_size);
                rtc->client_key_exchange_done = true;
            }
            break;
        default:
            break;
        }
    } else if (epoch == 1 && rtc->epoch_in == 1 && rtc->hs == Handshake::wait_client) {
        std::uint8_t plain[256];
        std::size_t plain_size{};
        if (!decrypt_record(record, size, plain, sizeof(plain), plain_size) ||
            plain_size < kHandshakeHeader + 12 || plain[0] != kFinished || get24(plain + 1) != 12)
            return;
        std::uint8_t expected[12];
        finished_data("client finished", expected);
        if (std::memcmp(plain + kHandshakeHeader, expected, 12) != 0) {
            AddLog(LogLevel::error, "WebRTC: client Finished mismatch");
            return;
        }
        br_sha256_update(&rtc->transcript, plain, kHandshakeHeader + 12);
        if (send_server_finished())
            handshake_complete();
    }
}

/** @brief The AES-CM IV of RFC 3711 for an SSRC and packet index. */
void srtp_iv(const Srtp& srtp, const std::uint32_t ssrc, const std::uint64_t index,
    std::uint8_t (&iv)[16], std::uint32_t& counter) {
    std::memset(iv, 0, sizeof(iv));
    std::memcpy(iv, srtp.salt_key, 14);
    for (int i = 0; i < 4; ++i)
        iv[4 + i] ^= static_cast<std::uint8_t>(ssrc >> (24 - 8 * i));
    for (int i = 0; i < 6; ++i)
        iv[8 + i] ^= static_cast<std::uint8_t>(index >> (40 - 8 * i));
    counter = (std::uint32_t(iv[12]) << 24) | (std::uint32_t(iv[13]) << 16);
}

void srtp_tag(const Srtp& srtp, const std::uint8_t* data, const std::size_t size,
    const std::uint8_t* extra, const std::size_t extra_size, std::uint8_t* tag) {
    br_hmac_context hmac;
    br_hmac_init(&hmac, &srtp.hmac, 0);
    br_hmac_update(&hmac, data, size);
    if (extra_size)
        br_hmac_update(&hmac, extra, extra_size);
    std::uint8_t full[20];
    br_hmac_out(&hmac, full);
    std::memcpy(tag, full, kAuthTag);
}

/** @brief Checks and decrypts the viewer's SRTCP; asks for an IDR on PLI or FIR. */
void handle_rtcp(std::uint8_t* data, std::size_t size) {
    if (rtc->state != State::streaming || size < 8 + kSrtcpTrailer + kAuthTag)
        return;
    const std::size_t body = size - kAuthTag;
    std::uint8_t tag[kAuthTag];
    srtp_tag(rtc->peer_srtcp, data, body, nullptr, 0, tag);
    if (std::memcmp(tag, data + body, kAuthTag) != 0)
        return;
    const std::size_t end = body - kSrtcpTrailer;
    const std::uint32_t trailer = (std::uint32_t(data[end]) << 24) |
                                  (std::uint32_t(data[end + 1]) << 16) |
                                  (std::uint32_t(data[end + 2]) << 8) | data[end + 3];
    const std::uint32_t index = trailer & 0x7FFFFFFFU;
    // A replayed report must not trigger another IDR.
    if (rtc->peer_srtcp_index != 0 && static_cast<std::int32_t>(index - rtc->peer_srtcp_index) <= 0)
        return;
    rtc->peer_srtcp_index = index;
    if ((trailer & 0x80000000U) != 0U) {
        const std::uint32_t ssrc = (std::uint32_t(data[4]) << 24) | (std::uint32_t(data[5]) << 16) |
                                   (std::uint32_t(data[6]) << 8) | data[7];
        std::uint8_t iv[16];
        std::uint32_t counter{};
        srtp_iv(rtc->peer_srtcp, ssrc, index, iv, counter);
        br_aes_big_ctr_run(&rtc->peer_srtcp.aes, iv, counter, data + 8, end - 8);
    }
    for (std::size_t offset = 0; offset + 4 <= end;) {
        const std::size_t length = (std::size_t(get16(data + offset + 2)) + 1U) * 4U;
        if (offset + length > end)
            break;
        const std::uint8_t type = data[offset + 1];
        const std::uint8_t format = data[offset] & 0x1FU;
        if (type == 206 && (format == 1 || format == 4)) // PLI or FIR
            h264_request_idr();
        offset += length;
    }
}

/** @brief Sends a sender report for a track every few seconds (RFC 3550 §6.4.1, SRTCP). */
void send_report(Track& track) {
    const std::uint32_t now = static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (static_cast<std::int32_t>(now - track.next_report) < 0)
        return;
    track.next_report = now + kReportMilliseconds;
    timeval time{};
    (void)gettimeofday(&time, nullptr);
    const std::uint32_t seconds = static_cast<std::uint32_t>(time.tv_sec) + 2208988800U;
    const auto fraction =
        static_cast<std::uint32_t>((std::uint64_t(time.tv_usec) << 32) / 1000000U);
    std::uint8_t packet[28 + kSrtcpTrailer + kAuthTag];
    packet[0] = 0x80;
    packet[1] = 200;
    put16(packet + 2, 6);
    for (int i = 0; i < 4; ++i) {
        packet[4 + i] = static_cast<std::uint8_t>(track.ssrc >> (24 - 8 * i));
        packet[8 + i] = static_cast<std::uint8_t>(seconds >> (24 - 8 * i));
        packet[12 + i] = static_cast<std::uint8_t>(fraction >> (24 - 8 * i));
        packet[16 + i] = static_cast<std::uint8_t>(track.timestamp >> (24 - 8 * i));
        packet[20 + i] = static_cast<std::uint8_t>(track.packets >> (24 - 8 * i));
        packet[24 + i] = static_cast<std::uint8_t>(track.octets >> (24 - 8 * i));
    }
    track.report_index = (track.report_index + 1U) & 0x7FFFFFFFU;
    std::uint8_t iv[16];
    std::uint32_t counter{};
    srtp_iv(track.srtcp, track.ssrc, track.report_index, iv, counter);
    br_aes_big_ctr_run(&track.srtcp.aes, iv, counter, packet + 8, 20);
    const std::uint32_t trailer = 0x80000000U | track.report_index;
    for (int i = 0; i < 4; ++i)
        packet[28 + i] = static_cast<std::uint8_t>(trailer >> (24 - 8 * i));
    srtp_tag(track.srtcp, packet, 28 + kSrtcpTrailer, nullptr, 0, packet + 28 + kSrtcpTrailer);
    send_raw(packet, sizeof(packet));
}

/** @brief Dispatches one datagram: STUN, RTCP feedback, ignored RTP, or DTLS records. */
void handle_datagram(const std::uint8_t* data, const std::size_t size, const sockaddr_in& from) {
    if (size >= 20 && data[0] < 2) {
        handle_stun(data, size, from);
        return;
    }
    if (data[0] >= 128 && data[0] < 192) {
        // RTCP shares the socket (rtcp-mux); its packet types are 192 to 223.
        if (size >= 2 && data[1] >= 192 && data[1] <= 223)
            handle_rtcp(const_cast<std::uint8_t*>(data), size);
        return;
    }
    if (data[0] < 20 || data[0] > 63)
        return;
    rtc->peer = from;
    for (std::size_t offset = 0; offset + kRecordHeader <= size;) {
        const std::uint8_t type = data[offset];
        const std::uint16_t epoch = get16(data + offset + 3);
        const std::uint16_t length = get16(data + offset + 11);
        if (length == 0 || length > 2048 || offset + kRecordHeader + length > size)
            break;
        const std::uint8_t* record = data + offset;
        if (type == kHandshake) {
            if (rtc->hs == Handshake::done) // a lost final flight: repeat it
                send_raw(rtc->last_flight, rtc->last_flight_size);
            else
                handle_handshake_record(record, kRecordHeader + length);
        } else if (type == kChangeCipherSpec) {
            if (epoch == 0 && rtc->hs == Handshake::wait_client)
                rtc->epoch_in = 1;
        } else if (type == kAlert && length >= 2) {
            AddLog(LogLevel::error, "WebRTC: DTLS alert %u/%u", unsigned(record[kRecordHeader]),
                unsigned(record[kRecordHeader + 1]));
        }
        offset += kRecordHeader + length;
    }
}

void handshake_loop(void*) {
    auto buffer = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[1500]);
    while (task_running.load() && buffer) {
        sockaddr_in from{};
        socklen_t from_size = sizeof(from);
        const auto received = lwip_recvfrom(
            udp, buffer.get(), 1500, 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (received > 0 && rtc && rtc->state >= State::answered)
            handle_datagram(buffer.get(), static_cast<std::size_t>(received), from);
    }
    handshake_task = nullptr;
    vTaskDelete(nullptr);
}

/** @brief Encrypts and sends one RTP packet of @p track (AES-CM and HMAC-SHA1-80). */
void send_srtp(
    Track& track, const std::uint8_t* payload, const std::size_t size, const bool marker) {
    if (!rtc || !track.active || rtc->state != State::streaming || size > kMaxPayload)
        return;
    std::uint8_t packet[12 + kMaxPayload + kAuthTag];
    packet[0] = 0x80;
    packet[1] = static_cast<std::uint8_t>(track.payload_type | (marker ? 0x80U : 0U));
    put16(packet + 2, track.sequence);
    for (int i = 0; i < 4; ++i) {
        packet[4 + i] = static_cast<std::uint8_t>(track.timestamp >> (24 - 8 * i));
        packet[8 + i] = static_cast<std::uint8_t>(track.ssrc >> (24 - 8 * i));
    }
    const std::uint64_t index = (std::uint64_t(track.roc) << 16) | track.sequence;
    std::uint8_t iv[16];
    std::uint32_t counter{};
    srtp_iv(track.srtp, track.ssrc, index, iv, counter);
    std::memcpy(packet + 12, payload, size);
    br_aes_big_ctr_run(&track.srtp.aes, iv, counter, packet + 12, size);
    std::uint8_t roc[4];
    for (int i = 0; i < 4; ++i)
        roc[i] = static_cast<std::uint8_t>(track.roc >> (24 - 8 * i));
    srtp_tag(track.srtp, packet, 12 + size, roc, 4, packet + 12 + size);
    send_raw(packet, 12 + size + kAuthTag);
    ++track.packets;
    track.octets += static_cast<std::uint32_t>(size);
    if (++track.sequence == 0)
        ++track.roc;
}

/** @brief Sends one NAL unit whole or as FU-A fragments. */
void send_nal(const std::uint8_t* nal, const std::size_t size) {
    const auto type = static_cast<std::uint8_t>(nal[0] & 0x1FU);
    const bool picture = type == 1U || type == 5U;
    if (size <= kMaxPayload) {
        send_srtp(rtc->video, nal, size, picture);
        return;
    }
    const std::uint8_t indicator = static_cast<std::uint8_t>((nal[0] & 0xE0U) | 28U);
    std::uint8_t packet[kMaxPayload];
    for (std::size_t offset = 1; offset < size;) {
        const std::size_t chunk = std::min(size - offset, kMaxPayload - 2U);
        const bool last = offset + chunk >= size;
        packet[0] = indicator;
        packet[1] =
            static_cast<std::uint8_t>(type | (offset == 1 ? 0x80U : 0U) | (last ? 0x40U : 0U));
        std::memcpy(packet + 2, nal + offset, chunk);
        send_srtp(rtc->video, packet, chunk + 2, last && picture);
        offset += chunk;
    }
}

void base64(
    const std::uint8_t* data, const std::size_t size, char* out, const std::size_t capacity) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t o{};
    for (std::size_t i = 0; i < size && o + 4 < capacity; i += 3) {
        std::uint32_t n = std::uint32_t(data[i]) << 16;
        if (i + 1 < size)
            n |= std::uint32_t(data[i + 1]) << 8;
        if (i + 2 < size)
            n |= data[i + 2];
        out[o++] = kTable[(n >> 18) & 63U];
        out[o++] = kTable[(n >> 12) & 63U];
        out[o++] = i + 1 < size ? kTable[(n >> 6) & 63U] : '=';
        out[o++] = i + 2 < size ? kTable[n & 63U] : '=';
    }
    out[o] = '\0';
}

/** @brief Fresh per-session state: ICE credentials, fingerprint, tracks and handshake. */
void reset_session() {
    auto& s = *rtc;
    const auto keep_peer = s.peer;
    std::memset(&s, 0, sizeof(s));
    s.peer = keep_peer;
    std::snprintf(s.ice_ufrag, sizeof(s.ice_ufrag), "%08X", unsigned(esp_random()));
    std::snprintf(s.ice_pwd, sizeof(s.ice_pwd), "%08X%08X%08X%08X", unsigned(esp_random()),
        unsigned(esp_random()), unsigned(esp_random()), unsigned(esp_random()));
    br_sha256_context hash;
    br_sha256_init(&hash);
    br_sha256_update(&hash, kCertificateDer, sizeof(kCertificateDer));
    br_sha256_out(&hash, s.fingerprint);
    char* text = s.fingerprint_text;
    for (int i = 0; i < 32; ++i)
        text += std::snprintf(text, 4, i < 31 ? "%02X:" : "%02X", s.fingerprint[i]);
    std::memcpy(s.certificate_key_bytes, kCertificateKey, sizeof(kCertificateKey));
    s.certificate_key.curve = BR_EC_secp256r1;
    s.certificate_key.x = s.certificate_key_bytes;
    s.certificate_key.xlen = sizeof(kCertificateKey);
    esp_fill_random(s.cookie_secret, sizeof(s.cookie_secret));
    s.video.payload_type = 96;
    s.video.ssrc = esp_random();
    s.video.sequence = static_cast<std::uint16_t>(esp_random());
    s.video.timestamp = esp_random();
    s.audio.payload_type = webrtc_audio_codec().payload_type;
    s.audio.ssrc = esp_random();
    s.audio.sequence = static_cast<std::uint16_t>(esp_random());
    s.audio.timestamp = esp_random();
    s.hs = Handshake::wait_hello;
    s.state = State::idle;
}

/** @brief Reads the remote H.264 payload type; Chrome assigns it dynamically. */
void parse_offer(const char* sdp) {
    rtc->video.payload_type = 96;
    for (const char* line = sdp; (line = std::strstr(line, "a=rtpmap:")) != nullptr;) {
        line += 9;
        const int type = std::atoi(line);
        const char* end = std::strpbrk(line, "\r\n");
        const char* codec = std::strstr(line, "H264/90000");
        if (type <= 0 || !codec || (end && codec > end))
            continue;
        char fmtp[24];
        std::snprintf(fmtp, sizeof(fmtp), "a=fmtp:%d ", type);
        const char* parameters = std::strstr(sdp, fmtp);
        const char* parameters_end = parameters ? std::strpbrk(parameters, "\r\n") : nullptr;
        const char* mode = parameters ? std::strstr(parameters, "packetization-mode=1") : nullptr;
        if (mode && (!parameters_end || mode < parameters_end)) {
            rtc->video.payload_type = static_cast<std::uint8_t>(type);
            break;
        }
        if (rtc->video.payload_type == 96)
            rtc->video.payload_type = static_cast<std::uint8_t>(type);
    }
}
} // namespace

bool webrtc_start() {
    if (task_running.load())
        return true;
    if (!rtc)
        rtc = static_cast<Session*>(heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_SPIRAM));
    udp_lock = udp_lock ? udp_lock : xSemaphoreCreateMutex();
    udp = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!rtc || !udp_lock || udp < 0) {
        webrtc_stop();
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    timeval timeout{0, 200000};
    (void)lwip_setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (lwip_bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        webrtc_stop();
        return false;
    }
    reset_session();
    task_running.store(true);
    if (xTaskCreatePinnedToCore(
            handshake_loop, "camera-dtls", 12288, nullptr, 5, &handshake_task, 0) != pdPASS) {
        handshake_task = nullptr;
        webrtc_stop();
        return false;
    }
    AddLog(LogLevel::info, "WebRTC: DTLS on UDP %u, fingerprint %s", unsigned(kPort),
        rtc->fingerprint_text);
    return true;
}

void webrtc_stop() {
    task_running.store(false);
    for (int i = 0; i < 20 && handshake_task; ++i)
        vTaskDelay(pdMS_TO_TICKS(25));
    if (handshake_task) {
        vTaskDelete(handshake_task);
        handshake_task = nullptr;
    }
    if (udp_lock)
        xSemaphoreTake(udp_lock, portMAX_DELAY);
    if (udp >= 0)
        lwip_close(udp);
    udp = -1;
    if (rtc) {
        std::memset(rtc, 0, sizeof(*rtc)); // drop the key material
        heap_caps_free(rtc);
        rtc = nullptr;
    }
    if (udp_lock)
        xSemaphoreGive(udp_lock);
}

void webrtc_send_video(const Encoded& frame) {
    if (!rtc || rtc->state != State::streaming)
        return;
    // A fresh peer can decode from an IDR only.
    if (rtc->waiting_for_idr && !frame.key)
        return;
    rtc->waiting_for_idr = false;
    const auto fps = detail::mode().fps ? detail::mode().fps : 30U;
    rtc->video.timestamp += 90000U / fps;
    const std::uint8_t* sps{};
    const std::uint8_t* pps{};
    std::size_t sps_size{}, pps_size{};
    const bool sets = h264_parameter_sets(sps, sps_size, pps, pps_size);
    for_each_nal(frame.data, frame.size, [&](const std::uint8_t* nal, const std::size_t size) {
        if ((nal[0] & 0x1FU) == 5U && sets) { // SPS/PPS before every IDR for Chrome
            send_srtp(rtc->video, sps, sps_size, false);
            send_srtp(rtc->video, pps, pps_size, false);
        }
        send_nal(nal, size);
    });
    send_report(rtc->video);
}
} // namespace detail

namespace {
/** @brief This device's SDP: the answer with setup:passive, or an offer with setup:actpass. */
std::size_t describe(
    const char* setup, const std::uint32_t local_ip, char* out, const std::size_t capacity) {
    using namespace detail;
    in_addr address{};
    address.s_addr = local_ip;
    if (local_ip == 0U && netif_default != nullptr)
        address.s_addr = ip4_addr_get_u32(netif_ip4_addr(netif_default));
    char ip[16];
    (void)inet_ntoa_r(address, ip, sizeof(ip));
    const std::uint8_t* sps{};
    const std::uint8_t* pps{};
    std::size_t sps_size{}, pps_size{};
    char sprop[800]{}, profile[7] = "42001E"; // fits both base64 sets and the key
    if (h264_parameter_sets(sps, sps_size, pps, pps_size)) {
        char sps_text[512], pps_text[256];
        base64(sps, sps_size, sps_text, sizeof(sps_text));
        base64(pps, pps_size, pps_text, sizeof(pps_text));
        std::snprintf(sprop, sizeof(sprop), ";sprop-parameter-sets=%s,%s", sps_text, pps_text);
        if (sps_size >= 4) // profile-level-id from the SPS itself
            std::snprintf(profile, sizeof(profile), "%02X%02X%02X", sps[1], sps[2], sps[3]);
    }
    const auto codec = webrtc_audio_codec();
    const std::uint8_t video = rtc->video.payload_type;
    const auto written = std::snprintf(out, capacity,
        "v=0\r\no=- %u 2 IN IP4 %s\r\ns=-\r\nt=0 0\r\na=ice-lite\r\n"
        "a=fingerprint:sha-256 %s\r\na=group:BUNDLE 0 1\r\na=msid-semantic: WMS newmota\r\n"
        "m=video %u UDP/TLS/RTP/SAVPF %u\r\nc=IN IP4 %s\r\na=rtcp-mux\r\n"
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=setup:%s\r\n"
        "a=mid:0\r\na=sendonly\r\na=rtpmap:%u H264/90000\r\n"
        "a=fmtp:%u packetization-mode=1;profile-level-id=%s%s\r\n"
        "a=rtcp-fb:%u nack pli\r\na=rtcp-fb:%u ccm fir\r\n"
        "a=msid:newmota video\r\na=candidate:1 1 udp 2130706431 %s %u typ host\r\n"
        "a=end-of-candidates\r\na=ssrc:%u cname:newmota-video\r\n"
        "a=ssrc:%u msid:newmota video\r\n"
        "m=audio %u UDP/TLS/RTP/SAVPF %u\r\nc=IN IP4 %s\r\na=rtcp-mux\r\n"
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=setup:%s\r\n"
        "a=mid:1\r\na=sendonly\r\n%s"
        "a=msid:newmota audio\r\na=candidate:1 1 udp 2130706431 %s %u typ host\r\n"
        "a=end-of-candidates\r\na=ssrc:%u cname:newmota-audio\r\n"
        "a=ssrc:%u msid:newmota audio\r\n",
        unsigned(esp_random()), ip, rtc->fingerprint_text, unsigned(kPort), unsigned(video), ip,
        rtc->ice_ufrag, rtc->ice_pwd, setup, unsigned(video), unsigned(video), profile, sprop,
        unsigned(video), unsigned(video), ip, unsigned(kPort), unsigned(rtc->video.ssrc),
        unsigned(rtc->video.ssrc), unsigned(kPort), unsigned(codec.payload_type), ip,
        rtc->ice_ufrag, rtc->ice_pwd, setup,
        codec.payload_type == 111U
            ? "a=rtpmap:111 opus/48000/2\r\n"
              "a=fmtp:111 minptime=20;useinbandfec=0;stereo=0;sprop-stereo=0\r\n"
            : "a=rtpmap:8 PCMA/8000\r\n",
        ip, unsigned(kPort), unsigned(rtc->audio.ssrc), unsigned(rtc->audio.ssrc));
    if (written <= 0 || static_cast<std::size_t>(written) >= capacity)
        return 0;
    return static_cast<std::size_t>(written);
}
} // namespace

std::size_t webrtc_answer(const char* offer, const std::size_t offer_size,
    const std::uint32_t local_ip, char* answer, const std::size_t capacity) {
    using namespace detail;
    if (!rtc || !offer || offer_size == 0)
        return 0;
    auto text = std::unique_ptr<char[]>(new (std::nothrow) char[offer_size + 1]);
    if (!text)
        return 0;
    std::memcpy(text.get(), offer, offer_size);
    text[offer_size] = '\0';
    if (!std::strstr(text.get(), "v=0"))
        return 0;
    reset_session();
    parse_offer(text.get());
    const std::size_t size = describe("passive", local_ip, answer, capacity);
    if (size != 0U)
        rtc->state = State::answered;
    return size;
}

std::size_t webrtc_offer(const std::uint32_t local_ip, char* offer, const std::size_t capacity) {
    using namespace detail;
    if (!rtc)
        return 0;
    reset_session();
    const std::size_t size = describe("actpass", local_ip, offer, capacity);
    if (size != 0U)
        rtc->state = State::offer;
    return size;
}

bool webrtc_accept_answer(const char* answer, const std::size_t answer_size) {
    using namespace detail;
    if (!rtc || rtc->state != State::offer || !answer || answer_size == 0)
        return false;
    auto text = std::unique_ptr<char[]>(new (std::nothrow) char[answer_size + 1]);
    if (!text)
        return false;
    std::memcpy(text.get(), answer, answer_size);
    text[answer_size] = '\0';
    // This side only runs the DTLS server, so the viewer must take the client role.
    const char* setup = std::strstr(text.get(), "a=setup:");
    if (!std::strstr(text.get(), "v=0") || (setup && std::strncmp(setup + 8, "active", 6) != 0))
        return false;
    parse_offer(text.get());
    rtc->state = State::answered;
    return true;
}

bool webrtc_streaming() {
    return detail::rtc && detail::rtc->state == detail::State::streaming;
}

AudioCodec webrtc_audio_codec() {
#if USE_AUDIO_OPUS
    return {111, 960}; // Opus: 48 kHz RTP clock
#else
    return {8, 160}; // G.711 A-law at 8 kHz
#endif
}

bool webrtc_send_audio(const std::uint8_t* payload, const std::size_t size) {
    using namespace detail;
    if (!webrtc_streaming())
        return false;
    send_srtp(rtc->audio, payload, size, false);
    rtc->audio.timestamp += webrtc_audio_codec().samples_per_packet;
    send_report(rtc->audio);
    return true;
}

} // namespace newmota::platform::camera_csi
