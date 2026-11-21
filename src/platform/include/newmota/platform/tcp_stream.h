/*
  tcp_stream.h - portable asynchronous plain-TCP byte stream

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace newmota::platform {

/** @brief Reports the lifecycle of one native TCP byte stream. */
enum class TcpStreamState : std::uint8_t {
    unavailable,
    idle,
    connecting,
    connected,
    closing,
};

/** @brief Reports whether an asynchronous stream operation made progress. */
enum class TcpStreamIoState : std::uint8_t {
    progressed,
    would_block,
    closed,
    failed,
    unavailable,
};

/** @brief Reports bounded read/write progress without blocking the caller. */
struct TcpStreamIoResult final {
    TcpStreamIoState state{TcpStreamIoState::unavailable};
    std::size_t size{};
};

/** @brief Reports the result of opening one asynchronous TCP stream. */
enum class TcpStreamOpenResult : std::uint8_t {
    accepted,
    busy,
    unavailable,
    invalid,
    failed,
};

/**
 * @brief Supplies bounded nonblocking native TCP operations.
 *
 * The adapter owns DNS, TCP state, one connection attempt, and SDK objects.
 * It copies the host before `open` returns and does not retain caller buffers
 * after `read` or `write` returns. Calls are serialized by the application
 * loop; native callbacks hand off bytes through bounded storage.
 */
struct TcpStream final {
    using State = TcpStreamState (*)(void* context);
    using Open = TcpStreamOpenResult (*)(
        void* context, std::string_view host, std::uint16_t port, bool secure);
    using Read = TcpStreamIoResult (*)(void* context, std::uint8_t* output, std::size_t capacity);
    using Write = TcpStreamIoResult (*)(void* context, const std::uint8_t* input, std::size_t size);
    using Close = void (*)(void* context);

    void* context{};
    State state{};
    Open open{};
    Read read{};
    Write write{};
    Close close{};
};

/** @brief Returns the single native TCP stream shared by MQTT and OTA pull. */
[[nodiscard]] TcpStream tcp_stream();

} // namespace newmota::platform
