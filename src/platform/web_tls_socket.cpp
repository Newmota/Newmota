/*
  web_tls_socket.cpp - single-client Mbed TLS server for Beken and Bouffalo

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota_features.h"
#include "newmota/platform/web_tls_socket.h"

#if USE_WEB_HTTPS

extern "C" {
#include "lwip/errno.h"
#include "lwip/sockets.h"
}

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"

#include "newmota/platform/network.h"

#include <array>
#include <cstdint>

namespace newmota::platform::web_tls_socket {
namespace {

mbedtls_ctr_drbg_context random_context;
mbedtls_ssl_config server_config;
mbedtls_x509_crt server_certificate;
mbedtls_pk_context server_key;
mbedtls_ssl_context client_session;
bool server_ready{};
bool client_ready{};
int active_socket{-1};

/** @brief Supplies Mbed TLS with bytes from the platform's native RNG. */
int entropy(void*, unsigned char* destination, const std::size_t size) {
    return platform::web_random(destination, size) ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

/** @brief Sends one TLS record to the active blocking TCP socket. */
int socket_send(void* context, const unsigned char* buffer, const std::size_t size) {
    const auto socket = *static_cast<const int*>(context);
    auto result = ::send(socket, buffer, size, 0);
    if (result < 0 && errno == EINTR)
        result = ::send(socket, buffer, size, 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(result);
}

/** @brief Receives one TLS record from the active blocking TCP socket. */
int socket_receive(void* context, unsigned char* buffer, const std::size_t size) {
    const auto socket = *static_cast<const int*>(context);
    auto result = ::recv(socket, buffer, size, 0);
    if (result < 0 && errno == EINTR)
        result = ::recv(socket, buffer, size, 0);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return MBEDTLS_ERR_SSL_WANT_READ;
    return result < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : static_cast<int>(result);
}

/** @brief Releases any partially initialized server contexts after an error. */
void clear_server() {
    mbedtls_ssl_config_free(&server_config);
    mbedtls_pk_free(&server_key);
    mbedtls_x509_crt_free(&server_certificate);
    mbedtls_ctr_drbg_free(&random_context);
    server_ready = false;
}

} // namespace

/** @brief Parses and validates the persistent identity, then prepares TLS defaults. */
bool start_server(const WebTlsIdentity& identity) {
    if (server_ready)
        return true;
    if (identity.certificate_size == 0U || identity.private_key_size == 0U ||
        identity.certificate_size >= identity.certificate_pem.size() ||
        identity.private_key_size >= identity.private_key_pem.size())
        return false;

    mbedtls_ctr_drbg_init(&random_context);
    mbedtls_ssl_config_init(&server_config);
    mbedtls_x509_crt_init(&server_certificate);
    mbedtls_pk_init(&server_key);
    static constexpr unsigned char personalization[] = "Newmota web TLS server";
    bool success = mbedtls_ctr_drbg_seed(&random_context, entropy, nullptr, personalization,
                       sizeof(personalization) - 1U) == 0 &&
                   mbedtls_x509_crt_parse(&server_certificate,
                       reinterpret_cast<const unsigned char*>(identity.certificate_pem.data()),
                       identity.certificate_size + 1U) == 0;
#if MBEDTLS_VERSION_MAJOR >= 4
    if (success) {
        success = mbedtls_pk_parse_key(&server_key,
                      reinterpret_cast<const unsigned char*>(identity.private_key_pem.data()),
                      identity.private_key_size + 1U, nullptr, 0U) == 0;
    }
#elif MBEDTLS_VERSION_MAJOR >= 3
    if (success) {
        success = mbedtls_pk_parse_key(&server_key,
                      reinterpret_cast<const unsigned char*>(identity.private_key_pem.data()),
                      identity.private_key_size + 1U, nullptr, 0U, mbedtls_ctr_drbg_random,
                      &random_context) == 0;
    }
#else
    if (success) {
        success = mbedtls_pk_parse_key(&server_key,
                      reinterpret_cast<const unsigned char*>(identity.private_key_pem.data()),
                      identity.private_key_size + 1U, nullptr, 0U) == 0;
    }
#endif
    success = success && mbedtls_ssl_config_defaults(&server_config, MBEDTLS_SSL_IS_SERVER,
                             MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (success) {
        mbedtls_ssl_conf_authmode(&server_config, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&server_config, mbedtls_ctr_drbg_random, &random_context);
        mbedtls_ssl_conf_min_version(
            &server_config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
        success = mbedtls_ssl_conf_own_cert(&server_config, &server_certificate, &server_key) == 0;
    }
    if (!success) {
        clear_server();
        return false;
    }
    server_ready = true;
    return true;
}

/** @brief Sets up one client session and completes its bounded handshake. */
bool accept_client(const int socket) {
    if (!server_ready || client_ready || socket < 0)
        return false;
    active_socket = socket;
    mbedtls_ssl_init(&client_session);
    client_ready = true;
    if (mbedtls_ssl_setup(&client_session, &server_config) != 0) {
        mbedtls_ssl_free(&client_session);
        client_ready = false;
        active_socket = -1;
        return false;
    }
    mbedtls_ssl_set_bio(&client_session, &active_socket, socket_send, socket_receive, nullptr);
    // lwIP sockets use a bounded receive timeout. Resume a pending blocking
    // handshake after a timeout, but do not let a stalled client monopolize
    // the one-client web task indefinitely.
    for (unsigned int attempt = 0U; attempt < 6U; ++attempt) {
        const auto result = mbedtls_ssl_handshake(&client_session);
        if (result == 0)
            return true;
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
    }
    mbedtls_ssl_free(&client_session);
    client_ready = false;
    active_socket = -1;
    return false;
}

/** @brief Reads decrypted application bytes from the single active session. */
int read_tls(void* destination, const std::size_t size) {
    if (!client_ready || destination == nullptr || size == 0U)
        return -1;
    const auto result =
        mbedtls_ssl_read(&client_session, static_cast<unsigned char*>(destination), size);
    return result == MBEDTLS_ERR_SSL_WANT_READ    ? retry_read
           : result == MBEDTLS_ERR_SSL_WANT_WRITE ? retry_write
                                                  : result;
}

/** @brief Writes at most one bounded TLS record of application data. */
int write_tls(const void* source, std::size_t size) {
    if (!client_ready || source == nullptr)
        return -1;
    constexpr std::size_t kWriteChunk = 2048U;
    if (size > kWriteChunk)
        size = kWriteChunk;
    const auto result =
        mbedtls_ssl_write(&client_session, static_cast<const unsigned char*>(source), size);
    return result == MBEDTLS_ERR_SSL_WANT_READ    ? retry_read
           : result == MBEDTLS_ERR_SSL_WANT_WRITE ? retry_write
                                                  : result;
}

/** @brief Suspends the web worker until readiness or a bounded timer tick. */
void wait_retry(const int result) {
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(active_socket, &sockets);
    timeval timeout{0, 100000};
    (void)select(active_socket + 1, result == retry_read ? &sockets : nullptr,
        result == retry_write ? &sockets : nullptr, nullptr, &timeout);
}

/** @brief Sends close-notify when possible and releases the active session. */
void close_tls() {
    if (client_ready) {
        (void)mbedtls_ssl_close_notify(&client_session);
        mbedtls_ssl_free(&client_session);
        client_ready = false;
    }
    active_socket = -1;
}

} // namespace newmota::platform::web_tls_socket

#endif // USE_WEB_HTTPS
