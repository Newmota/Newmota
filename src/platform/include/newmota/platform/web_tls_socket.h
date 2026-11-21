/*
  web_tls_socket.h - bounded Mbed TLS socket transport for native web adapters

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "newmota/platform/web.h"

#include <cstddef>

namespace newmota::platform::web_tls_socket {

/** @brief Prepares the single-client TLS server around a persistent identity. */
[[nodiscard]] bool start_server(const WebTlsIdentity& identity);
/** @brief Runs a TLS handshake on the accepted socket. */
[[nodiscard]] bool accept_client(int socket);
constexpr int retry_read = -2;
constexpr int retry_write = -3;
/** @brief Waits briefly for the socket readiness requested by a retry result. */
void wait_retry(int result);
/** @brief Reads decrypted bytes from the active TLS socket. */
[[nodiscard]] int read_tls(void* destination, std::size_t size);
/** @brief Writes encrypted bytes to the active TLS socket. */
[[nodiscard]] int write_tls(const void* source, std::size_t size);
/** @brief Releases the active session before its socket is closed. */
void close_tls();

} // namespace newmota::platform::web_tls_socket
