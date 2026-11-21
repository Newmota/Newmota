/*
  mqtt_wire.cpp - host tests for the bounded MQTT stream boundary

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/mqtt/mqtt_wire.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace {

struct FakeStream final {
    std::size_t max_write{1U};
    std::array<std::uint8_t, 1280> output{};
    std::size_t output_size{};

    static newmota::platform::TcpStreamState state(void*) {
        return newmota::platform::TcpStreamState::connected;
    }

    static newmota::platform::TcpStreamIoResult write(
        void* context, const std::uint8_t* input, const std::size_t size) {
        auto& stream = *static_cast<FakeStream*>(context);
        const auto count = size < stream.max_write ? size : stream.max_write;
        std::memcpy(stream.output.data() + stream.output_size, input, count);
        stream.output_size += count;
        return {newmota::platform::TcpStreamIoState::progressed, count};
    }
};

void expect(const bool condition, const char* const message, int& failures) {
    if (!condition) {
        std::fprintf(stderr, "mqtt wire: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    using namespace newmota;
    int failures = 0;

    MqttFrameReader reader;
    const std::array<std::uint8_t, 4> connack{0x20U, 0x02U, 0x00U, 0x00U};
    for (std::size_t index = 0U; index < connack.size(); ++index) {
        std::size_t consumed = 0U;
        const auto result = reader.feed(&connack[index], 1U, consumed);
        expect(consumed == 1U, "one-byte stream input is consumed", failures);
        if (index + 1U == connack.size()) {
            expect(result == MqttFrameResult::complete && reader.complete(),
                "split CONNACK becomes one complete frame", failures);
        } else {
            expect(
                result == MqttFrameResult::accepted, "partial CONNACK remains accepted", failures);
        }
    }
    expect(reader.size() == connack.size() &&
               std::memcmp(reader.data(), connack.data(), connack.size()) == 0,
        "complete frame bytes remain unchanged", failures);
    reader.reset();

    const std::array<std::uint8_t, 2> invalid_flags{0x21U, 0x00U};
    std::size_t consumed = 0U;
    expect(reader.feed(invalid_flags.data(), invalid_flags.size(), consumed) ==
               MqttFrameResult::invalid,
        "invalid fixed-header flags are rejected", failures);
    reader.reset();

    const std::array<std::uint8_t, 2> duplicate_qos_zero{0x38U, 0x00U};
    expect(reader.feed(duplicate_qos_zero.data(), duplicate_qos_zero.size(), consumed) ==
               MqttFrameResult::invalid,
        "DUP on QoS 0 is rejected", failures);
    reader.reset();

    const std::array<std::uint8_t, 3> reserved_packet_type{0xe0U, 0x00U, 0x00U};
    expect(reader.feed(reserved_packet_type.data(), reserved_packet_type.size(), consumed) ==
               MqttFrameResult::invalid,
        "reserved MQTT 3.1.1 packet type is rejected", failures);
    reader.reset();

    const std::array<std::uint8_t, 6> invalid_length{0x30U, 0x80U, 0x80U, 0x80U, 0x80U, 0x00U};
    expect(reader.feed(invalid_length.data(), invalid_length.size(), consumed) ==
               MqttFrameResult::invalid,
        "five-byte remaining length is rejected", failures);
    reader.reset();

    const std::array<std::uint8_t, 3> non_minimal_length{0xd0U, 0x80U, 0x00U};
    expect(reader.feed(non_minimal_length.data(), non_minimal_length.size(), consumed) ==
               MqttFrameResult::invalid,
        "non-minimal remaining length is rejected", failures);
    reader.reset();

    const std::array<std::uint8_t, 3> oversized{0x90U, 0x80U, 0x10U};
    expect(reader.feed(oversized.data(), oversized.size(), consumed) == MqttFrameResult::too_large,
        "a control packet above the fixed frame capacity is rejected", failures);
    reader.reset();

    // A publication above the capacity is consumed in pieces, then the next packet parses.
    std::array<std::uint8_t, 700> body{};
    const std::array<std::uint8_t, 3> large_publish{0x30U, 0x80U, 0x10U}; // Remaining length 2048.
    expect(reader.feed(large_publish.data(), large_publish.size(), consumed) ==
               MqttFrameResult::accepted,
        "a large publication starts skipping", failures);
    expect(reader.feed(body.data(), body.size(), consumed) == MqttFrameResult::accepted &&
               consumed == body.size(),
        "skipping consumes partial input", failures);
    expect(reader.feed(body.data(), body.size(), consumed) == MqttFrameResult::accepted,
        "skipping continues", failures);
    const std::array<std::uint8_t, 650> tail{};
    expect(reader.feed(tail.data(), tail.size(), consumed) == MqttFrameResult::skipped &&
               consumed == 648U,
        "skipping ends exactly at the frame boundary", failures);
    const std::array<std::uint8_t, 2> ping{0xD0U, 0x00U};
    expect(reader.feed(ping.data(), ping.size(), consumed) == MqttFrameResult::complete,
        "the next packet parses after a skip", failures);
    reader.reset();

    const std::array<std::uint8_t, 6> packet{0xD0U, 0x04U, 0x01U, 0x02U, 0x03U, 0x04U};
    MqttPacketWriter writer;
    expect(writer.queue(packet.data(), packet.size()), "one complete packet is queued", failures);
    expect(!writer.queue(packet.data(), packet.size()),
        "a second packet cannot overtake a partial write", failures);
    FakeStream fake;
    const platform::TcpStream stream{
        &fake, &FakeStream::state, nullptr, nullptr, &FakeStream::write, nullptr};
    while (writer.pending()) {
        const auto progress = writer.poll(stream);
        expect(progress == MqttWriteProgress::pending || progress == MqttWriteProgress::sent,
            "partial stream writes remain bounded", failures);
    }
    expect(fake.output_size == packet.size() &&
               std::memcmp(fake.output.data(), packet.data(), packet.size()) == 0,
        "partial writes preserve packet bytes and order", failures);

    if (failures == 0)
        std::puts("MQTT wire tests passed");
    return failures == 0 ? 0 : 1;
}
