/*
  matter_wire.cpp - hostile input tests for the Matter wire codec and DNS-SD

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_btp.h"
#include "services/matter/matter_cert.h"
#include "services/matter/matter_dns_sd.h"
#include "services/matter/matter_wire.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace newmota::matter;

namespace {
void check(const bool condition, const char* const name) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", name);
        std::exit(1);
    }
}

/** @brief Every prefix of a valid buffer must fail or stay inside it, under the sanitizers. */
void walk_tlv(const std::vector<std::uint8_t>& data, const std::size_t size) {
    std::size_t offset = 0;
    TlvElement element;
    while (tlv_next(data.data(), size, offset, element)) {
        check(element.next > offset && element.next <= size, "tlv advances inside the buffer");
        if (element.data != nullptr)
            check(element.data + element.length <= data.data() + size, "tlv data inside buffer");
        offset = element.next;
    }
}

void test_frame() {
    FrameHeader header;
    header.session_id = 0x1234;
    header.counter = 0xA1B2C3D4;
    header.has_destination = true;
    header.destination[0] = 0x77;
    std::uint8_t buffer[64];
    const std::size_t size = write_frame(buffer, sizeof(buffer), header);
    check(size == 16U, "frame size");
    FrameHeader parsed;
    check(parse_frame(buffer, size, parsed), "frame parses");
    check(parsed.session_id == 0x1234 && parsed.counter == 0xA1B2C3D4, "frame fields");
    check(parsed.has_destination && parsed.destination[0] == 0x77, "frame destination");
    for (std::size_t length = 0; length < size; ++length)
        check(!parse_frame(buffer, length, parsed), "truncated frame refused");
    std::uint8_t group[8] = {0x02, 1, 0, 0, 0, 0, 0, 0};
    check(!parse_frame(group, sizeof(group), parsed), "group destination refused");
    std::uint8_t version[8] = {0x10, 1, 0, 0, 0, 0, 0, 0};
    check(!parse_frame(version, sizeof(version), parsed), "other version refused");
    std::uint8_t privacy[8] = {0x00, 1, 0, 0x80, 0, 0, 0, 0};
    check(!parse_frame(privacy, sizeof(privacy), parsed), "privacy refused");
    check(write_frame(buffer, 15, header) == 0U, "small output refused");
}

void test_protocol() {
    ProtocolHeader header;
    header.flags = kExchangeAck | kExchangeInitiator;
    header.opcode = 0x20;
    header.exchange = 7;
    header.protocol = 1;
    header.ack_counter = 99;
    std::uint8_t buffer[16];
    const std::size_t size = write_protocol(buffer, sizeof(buffer), header);
    ProtocolHeader parsed;
    check(size == 10U && parse_protocol(buffer, size, parsed), "protocol parses");
    check(parsed.ack_counter == 99 && parsed.exchange == 7, "protocol fields");
    for (std::size_t length = 0; length < size; ++length)
        check(!parse_protocol(buffer, length, parsed), "truncated protocol refused");
    const std::uint8_t extension[8] = {kExchangeSecuredExtensions, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    check(!parse_protocol(extension, sizeof(extension), parsed), "oversized extension refused");
}

void test_tlv() {
    const std::vector<std::uint8_t> valid = {0x15, 0x24, 0x01, 0x05, 0x30, 0x02, 0x02, 0xAA, 0xBB,
        0x2C, 0x03, 0x01, 'x', 0x29, 0x04, 0x34, 0x05, 0x18};
    TlvElement element;
    check(tlv_next(valid.data(), valid.size(), 0, element), "struct");
    check(element.type == TlvType::structure && !element.has_tag, "struct type");
    check(tlv_next(valid.data(), valid.size(), 1, element), "u8");
    check(element.type == TlvType::unsigned_integer && element.tag == 1 && element.value == 5,
        "u8 value");
    check(tlv_next(valid.data(), valid.size(), 4, element), "octets");
    check(element.type == TlvType::octets && element.length == 2 && element.data[1] == 0xBB,
        "octets value");
    for (std::size_t size = 0; size <= valid.size(); ++size)
        walk_tlv(valid, size);
    // Declared lengths past the buffer, with every length width.
    for (const unsigned kind : {0x10U, 0x11U, 0x12U, 0x13U, 0x0CU, 0x0DU, 0x0EU, 0x0FU}) {
        const std::vector<std::uint8_t> hostile = {
            static_cast<std::uint8_t>(kind), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        check(!tlv_next(hostile.data(), hostile.size(), 0, element), "huge length refused");
    }
    // Lists split into items: a nested item is skipped whole and gains the tag it travels with.
    const std::vector<std::uint8_t> items = {
        0x15, 0x24, 0x01, 0x05, 0x37, 0x02, 0x24, 0x03, 0x06, 0x18, 0x18, 0x24};
    TlvElement top;
    check(tlv_next(items.data(), items.size(), 0, top), "item");
    std::size_t after = 0;
    check(
        tlv_skip(items.data(), items.size(), after, top) && after == 11U, "skips a nested element");
    std::uint8_t retagged[16];
    check(tlv_with_context_tag(items.data(), 11, 2, retagged, sizeof(retagged)) == 12U &&
              retagged[0] == 0x35 && retagged[1] == 2 && retagged[2] == 0x24,
        "gains a context tag");
    check(tlv_with_context_tag(items.data(), 11, 2, retagged, 11) == 0U, "tagged copy is bounded");
    check(tlv_with_context_tag(items.data() + 1, 3, 2, retagged, sizeof(retagged)) == 0U,
        "an element with a tag stays refused");
    std::size_t cut = 0;
    check(!tlv_skip(items.data(), 5, cut, top), "a truncated container fails");
    const std::vector<std::uint8_t> deep(40, 0x15);
    TlvElement deep_top;
    check(tlv_next(deep.data(), deep.size(), 0, deep_top) &&
              !tlv_skip(deep.data(), deep.size(), cut, deep_top),
        "deep nesting fails");
    const std::vector<std::uint8_t> reserved = {0x19};
    check(!tlv_next(reserved.data(), reserved.size(), 0, element), "reserved type refused");
    // Every control byte, with and without trailing data, stays inside the buffer.
    for (unsigned control = 0; control <= 0xFFU; ++control) {
        for (std::size_t extra = 0; extra < 12; ++extra) {
            std::vector<std::uint8_t> data(1U + extra, 0x01);
            data[0] = static_cast<std::uint8_t>(control);
            walk_tlv(data, data.size());
        }
    }
}

DnsRecords records() {
    DnsRecords result;
    std::strcpy(result.host, "AABBCCDDEEFF");
    result.service_count = 1;
    DnsService& service = result.services[0];
    std::strcpy(service.instance, "0011223344556677");
    service.commissionable = true;
    std::strcpy(service.subtypes[0], "_L3840");
    std::strcpy(service.txt, "D=3840\nCM=1");
    result.address_count = 1;
    result.addresses[0][0] = 0xFE;
    return result;
}

std::vector<std::uint8_t> query(const std::vector<const char*>& labels, const std::uint16_t type) {
    std::vector<std::uint8_t> packet(12, 0);
    packet[5] = 1;
    for (const char* const label : labels) {
        packet.push_back(static_cast<std::uint8_t>(std::strlen(label)));
        packet.insert(packet.end(), label, label + std::strlen(label));
    }
    packet.push_back(0);
    packet.push_back(static_cast<std::uint8_t>(type >> 8U));
    packet.push_back(static_cast<std::uint8_t>(type));
    packet.push_back(0);
    packet.push_back(1);
    return packet;
}

void test_dns() {
    const DnsRecords published = records();
    std::uint8_t output[kMaxFrameSize];
    auto answer = [&](const std::vector<std::uint8_t>& packet) {
        return dns_sd_respond(packet.data(), packet.size(), published, output, sizeof(output));
    };
    check(answer(query({"_matterc", "_udp", "local"}, 12)) != 0U, "service type answered");
    check(answer(query({"_MATTERC", "_UDP", "LOCAL"}, 12)) != 0U, "case ignored");
    check(answer(query({"_L3840", "_sub", "_matterc", "_udp", "local"}, 12)) != 0U, "subtype");
    check(answer(query({"0011223344556677", "_matterc", "_udp", "local"}, 33)) != 0U, "instance");
    check(answer(query({"AABBCCDDEEFF", "local"}, 28)) != 0U, "host");
    check(answer(query({"_services", "_dns-sd", "_udp", "local"}, 12)) != 0U, "enumeration");
    check(answer(query({"_L9", "_sub", "_matterc", "_udp", "local"}, 12)) == 0U, "other subtype");
    check(answer(query({"_http", "_tcp", "local"}, 12)) == 0U, "other service");
    check(dns_sd_announce(published, output, sizeof(output)) != 0U, "announce");

    // A pointer to itself and forward pointers must end without answering.
    std::vector<std::uint8_t> loop(12, 0);
    loop[5] = 1;
    loop.insert(loop.end(), {0xC0, 12, 0, 12, 0, 1});
    check(answer(loop) == 0U, "pointer loop refused");
    std::vector<std::uint8_t> forward(12, 0);
    forward[5] = 1;
    forward.insert(forward.end(), {0xC0, 200, 0, 12, 0, 1});
    check(answer(forward) == 0U, "forward pointer refused");

    // Truncated and small outputs stay in bounds.
    const std::vector<std::uint8_t> packet = query({"_matterc", "_udp", "local"}, 12);
    for (std::size_t size = 0; size < packet.size(); ++size)
        check(dns_sd_respond(packet.data(), size, published, output, sizeof(output)) == 0U,
            "truncated query refused");
    for (std::size_t capacity = 0; capacity < 300; ++capacity) {
        std::vector<std::uint8_t> small(capacity + 1U);
        const std::size_t size =
            dns_sd_respond(packet.data(), packet.size(), published, small.data(), capacity);
        check(size <= capacity, "response fits its output");
    }
    // Arbitrary bytes after a valid header.
    for (unsigned seed = 0; seed < 2000U; ++seed) {
        std::vector<std::uint8_t> noise(12U + seed % 80U);
        std::uint32_t value = seed * 2654435761U + 1U;
        for (std::uint8_t& byte : noise) {
            value = value * 1664525U + 1013904223U;
            byte = static_cast<std::uint8_t>(value >> 24U);
        }
        noise[2] = 0;
        noise[4] = 0;
        noise[5] = 1;
        (void)answer(noise);
    }
}

const std::uint8_t kHandshake[9] = {0x65, 0x6C, 0x04, 0, 0, 0, 100, 0, 5};

/** @brief Brings a link past its handshake; sequence zero is the response. */
void establish(Btp& link) {
    std::uint8_t output[64];
    check(
        link.on_write(kHandshake, sizeof(kHandshake), 247) == Btp::Received::nothing, "handshake");
    check(link.next(output, sizeof(output)) == 0U, "response waits for the subscription");
    link.on_subscribed();
    const std::size_t size = link.next(output, sizeof(output));
    check(size == 6U && output[0] == 0x65 && output[2] == 4 && output[3] == 100, "response");
    check(output[5] == Btp::kWindow, "our window");
    link.on_confirmed();
    check(link.established(), "established");
}

void test_btp() {
    static Btp link;
    std::uint8_t output[256];
    // Writes before the handshake and bad handshakes.
    const std::uint8_t data_frame[6] = {0x05, 0, 2, 0, 0xAA, 0xBB};
    check(link.on_write(data_frame, sizeof(data_frame), 247) == Btp::Received::violation,
        "no handshake");
    const std::uint8_t other_version[9] = {0x65, 0x6C, 0x03, 0, 0, 0, 100, 0, 5};
    check(link.on_write(other_version, 9, 247) == Btp::Received::violation, "other version");
    const std::uint8_t no_window[9] = {0x65, 0x6C, 0x04, 0, 0, 0, 100, 0, 0};
    check(link.on_write(no_window, 9, 247) == Btp::Received::violation, "zero window");
    for (std::size_t size = 0; size < 9; ++size)
        check(link.on_write(kHandshake, size, 247) == Btp::Received::violation, "short handshake");

    // A message in one segment, then an acknowledgement goes out.
    link.reset();
    establish(link);
    check(link.on_write(data_frame, sizeof(data_frame), 247) == Btp::Received::message, "message");
    check(link.message_size() == 2 && link.message()[1] == 0xBB, "message content");
    std::size_t size = link.next(output, sizeof(output));
    check(size == 3U && output[0] == 0x08 && output[1] == 0 && output[2] == 1, "standalone ack");
    check(link.next(output, sizeof(output)) == 0U, "one indication at a time");
    link.on_confirmed();

    // A bare acknowledgement needs no answer, so both ends fall idle.
    const std::uint8_t bare_ack[3] = {0x08, 1, 1};
    check(link.on_write(bare_ack, 3, 247) == Btp::Received::nothing, "bare acknowledgement");
    check(link.next(output, sizeof(output)) == 0U, "bare acknowledgement is not acknowledged");
    const std::uint8_t first[6] = {0x01, 2, 3, 0, 1, 2};
    const std::uint8_t second[3] = {0x06, 3, 3};

    // Two segments, with the declared length enforced.
    check(link.on_write(first, 6, 247) == Btp::Received::nothing, "first segment");
    check(link.on_write(second, 3, 247) == Btp::Received::message, "last segment");
    check(link.message_size() == 3 && link.message()[2] == 3, "assembled");

    // Declared length, overrun, sequence and acknowledgement violations.
    link.reset();
    establish(link);
    const std::uint8_t huge[5] = {0x01, 0, 0xFF, 0xFF, 0};
    check(link.on_write(huge, 5, 247) == Btp::Received::violation, "oversized message");
    link.reset();
    establish(link);
    const std::uint8_t overrun[6] = {0x05, 0, 1, 0, 1, 2};
    check(link.on_write(overrun, 6, 247) == Btp::Received::violation,
        "segment above declared length");
    link.reset();
    establish(link);
    const std::uint8_t short_end[5] = {0x05, 0, 2, 0, 1};
    check(
        link.on_write(short_end, 5, 247) == Btp::Received::violation, "end below declared length");
    link.reset();
    establish(link);
    const std::uint8_t wrong_sequence[5] = {0x05, 9, 1, 0, 1};
    check(link.on_write(wrong_sequence, 5, 247) == Btp::Received::violation, "wrong sequence");
    link.reset();
    establish(link);
    const std::uint8_t future_ack[6] = {0x0D, 9, 0, 1, 0, 1};
    check(link.on_write(future_ack, 6, 247) == Btp::Received::violation, "ack of something unsent");
    link.reset();
    establish(link);
    const std::uint8_t orphan[3] = {0x06, 0, 1};
    check(link.on_write(orphan, 3, 247) == Btp::Received::violation, "continue without begin");

    // Sending splits at the segment size, stops at the peer's window, and resumes on its ack.
    link.reset();
    establish(link);
    std::vector<std::uint8_t> message(1000, 0x5A);
    check(link.enqueue(message.data(), message.size()), "enqueue");
    std::size_t total = 0;
    unsigned segments = 0;
    std::uint8_t last = 0;
    while ((size = link.next(output, sizeof(output))) != 0U) {
        check(size <= 100U, "segment size respected");
        last = output[output[0] & 0x08 ? 2 : 1];
        total += size;
        ++segments;
        link.on_confirmed();
    }
    check(segments == 4U, "window of four limits unacknowledged segments");
    const std::uint8_t ack[3] = {0x08, last, 0};
    check(link.on_write(ack, 3, 247) == Btp::Received::nothing, "ack accepted");
    const auto resumed = link.next(output, sizeof(output));
    check(resumed != 0U && (output[0] & 0x08) != 0U, "sending resumes with a piggybacked ack");
    (void)total;

    // A peer that never acknowledges bare acknowledgements cannot block us.
    link.reset();
    establish(link);
    for (std::uint8_t sequence = 0; sequence < 8U; ++sequence) {
        const std::uint8_t segment[5] = {0x05, sequence, 1, 0, 7};
        check(link.on_write(segment, 5, 247) == Btp::Received::message, "peer message");
        check(link.next(output, sizeof(output)) != 0U, "acknowledged despite a full window");
        link.on_confirmed();
    }
    check(link.enqueue(message.data(), 10), "response queued");
    check(link.next(output, sizeof(output)) != 0U && (output[0] & 0x01U) != 0U,
        "bare acknowledgements do not close the window for data");

    // Queue bound.
    link.reset();
    establish(link);
    for (std::size_t index = 0; index < Btp::kQueue; ++index)
        check(link.enqueue(message.data(), 10), "queue accepts");
    check(!link.enqueue(message.data(), 10), "queue is bounded");
    check(!link.enqueue(message.data(), kMaxFrameSize + 1U), "message size bounded");

    // Arbitrary writes never leave the buffers.
    link.reset();
    establish(link);
    for (unsigned seed = 0; seed < 4000U; ++seed) {
        std::vector<std::uint8_t> noise(1U + seed % 40U);
        std::uint32_t value = seed * 2654435761U + 7U;
        for (std::uint8_t& byte : noise) {
            value = value * 1664525U + 1013904223U;
            byte = static_cast<std::uint8_t>(value >> 24U);
        }
        if (link.on_write(noise.data(), noise.size(), 247) == Btp::Received::violation) {
            link.reset();
            establish(link);
        }
        check(link.message_size() <= kMaxFrameSize, "reassembly stays in bounds");
    }
}

// Generated with an X.509 encoder from the same fields as the TLV, so a match proves the DER form.
const char* const kCertificate =
    "1530010801020304050607082402013703271401000000cacacaca271505000000000000001826048089432b"
    "26054db8ed3e3706271142000000000000002715050000000000000026160100cdab18240701240801300941"
    "0452c6ff5b71de65d83c75d393e249ec03894370515d0b229b1c6224681f2a5030b6f323c20c20d021d611e0"
    "87cf482f5297e6135cbee6d3994832164e479e6e96370a350118250201003603040204011830041400010203"
    "0405060708090a0b0c0d0e0f101112133005141415161718191a1b1c1d1e1f202122232425262718300b401e"
    "346665aebc17ecb4d6d4caeeba99014d9102fb7dfe389021e2faf48c7492485eaeace61d65844d231dbaa23e"
    "a34bdddf4fa9bd6440e81a08bda80b1c5ccb0c18";
const char* const kSignedData =
    "308201c2a00302010202080102030405060708300a06082a8648ce3d04030230443120301e060a2b06010401"
    "82a27c01040c10434143414341434130303030303030313120301e060a2b0601040182a27c01050c10303030"
    "30303030303030303030303035301e170d3233303130313030303030305a170d333330363135313233303035"
    "5a305e3120301e060a2b0601040182a27c01010c10303030303030303030303030303034323120301e060a2b"
    "0601040182a27c01050c103030303030303030303030303030303531183016060a2b0601040182a27c01060c"
    "0841424344303030313059301306072a8648ce3d020106082a8648ce3d0301070342000452c6ff5b71de65d8"
    "3c75d393e249ec03894370515d0b229b1c6224681f2a5030b6f323c20c20d021d611e087cf482f5297e6135c"
    "bee6d3994832164e479e6e96a38183308180300c0603551d130101ff04023000300e0603551d0f0101ff0404"
    "0302078030200603551d250101ff0416301406082b0601050507030206082b06010505070301301d0603551d"
    "0e04160414000102030405060708090a0b0c0d0e0f10111213301f0603551d23041830168014141516171819"
    "1a1b1c1d1e1f2021222324252627";

// From the Matter SDK's certificate test vectors: a root, and a node with future extensions.
const char* const kSdkRoot =
    "15300108534c4582736235142402013703271401000000cacacaca182604ef171b2726056eb5b94c37062714"
    "01000000cacacaca18240701240801300941043b88460ec9687a5d0f3b4b3b13fcd299c2f6d5051d003ee49c"
    "9924cf98f4f780eb20fd37c8d358347f5f87d08c3213e540af11bab9137e49354f0c5b6343de63370a350129"
    "0118240260300414cc1308af82cfee505eb23b57bfe86a311665535f300514cc1308af82cfee505eb23b57bf"
    "e86a311665535f18300b40f7f0092690494e46c8b1c5cbd1a5085e1e65d4360f98e96c4e8e495dc5e216d0bf"
    "a23d8f57470d89fddaf03f0464b0ae8e1f956d6f67a31124385824689780a918";
const char* const kSdkRootSigned =
    "30820143a0030201020208534c458273623514300a06082a8648ce3d04030230223120301e060a2b06010401"
    "82a27c01040c1043414341434143413030303030303031301e170d3230313031353134323334335a170d3430"
    "313031353134323334325a30223120301e060a2b0601040182a27c01040c1043414341434143413030303030"
    "3030313059301306072a8648ce3d020106082a8648ce3d030107034200043b88460ec9687a5d0f3b4b3b13fc"
    "d299c2f6d5051d003ee49c9924cf98f4f780eb20fd37c8d358347f5f87d08c3213e540af11bab9137e49354f"
    "0c5b6343de63a3633061300f0603551d130101ff040530030101ff300e0603551d0f0101ff04040302010630"
    "1d0603551d0e04160414cc1308af82cfee505eb23b57bfe86a311665535f301f0603551d23041830168014cc"
    "1308af82cfee505eb23b57bfe86a311665535f";
const char* const kSdkNode =
    "15300108607820c015a0d1662402013703271304000000cacacaca27151d0000000000b0fa182604ef171b27"
    "26056eb5b94c3706271107000200dededede27151d0000000000b0fa1824070124080130094104f73c7fe575"
    "d8b906d475fff7912ee0e1b3cd236c3246cf2d853de33984c1f41817b1c5b528018f90e62616eaa1877f4714"
    "b96fc8786f91b303e446a96509d361370a350128011824020136030402040118300414777c77b63565dc51f3"
    "02045963c5cefce7092a1e300514e1e76e6777851dd77416bddd35ec3c137c4729dc30061d301b0603551d11"
    "0101ff0411300f810d7465737440636869702e6f7267300622302006082b0601050507010104143012301006"
    "082b0601050507300186047465737418300b40a7c5939ddb189cd84c1e8548bc40c2c3afcd9819548cf8e5c2"
    "49d27daa5ed88619704695b71050fe701a5a675a4955981c287f25f673938b43e7717354f6c82e18";
const char* const kSdkNodeSigned =
    "308201e7a0030201020208607820c015a0d166300a06082a8648ce3d04030230443120301e060a2b06010401"
    "82a27c01030c10434143414341434130303030303030343120301e060a2b0601040182a27c01050c10464142"
    "30303030303030303030303144301e170d3230313031353134323334335a170d343031303135313432333432"
    "5a30443120301e060a2b0601040182a27c01010c10444544454445444530303032303030373120301e060a2b"
    "0601040182a27c01050c10464142303030303030303030303031443059301306072a8648ce3d020106082a86"
    "48ce3d03010703420004f73c7fe575d8b906d475fff7912ee0e1b3cd236c3246cf2d853de33984c1f41817b1"
    "c5b528018f90e62616eaa1877f4714b96fc8786f91b303e446a96509d361a381c23081bf300c0603551d1301"
    "01ff04023000300e0603551d0f0101ff04040302078030200603551d250101ff0416301406082b0601050507"
    "030206082b06010505070301301d0603551d0e04160414777c77b63565dc51f302045963c5cefce7092a1e30"
    "1f0603551d23041830168014e1e76e6777851dd77416bddd35ec3c137c4729dc301b0603551d110101ff0411"
    "300f810d7465737440636869702e6f7267302006082b0601050507010104143012301006082b060105050730"
    "01860474657374";

std::vector<std::uint8_t> from_hex(const char* const text) {
    std::vector<std::uint8_t> bytes;
    for (const char* cursor = text; cursor[0] != 0 && cursor[1] != 0; cursor += 2) {
        unsigned value = 0;
        std::sscanf(cursor, "%2x", &value);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

void test_certificate() {
    const std::vector<std::uint8_t> certificate = from_hex(kCertificate);
    const std::vector<std::uint8_t> expected = from_hex(kSignedData);
    std::uint8_t output[kMaxCertificateDer];
    const std::uint8_t* der = nullptr;
    const std::size_t size = certificate_signed_data(
        certificate.data(), certificate.size(), output, sizeof(output), &der);
    check(size == expected.size() && std::memcmp(der, expected.data(), size) == 0, "signed data");
    const std::vector<std::uint8_t> sdk_root = from_hex(kSdkRoot);
    const std::vector<std::uint8_t> sdk_root_signed = from_hex(kSdkRootSigned);
    std::size_t sdk_size =
        certificate_signed_data(sdk_root.data(), sdk_root.size(), output, sizeof(output), &der);
    check(sdk_size == sdk_root_signed.size() &&
              std::memcmp(der, sdk_root_signed.data(), sdk_size) == 0,
        "SDK root");
    const std::vector<std::uint8_t> sdk_node = from_hex(kSdkNode);
    const std::vector<std::uint8_t> sdk_node_signed = from_hex(kSdkNodeSigned);
    sdk_size =
        certificate_signed_data(sdk_node.data(), sdk_node.size(), output, sizeof(output), &der);
    check(sdk_size == sdk_node_signed.size() &&
              std::memcmp(der, sdk_node_signed.data(), sdk_size) == 0,
        "SDK node with future extensions");
    std::uint32_t cats[4];
    check(certificate_cats(certificate.data(), certificate.size(), cats, 4) == 1U &&
              cats[0] == 0xABCD0001U,
        "CASE Authenticated Tag");
    // Truncated, corrupted and small-output cases fail inside their bounds.
    for (std::size_t length = 0; length < certificate.size(); ++length) {
        check(
            certificate_signed_data(certificate.data(), length, output, sizeof(output), &der) == 0U,
            "truncated certificate");
        (void)certificate_cats(certificate.data(), length, cats, 4);
    }
    for (std::size_t capacity = 0; capacity < expected.size(); ++capacity) {
        std::vector<std::uint8_t> small(capacity + 1U);
        check(certificate_signed_data(
                  certificate.data(), certificate.size(), small.data(), capacity, &der) == 0U,
            "small output");
    }
    for (std::size_t position = 0; position < certificate.size(); ++position) {
        for (const unsigned mask : {0x01U, 0x20U, 0x80U, 0xFFU}) {
            std::vector<std::uint8_t> corrupted = certificate;
            corrupted[position] = static_cast<std::uint8_t>(corrupted[position] ^ mask);
            const std::size_t result = certificate_signed_data(
                corrupted.data(), corrupted.size(), output, sizeof(output), &der);
            check(result <= sizeof(output), "corrupted certificate stays in bounds");
            (void)certificate_cats(corrupted.data(), corrupted.size(), cats, 4);
        }
    }
    // Nesting beyond the limit is refused rather than followed.
    std::vector<std::uint8_t> deep(300, 0x35);
    check(certificate_signed_data(deep.data(), deep.size(), output, sizeof(output), &der) == 0U,
        "deep nesting refused");
}
} // namespace

int main() {
    test_frame();
    test_protocol();
    test_tlv();
    test_dns();
    test_btp();
    test_certificate();
    return 0;
}
