/*
  matter_ble.cpp - Matter commissioning link over the BLE server

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "services/matter/matter_ble.h"

#include "services/matter/matter_btp.h"

#include "newmota/log.h"
#include "newmota/platform/ble.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace newmota::matter {
namespace {
// 0000FFF6-0000-1000-8000-00805F9B34FB and 18EE2EF5-263D-4559-959F-4F9C429F9D1x, little-endian.
constexpr std::uint8_t kService[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xF6, 0xFF, 0x00, 0x00};
constexpr std::uint8_t kWrite[16] = {
    0x11, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18};
constexpr std::uint8_t kIndicate[16] = {
    0x12, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE, 0x18};
constexpr std::uint16_t kIndicateShort = 0x2EF5; // What events report: bytes 12 and 13 of the UUID.
constexpr std::uint8_t kOperationAdvertise = 201;
constexpr std::uint8_t kOperationResponse = 202;
constexpr std::uint8_t kOperationCharacteristic = 211;
constexpr std::uint8_t kOperationAdvertiseParameters = 232;
constexpr std::int32_t kFlagsWrite = 0x08;
constexpr std::int32_t kFlagsIndicate = 0x20;
constexpr std::uint8_t kEventWrite = 222;
constexpr std::uint8_t kEventSubscribedNotify = 224;
constexpr std::uint8_t kEventSubscribedIndicate = 225;
constexpr std::uint8_t kEventDisconnected = 228;
constexpr std::uint8_t kEventIndicated = 229;
constexpr unsigned kEventsPerPoll = 8;
constexpr std::uint32_t kConfirmTimeoutMilliseconds = 30000;

enum class Step : std::uint8_t { define_write, define_indicate, advertise, response, serving };

struct Link {
    Btp btp{};
    Step step{Step::define_write};
    bool job_running{};
    bool opened{};
    bool waiting_confirm{};
    bool stack_was_running{};
    std::uint32_t confirm_deadline{};
    std::uint8_t advertisement[15]{};
    std::uint8_t job[Btp::kMaxSegment + 8U]{};
    std::uint8_t reply[255]{};
    platform::UdpDatagram* message{};
};

Link* link{};

/** @brief Starts a server job with the service and characteristic filled in. */
[[nodiscard]] bool start_job(const std::uint8_t operation, const std::uint8_t* const characteristic,
    const std::uint8_t* const data, const std::size_t length, const std::int32_t flags = 0) {
    platform::BleRequest request{};
    request.operation = operation;
    request.confirm = true;
    request.has_arg1 = flags != 0;
    request.arg1 = flags;
    std::memcpy(request.service, kService, sizeof(kService));
    if (characteristic != nullptr)
        std::memcpy(request.characteristic, characteristic, sizeof(request.characteristic));
    request.data = data;
    request.length = static_cast<std::uint16_t>(length);
    request.reply = link->reply;
    request.reply_capacity = sizeof(link->reply);
    if (!platform::ble_connection_request(request))
        return false;
    link->job_running = true;
    return true;
}

/** @brief Runs the next setup step once the previous job finished. */
void advance_setup() {
    if (link->job_running)
        return;
    static constexpr std::uint8_t kEmpty[1] = {0};
    static constexpr std::uint8_t kNoResponse[2] = {1, 0x09};
    switch (link->step) {
    case Step::define_write:
        if (start_job(kOperationCharacteristic, kWrite, kEmpty, 1, kFlagsWrite))
            link->step = Step::define_indicate;
        break;
    case Step::define_indicate:
        if (start_job(kOperationCharacteristic, kIndicate, kEmpty, 1, kFlagsIndicate))
            link->step = Step::advertise;
        break;
    case Step::advertise:
        if (start_job(
                kOperationAdvertise, nullptr, link->advertisement, sizeof(link->advertisement)))
            link->step = Step::response;
        break;
    case Step::response:
        if (start_job(kOperationResponse, nullptr, kNoResponse, sizeof(kNoResponse)))
            link->step = Step::serving;
        break;
    case Step::serving:
        break;
    }
}

/** @brief Drops the connection state; the platform keeps advertising after a disconnect. */
void forget_peer() {
    link->btp.reset();
    link->waiting_confirm = false;
    if (link->message != nullptr) {
        platform::matter_udp_release(link->message);
        link->message = nullptr;
    }
}

/** @brief Copies a finished message for the session stack; one waits at a time. */
void deliver() {
    const std::size_t size = link->btp.message_size();
    auto* const datagram =
        static_cast<platform::UdpDatagram*>(std::malloc(sizeof(platform::UdpDatagram) + size));
    if (datagram == nullptr || link->message != nullptr) {
        std::free(datagram);
        AddLog(LogLevel::error, "MTR: BLE message dropped");
        return;
    }
    new (datagram) platform::UdpDatagram{};
    datagram->size = static_cast<std::uint16_t>(size);
    std::memcpy(datagram->data, link->btp.message(), size);
    link->message = datagram;
}

void handle_event(const platform::BleEvent& event, const std::uint8_t* const payload) {
    if (event.operation == kOperationCharacteristic || event.operation == kOperationAdvertise ||
        event.operation == kOperationResponse) {
        link->job_running = false;
        return;
    }
    if (event.operation == kEventDisconnected) {
        forget_peer();
        return;
    }
    // Only the two BTP characteristics exist here. A write to the wrong one is a BTP violation.
    if (event.operation == kEventWrite) {
        platform::BleInfo info{};
        platform::ble_info(info);
        const auto received = link->btp.on_write(payload, event.length, info.connection.mtu);
        if (received == Btp::Received::message)
            deliver();
        else if (received == Btp::Received::violation)
            forget_peer();
    } else if ((event.operation == kEventSubscribedNotify ||
                   event.operation == kEventSubscribedIndicate) &&
               event.uuid == kIndicateShort) {
        // A Mac first subscribes to Service Changed. A response sent then would reach nobody.
        link->btp.on_subscribed();
    } else if (event.operation == kEventIndicated && link->waiting_confirm && event.length >= 4U) {
        int code = 0;
        std::memcpy(&code, payload, sizeof(code));
#if defined(ESP_PLATFORM)
        const bool confirmed = code != 0; // NimBLE reports zero when sent, then the confirmation.
#else
        const bool confirmed = true; // Other hosts report once, when the transaction ended.
#endif
        if (confirmed) {
            link->waiting_confirm = false;
            link->btp.on_confirmed();
        }
    }
}
} // namespace

bool ble_link_start(
    const std::uint16_t discriminator, const std::uint16_t vendor, const std::uint16_t product) {
    if (link != nullptr)
        return true;
    link = new (std::nothrow) Link{};
    if (link == nullptr)
        return false;
    link->stack_was_running = platform::ble_ready();
    if (!platform::ble_start()) {
        delete link;
        link = nullptr;
        return false;
    }
    const std::uint8_t advertisement[15] = {0x02, 0x01, 0x06, 0x0B, 0x16, 0xF6, 0xFF, 0x00,
        static_cast<std::uint8_t>(discriminator), static_cast<std::uint8_t>(discriminator >> 8U),
        static_cast<std::uint8_t>(vendor), static_cast<std::uint8_t>(vendor >> 8U),
        static_cast<std::uint8_t>(product), static_cast<std::uint8_t>(product >> 8U), 0x00};
    std::memcpy(link->advertisement, advertisement, sizeof(advertisement));
    AddLog(LogLevel::info, "MTR: BLE commissioning advertised");
    return true;
}

void ble_link_stop() {
    if (link == nullptr)
        return;
    forget_peer();
    platform::ble_connection_close();
    // A stop also removes the commissioning service. A stack SetOption115 runs starts again.
    if (!platform::ble_stop())
        AddLog(LogLevel::error, "MTR: BLE stop failed");
    else if (link->stack_was_running && !platform::ble_start())
        AddLog(LogLevel::error, "BLE: start failed");
    delete link;
    link = nullptr;
    AddLog(LogLevel::info, "MTR: BLE commissioning closed");
}

void ble_link_poll(const std::uint32_t now) {
    if (link == nullptr || !platform::ble_ready())
        return;
    if (!link->opened) {
        if (!platform::ble_connection_open())
            return;
        constexpr std::uint8_t kParameters[5] = {
            2, 0x50, 0x00, 0xA0, 0x00}; // Connectable, 50-100 ms.
        platform::BleRequest parameters{};
        parameters.operation = kOperationAdvertiseParameters;
        parameters.data = kParameters;
        parameters.length = sizeof(kParameters);
        (void)platform::ble_configure(parameters);
        link->opened = true;
    }
    for (unsigned count = 0; count < kEventsPerPoll; ++count) {
        platform::BleEvent event{};
        if (!platform::ble_connection_poll(event, link->reply, sizeof(link->reply)))
            break;
        handle_event(event, link->reply);
    }
    advance_setup();
    if (link->step != Step::serving || link->job_running || platform::ble_connection_busy())
        return;
    if (link->waiting_confirm) {
        if (static_cast<std::int32_t>(now - link->confirm_deadline) >= 0) {
            AddLog(LogLevel::error, "MTR: BLE peer stopped confirming");
            forget_peer();
        }
        return;
    }
    const std::size_t size = link->btp.next(link->job, sizeof(link->job));
    if (size == 0U)
        return;
    if (start_job(kOperationCharacteristic, kIndicate, link->job, size)) {
        link->waiting_confirm = true;
        link->confirm_deadline = now + kConfirmTimeoutMilliseconds;
    }
}

platform::UdpDatagram* ble_link_take() {
    if (link == nullptr || link->message == nullptr)
        return nullptr;
    platform::UdpDatagram* const message = link->message;
    link->message = nullptr;
    return message;
}

bool ble_link_send(const std::uint8_t* const data, const std::size_t size) {
    return link != nullptr && link->btp.enqueue(data, size);
}

} // namespace newmota::matter
