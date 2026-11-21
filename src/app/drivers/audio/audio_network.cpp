/*
  audio_network.cpp - Native audio HTTP source and microphone listener

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file audio_network.cpp
 * @brief Streams compressed audio through independent TCP owners and bounded framing.
 */
#include "drivers/audio/audio_network.h"
#include "drivers/audio/audio_codecs.h"
#include "newmota/platform/audio_network.h"
#include "newmota/platform/platform.h"
#include "support/number.h"
#include "support/text.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace newmota::audio {
namespace {
using platform::TcpStream;
using platform::TcpStreamIoState;
using platform::TcpStreamState;
constexpr std::uint32_t kNetworkTimeoutMilliseconds = 15000;
constexpr std::size_t kMaximumHeaderBytes = 2048;
constexpr std::size_t kRequestBufferBytes = 640;
constexpr std::size_t kNetworkReadBufferBytes = 1024;
constexpr std::size_t kChunkLineBytes = 96;
constexpr std::size_t kHostBufferBytes = 128;
constexpr std::size_t kServerResponseBytes = 256;
constexpr std::size_t kChunkHeaderBytes = 12;
constexpr std::size_t kListenerReadBufferBytes = 256;
constexpr std::size_t kChunkPayloadBytes = 1024;
constexpr std::uint16_t kHttpDefaultPort = 80;
constexpr std::uint32_t kIcyMetadataLengthUnitBytes = 16;
/** Leading metadata bytes kept for parsing; stations put StreamTitle first. */
constexpr std::size_t kIcyMetadataCaptureBytes = 256;
constexpr std::size_t kIcyTextBytes = 128;
constexpr std::string_view kIcyTitleStart = "StreamTitle='";
constexpr std::string_view kIcyTitleEnd = "';";
constexpr std::size_t kHttpVersionOffset = 9;
constexpr std::size_t kIcyStatusPrefixBytes = 4;
constexpr std::size_t kHttpStatusCodeBytes = 3;
constexpr std::uint32_t kAsciiDelete = 0x7f;
constexpr std::string_view kHttpCrlf = "\r\n";
constexpr std::string_view kHttpHeaderTerminator = "\r\n\r\n";
constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kHttpsScheme = "https://";
constexpr std::string_view kHttpOkStatus = "200";

struct Radio {
    enum class Phase : std::uint8_t { closed, connecting, request, headers, body, ended, failed };
    enum class Chunk : std::uint8_t { size, bytes, cr, lf, trailers, ended };
    TcpStream stream{};
    Phase phase{Phase::closed};
    Chunk chunk{Chunk::size};
    std::array<char, kRequestBufferBytes> request{};
    std::array<char, kMaximumHeaderBytes> headers{};
    std::array<std::uint8_t, kNetworkReadBufferBytes> raw{};
    std::array<char, kChunkLineBytes> chunk_line{};
    std::size_t request_size{}, sent{}, header_size{}, raw_size{}, raw_offset{}, line_size{};
    std::uint32_t progress{}, content_remaining{}, chunk_remaining{}, meta_interval{}, meta_audio{},
        meta_remaining{};
    std::size_t trailers{}, metadata_size{};
    bool chunked{}, content_length{}, meta_length{};
    std::array<char, kIcyMetadataCaptureBytes> metadata{};
    /** NUL-terminated `icy-name` header and latest StreamTitle. */
    std::array<char, kIcyTextBytes> station{}, title{};
};

Radio radio;

/** @brief Parses only plaintext URLs and rejects control characters before constructing HTTP. */
bool url(std::string_view value, std::array<char, kHostBufferBytes>& host, std::uint16_t& port,
    std::string_view& authority, std::string_view& path) {
    if (value.substr(0, kHttpScheme.size()) != kHttpScheme)
        return false;
    for (const auto c : value)
        if (static_cast<unsigned char>(c) <= ' ' || c == kAsciiDelete)
            return false;
    value.remove_prefix(kHttpScheme.size());
    const auto target = value.find_first_of("/?");
    authority = value.substr(0, target);
    path = target == std::string_view::npos ? std::string_view{"/"} : value.substr(target);
    if (authority.empty() || authority.find('@') != std::string_view::npos ||
        authority.find('[') != std::string_view::npos || value.find('#') != std::string_view::npos)
        return false;
    auto name = authority;
    port = kHttpDefaultPort;
    const auto colon = name.find(':');
    if (colon != std::string_view::npos) {
        std::uint32_t parsed{};
        if (!support::parse_unsigned_decimal(
                name.substr(colon + 1), std::numeric_limits<std::uint16_t>::max(), parsed) ||
            !parsed)
            return false;
        port = static_cast<std::uint16_t>(parsed);
        name = name.substr(0, colon);
    }
    if (name.empty() || name.size() >= host.size())
        return false;
    std::copy(name.begin(), name.end(), host.begin());
    return true;
}

/** @brief Copies text up to a NUL without splitting a UTF-8 sequence at the cut. */
void copy_text(std::array<char, kIcyTextBytes>& target, std::string_view text) {
    text = text.substr(0, text.find('\0'));
    auto size = std::min(text.size(), target.size() - 1);
    if (size < text.size())
        while (size && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U)
            --size;
    std::copy_n(text.data(), size, target.data());
    target[size] = '\0';
}

/** @brief Takes StreamTitle from a complete metadata block; a block without one keeps it. */
void icy_metadata(Radio& r) {
    const std::string_view text{r.metadata.data(), r.metadata_size};
    const auto start = text.find(kIcyTitleStart);
    if (start == std::string_view::npos)
        return;
    const auto value = text.substr(start + kIcyTitleStart.size());
    copy_text(r.title, value.substr(0, value.find(kIcyTitleEnd)));
}

bool decimal(std::string_view value, std::uint32_t& number) {
    return support::parse_unsigned_decimal(
        support::trim_ascii(value), std::numeric_limits<std::uint32_t>::max(), number);
}

/** @brief Accepts HTTP/ICY status and explicitly understood body framing. */
bool response_headers(Radio& r) {
    const std::string_view headers{r.headers.data(), r.header_size};
    const auto first = headers.find("\r\n");
    if (first == std::string_view::npos)
        return false;
    const auto status = headers.substr(0, first);
    const auto code_offset = status.substr(0, kHttpVersionOffset) == "HTTP/1.0 " ||
                                     status.substr(0, kHttpVersionOffset) == "HTTP/1.1 "
                                 ? kHttpVersionOffset
                             : status.substr(0, kIcyStatusPrefixBytes) == "ICY "
                                 ? kIcyStatusPrefixBytes
                                 : 0U;
    if (!code_offset || status.substr(code_offset, kHttpStatusCodeBytes) != kHttpOkStatus ||
        (status.size() != code_offset + kHttpStatusCodeBytes &&
            status[code_offset + kHttpStatusCodeBytes] != ' '))
        return false;
    for (auto offset = first + kHttpCrlf.size(); offset + kHttpCrlf.size() <= headers.size();) {
        const auto end = headers.find(kHttpCrlf, offset);
        if (end == std::string_view::npos)
            return false;
        if (end == offset)
            break;
        const auto line = headers.substr(offset, end - offset);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
            return false;
        const auto name = line.substr(0, colon);
        const auto value = support::trim_ascii(line.substr(colon + 1));
        if (support::ascii_iequals(name, "Transfer-Encoding")) {
            if (r.chunked || !support::ascii_iequals(value, "chunked"))
                return false;
            r.chunked = true;
        } else if (support::ascii_iequals(name, "Content-Length")) {
            if (r.content_length || !decimal(value, r.content_remaining))
                return false;
            r.content_length = true;
        } else if (support::ascii_iequals(name, "Content-Encoding")) {
            if (!support::ascii_iequals(value, "identity"))
                return false;
        } else if (support::ascii_iequals(name, "icy-metaint")) {
            if (r.meta_interval || !decimal(value, r.meta_interval) || !r.meta_interval)
                return false;
        } else if (support::ascii_iequals(name, "icy-name")) {
            copy_text(r.station, value);
        }
        offset = end + kHttpCrlf.size();
    }
    if (r.chunked && r.content_length)
        return false;
    if (r.content_length && !r.content_remaining)
        r.phase = Radio::Phase::ended;
    return true;
}

/** @brief Decodes one transfer byte; returns true only for an audio/ICY payload byte. */
bool body_byte(Radio& r, std::uint8_t byte, bool& payload) {
    payload = false;
    if (!r.chunked) {
        if (r.content_length) {
            if (!r.content_remaining)
                return false;
            if (--r.content_remaining == 0)
                r.phase = Radio::Phase::ended;
        }
        payload = true;
        return true;
    }
    switch (r.chunk) {
    case Radio::Chunk::size:
        if (byte == '\n') {
            if (!r.line_size || r.chunk_line[r.line_size - 1] != '\r')
                return false;
            std::string_view line{r.chunk_line.data(), r.line_size - 1};
            line = line.substr(0, line.find(';'));
            if (line.empty())
                return false;
            std::uint32_t length{};
            for (const auto c : line) {
                const auto digit = c >= '0' && c <= '9'   ? c - '0'
                                   : c >= 'a' && c <= 'f' ? c - 'a' + 16
                                   : c >= 'A' && c <= 'F' ? c - 'A' + 16
                                                          : -1;
                if (digit < 0 || length > (std::numeric_limits<std::uint32_t>::max() -
                                              static_cast<unsigned>(digit)) /
                                              16)
                    return false;
                length = length * 16 + static_cast<unsigned>(digit);
            }
            r.line_size = 0;
            r.chunk_remaining = length;
            r.chunk = length ? Radio::Chunk::bytes : Radio::Chunk::trailers;
        } else {
            if (r.line_size == r.chunk_line.size())
                return false;
            r.chunk_line[r.line_size++] = static_cast<char>(byte);
        }
        return true;
    case Radio::Chunk::bytes:
        payload = true;
        if (--r.chunk_remaining == 0)
            r.chunk = Radio::Chunk::cr;
        return true;
    case Radio::Chunk::cr:
        if (byte != '\r')
            return false;
        r.chunk = Radio::Chunk::lf;
        return true;
    case Radio::Chunk::lf:
        if (byte != '\n')
            return false;
        r.chunk = Radio::Chunk::size;
        return true;
    case Radio::Chunk::trailers:
        if (++r.trailers > kMaximumHeaderBytes)
            return false;
        if (byte == '\n') {
            if (r.line_size == 1 && r.chunk_line[0] == '\r') {
                r.phase = Radio::Phase::ended;
                r.chunk = Radio::Chunk::ended;
            }
            r.line_size = 0;
        } else {
            if (r.line_size == r.chunk_line.size())
                return false;
            r.chunk_line[r.line_size++] = static_cast<char>(byte);
        }
        return true;
    case Radio::Chunk::ended:
        return false;
    }
    return false;
}

Io radio_read(void*, std::uint8_t* output, std::size_t capacity) {
    auto& r = radio;
    const auto now = platform::monotonic_milliseconds();
    if (r.phase == Radio::Phase::ended)
        return {Result::success, 0, true};
    if (r.phase == Radio::Phase::failed || r.phase == Radio::Phase::closed)
        return {Result::io_error};
    if (now - r.progress >= kNetworkTimeoutMilliseconds) {
        r.phase = Radio::Phase::failed;
        return {Result::io_error};
    }
    const auto state = r.stream.state(r.stream.context);
    if (r.phase == Radio::Phase::connecting) {
        if (state == TcpStreamState::connecting)
            return {};
        if (state != TcpStreamState::connected)
            return {Result::io_error};
        r.phase = Radio::Phase::request;
        r.progress = now;
    }
    if (r.phase == Radio::Phase::request) {
        const auto sent = r.stream.write(r.stream.context,
            reinterpret_cast<const std::uint8_t*>(r.request.data()) + r.sent,
            r.request_size - r.sent);
        if (sent.state == TcpStreamIoState::progressed) {
            r.sent += sent.size;
            r.progress = now;
        } else if (sent.state != TcpStreamIoState::would_block)
            return {Result::io_error};
        if (r.sent == r.request_size)
            r.phase = Radio::Phase::headers;
        return {};
    }
    if (r.raw_offset == r.raw_size) {
        const auto read = r.stream.read(r.stream.context, r.raw.data(), r.raw.size());
        if (read.state == TcpStreamIoState::closed) {
            if (r.phase == Radio::Phase::body && !r.chunked && !r.content_length)
                return {Result::success, 0, true};
            return {Result::io_error};
        }
        if (read.state == TcpStreamIoState::would_block)
            return {};
        if (read.state != TcpStreamIoState::progressed)
            return {Result::io_error};
        r.raw_size = read.size;
        r.raw_offset = 0;
        r.progress = now;
    }
    Io result{};
    while (r.raw_offset < r.raw_size && result.count < capacity && r.phase != Radio::Phase::ended) {
        const auto byte = r.raw[r.raw_offset++];
        if (r.phase == Radio::Phase::headers) {
            if (r.header_size == r.headers.size())
                return {Result::io_error};
            r.headers[r.header_size++] = static_cast<char>(byte);
            if (r.header_size >= kHttpHeaderTerminator.size() &&
                std::memcmp(r.headers.data() + r.header_size - kHttpHeaderTerminator.size(),
                    kHttpHeaderTerminator.data(), kHttpHeaderTerminator.size()) == 0) {
                r.phase = Radio::Phase::body;
                if (!response_headers(r))
                    return {Result::io_error};
            }
            continue;
        }
        bool payload{};
        if (!body_byte(r, byte, payload))
            return {Result::io_error};
        if (!payload)
            continue;
        if (r.meta_remaining) {
            if (r.metadata_size < r.metadata.size())
                r.metadata[r.metadata_size++] = static_cast<char>(byte);
            if (--r.meta_remaining == 0)
                icy_metadata(r);
            continue;
        }
        if (r.meta_length) {
            r.meta_remaining = std::uint32_t(byte) * kIcyMetadataLengthUnitBytes;
            r.meta_length = false;
            r.meta_audio = 0;
            r.metadata_size = 0;
            continue;
        }
        output[result.count++] = byte;
        if (r.meta_interval && ++r.meta_audio == r.meta_interval)
            r.meta_length = true;
    }
    result.eof = r.phase == Radio::Phase::ended;
    return result;
}

void radio_close(void*) {
    if (radio.stream.close)
        radio.stream.close(radio.stream.context);
    radio = {};
}

struct Server {
    enum class Phase : std::uint8_t { waiting, request, response, streaming, closing };
    TcpStream stream{};
    Phase phase{Phase::waiting};
    Handle session{};
    std::array<char, kMaximumHeaderBytes> request{};
    std::array<char, kServerResponseBytes> response{};
    /** One complete HTTP chunk (size line, payload, CRLF), sent with a single write. */
    std::array<std::uint8_t, kChunkHeaderBytes + kChunkPayloadBytes + 2> chunk{};
    std::size_t request_size{}, response_size{}, response_offset{}, chunk_size{}, chunk_offset{},
        end_offset{};
    std::uint32_t progress{};
    bool attaching{}, attached{}, finishing{};
};

Server server;

/** @brief Sends the pending chunk; returns false only on a socket failure. */
bool flush_chunk() {
    auto& s = server;
    if (s.chunk_offset == s.chunk_size)
        return true;
    const auto sent = s.stream.write(
        s.stream.context, s.chunk.data() + s.chunk_offset, s.chunk_size - s.chunk_offset);
    if (sent.state == TcpStreamIoState::progressed) {
        s.chunk_offset += sent.size;
        s.progress = platform::monotonic_milliseconds();
    } else if (sent.state != TcpStreamIoState::would_block)
        return false;
    return true;
}

/** @brief Frames caller bytes as one HTTP chunk and sends it with a single write.
 * The payload is copied, so the caller's buffer is never retained.
 */
Io stream_write(void*, const std::uint8_t* bytes, std::size_t size) {
    auto& s = server;
    if (s.phase != Server::Phase::streaming)
        return {};
    if (s.stream.state(s.stream.context) != TcpStreamState::connected || !flush_chunk())
        return {Result::io_error};
    if (s.chunk_offset != s.chunk_size || !size)
        return {};
    const auto amount = std::min<std::size_t>(size, kChunkPayloadBytes);
    const auto length = std::snprintf(
        reinterpret_cast<char*>(s.chunk.data()), kChunkHeaderBytes, "%x\r\n", unsigned(amount));
    if (length < 0 || static_cast<std::size_t>(length) >= kChunkHeaderBytes)
        return {Result::io_error};
    std::memcpy(s.chunk.data() + length, bytes, amount);
    s.chunk[length + amount] = '\r';
    s.chunk[length + amount + 1U] = '\n';
    s.chunk_size = static_cast<std::size_t>(length) + amount + kHttpCrlf.size();
    s.chunk_offset = 0;
    if (!flush_chunk())
        return {Result::io_error};
    return {Result::success, amount};
}

void stream_close(void*) {
    server.attached = false;
}

Result endpoint(const Config& config, bool input, Endpoint& opened) {
    opened = {};
    if (input && config.source == Source::network) {
        if (radio.phase != Radio::Phase::closed)
            return Result::busy;
        std::array<char, kHostBufferBytes> host{};
        std::uint16_t port{};
        std::string_view authority, path;
        if (std::string_view{config.location.data()}.substr(0, kHttpsScheme.size()) == kHttpsScheme)
            return Result::unsupported;
        if (!url(config.location.data(), host, port, authority, path))
            return Result::invalid_argument;
        radio = {};
        radio.stream = platform::audio_tcp_stream();
        const auto count = std::snprintf(radio.request.data(), radio.request.size(),
            "GET %s%.*s HTTP/1.1\r\nHost: %.*s\r\nConnection: close\r\nIcy-MetaData: "
            "1\r\nUser-Agent: Newmota\r\n\r\n",
            path.front() == '?' ? "/" : "", int(path.size()), path.data(), int(authority.size()),
            authority.data());
        if (count < 0 || static_cast<std::size_t>(count) >= radio.request.size()) {
            radio = {};
            return Result::invalid_argument;
        }
        radio.request_size = static_cast<std::size_t>(count);
        const auto result = radio.stream.open(radio.stream.context, host.data(), port, false);
        if (result != platform::TcpStreamOpenResult::accepted) {
            radio = {};
            return result == platform::TcpStreamOpenResult::busy ? Result::busy : Result::io_error;
        }
        radio.phase = Radio::Phase::connecting;
        radio.progress = platform::monotonic_milliseconds();
        opened = {&radio, radio_read, nullptr, radio_close};
        return Result::success;
    }
    if (!input && config.sink == Sink::network && server.attaching && !server.attached) {
        server.attached = true;
        opened = {&server, nullptr, stream_write, stream_close};
        return Result::success;
    }
    return Result::unsupported;
}

Result control(bool enabled) {
    if (!enabled) {
        const auto stopped = stop(server.session);
        if (stopped != Result::success)
            return stopped;
        server = {};
    }
    return platform::audio_listen(enabled) ? Result::success : Result::io_error;
}

void close_server() {
    const auto released = stop(server.session);
    if (server.stream.close)
        server.stream.close(server.stream.context);
    if (released != Result::success) {
        server.phase = Server::Phase::closing;
        return;
    }
    server = {};
}

/** @brief Sends as much of the pending response as the socket accepts.
 * @return False if the connection failed.
 */
bool send_response() {
    auto& s = server;
    const auto sent = s.stream.write(s.stream.context,
        reinterpret_cast<const std::uint8_t*>(s.response.data()) + s.response_offset,
        s.response_size - s.response_offset);
    if (sent.state == TcpStreamIoState::progressed) {
        s.response_offset += sent.size;
        s.progress = platform::monotonic_milliseconds();
        if (s.response_offset == s.response_size)
            s.phase = s.finishing ? Server::Phase::closing : Server::Phase::streaming;
        return true;
    }
    return sent.state == TcpStreamIoState::would_block;
}

/** @brief Starts capture only after a complete bounded request chooses its compiled encoder.
 * The response goes out in the same pass, so audio chunks follow the headers at once.
 */
void serve_request() {
    auto& s = server;
    const std::string_view request{s.request.data(), s.request_size};
    const auto end = request.find("\r\n");
    const auto line = request.substr(0, end);
    Codec codec{};
    bool valid = true;
    if (line == "GET /stream.mp3 HTTP/1.1" || line == "GET /stream.mp3 HTTP/1.0" ||
        line == "GET /stream.m3a HTTP/1.1" || line == "GET /stream.m3a HTTP/1.0")
        codec = Codec::mp3;
    else if (line == "GET /stream.webm HTTP/1.1" || line == "GET /stream.webm HTTP/1.0")
        codec = Codec::opus;
    else
        valid = false;
    auto config = command_config();
    config.source = Source::microphone;
    config.sink = Sink::network;
    config.codec = codec;
    config.loop = false;
    s.attaching = true;
    const auto opened = valid ? start(config, s.session) : Result::invalid_argument;
    s.attaching = false;
    if (opened == Result::success) {
        const auto length = std::snprintf(s.response.data(), s.response.size(),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nCache-Control: "
            "no-store\r\nConnection: close\r\n\r\n",
            codec == Codec::mp3 ? "audio/mpeg" : "audio/webm; codecs=opus");
        s.response_size = length > 0 ? static_cast<std::size_t>(length) : 0;
    } else {
        const auto* response =
            valid ? "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: "
                    "close\r\n\r\n"
                  : "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        s.response_size = std::strlen(response);
        std::copy_n(response, s.response_size, s.response.data());
        s.finishing = true;
    }
    s.phase = Server::Phase::response;
    s.progress = platform::monotonic_milliseconds();
    if (!send_response())
        close_server();
}
} // namespace

std::string_view network_station() {
    return radio.station.data();
}

std::string_view network_title() {
    return radio.title.data();
}

void initialize_network() {
    (void)register_endpoint_provider(endpoint);
    set_stream_control(control);
}

void poll_network() {
    if (!stream_enabled())
        return;
    const auto listener = platform::audio_listener_state();
    if (listener == platform::AudioListenerState::failed) {
        (void)stream_control(false);
        return;
    }
    if (listener != platform::AudioListenerState::listening)
        return;
    auto& s = server;
    if (!s.stream.state)
        s.stream = platform::audio_listener_stream();
    const auto connected = s.stream.state(s.stream.context);
    if (s.phase == Server::Phase::waiting) {
        if (connected == TcpStreamState::connected) {
            s.phase = Server::Phase::request;
            s.progress = platform::monotonic_milliseconds();
        } else
            return;
    }
    if (connected != TcpStreamState::connected) {
        close_server();
        return;
    }
    if (platform::monotonic_milliseconds() - s.progress >= kNetworkTimeoutMilliseconds) {
        close_server();
        return;
    }
    if (s.phase == Server::Phase::request) {
        std::array<std::uint8_t, kListenerReadBufferBytes> input{};
        const auto received = s.stream.read(s.stream.context, input.data(), input.size());
        if (received.state == TcpStreamIoState::would_block)
            return;
        if (received.state != TcpStreamIoState::progressed) {
            close_server();
            return;
        }
        s.progress = platform::monotonic_milliseconds();
        for (std::size_t i = 0; i < received.size; ++i) {
            if (s.request_size == s.request.size()) {
                close_server();
                return;
            }
            s.request[s.request_size++] = static_cast<char>(input[i]);
            if (s.request_size >= kHttpHeaderTerminator.size() &&
                std::memcmp(s.request.data() + s.request_size - kHttpHeaderTerminator.size(),
                    kHttpHeaderTerminator.data(), kHttpHeaderTerminator.size()) == 0) {
                serve_request();
                return;
            }
        }
    } else if (s.phase == Server::Phase::response) {
        if (!send_response())
            close_server();
    } else if (s.phase == Server::Phase::streaming) {
        const auto session = status(s.session);
        if (session.state == State::failed || !s.attached) {
            close_server();
            return;
        }
        if (session.state == State::ended) {
            // Capture is normally indefinite; native finish requests produce a terminal chunk
            // after the last data chunk has been sent.
            constexpr std::uint8_t end[] = {'0', '\r', '\n', '\r', '\n'};
            if (!flush_chunk()) {
                close_server();
                return;
            }
            if (s.chunk_offset != s.chunk_size)
                return;
            const auto sent =
                s.stream.write(s.stream.context, end + s.end_offset, sizeof(end) - s.end_offset);
            if (sent.state == TcpStreamIoState::progressed)
                s.end_offset += sent.size;
            else if (sent.state != TcpStreamIoState::would_block) {
                close_server();
                return;
            }
            if (s.end_offset == sizeof(end)) {
                s.phase = Server::Phase::closing;
                s.progress = platform::monotonic_milliseconds();
            }
        }
    } else if (s.phase == Server::Phase::closing && platform::audio_listener_idle())
        close_server();
}
} // namespace newmota::audio
