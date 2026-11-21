/*
  gps_service.cpp - Cooperative UBX GPS owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file gps_service.cpp
 * @brief Older u-blox 6 through 8 volatile setup and checked navigation intake.
 */
#include "drivers/gps/gps_service.h"
#include "core/hardware/hardware.h"
#include "services/time/time_service.h"

namespace newmota::gps {
namespace {
constexpr std::uint32_t kFresh = 5000;
constexpr std::uint8_t kSteps = 11;

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
}
} // namespace

void Service::initialize(const Hardware& hardware, TimeService& time, std::uint32_t now) {
    // Startup owns initialization. Never lose an existing or quarantined UART generation.
    if (time_)
        return;
    time_ = &time;
    const PinBinding *rx{}, *tx{};
    unsigned rx_count{}, tx_count{};
    for (std::size_t i = 0; i < hardware.binding_count(); ++i) {
        const auto* b = hardware.binding(i);
        if (b->role == PinRole::gps_rx) {
            ++rx_count;
            rx = b;
        }
        if (b->role == PinRole::gps_tx) {
            ++tx_count;
            tx = b;
        }
    }
    if (!rx_count && !tx_count)
        return;
    retained_.state = "invalid_bindings";
    if (rx_count != 1 || tx_count != 1 || !rx->active() || !tx->active() || rx->instance < 1 ||
        rx->instance > 3)
        return;
    serial::Config config{};
    config.rx = rx->pin;
    config.tx = tx->pin;
    config.baud = 9600U << (rx->instance - 1);
    retained_.baud = config.baud;
    const auto result = serial::open_gps(config, handle_);
    if (result != serial::Result::success) {
        retained_.state = "uart_unavailable";
        return;
    }
    retained_.open = true;
    retained_.state = "configuring";
    (void)serial::discard_rx(handle_);
    navigation_at_ = now;
    deadline_ = now + 150;
}

void Service::invalidate(std::uint32_t now) {
    time_seen_ = fix_seen_ = position_seen_ = velocity_seen_ = false;
    if (time_)
        time_->invalidate_gps(now);
}

void Service::retry(std::uint32_t now) {
    ++retained_.retries;
    sent_ = command_size_ = 0;
    if (++attempt_ >= 3) {
        deadline_ = now + 30000;
        phase_ = 3;
        retained_.state = "configuration_failed";
        return;
    }
    phase_ = 0;
    deadline_ = now + 500U * attempt_;
}

void Service::configure(std::uint32_t now) {
    if (phase_ == 3) {
        if (!reached(now, deadline_))
            return;
        step_ = attempt_ = phase_ = 0;
        retained_.state = "configuring";
    }
    if (step_ == kSteps) {
        if (static_cast<std::uint32_t>(now - navigation_at_) < 10000U)
            return;
        phase_ = 3;
        deadline_ = now + 30000;
        retained_.state = "backoff";
        return;
    }
    if (phase_ == 2) {
        if (reached(now, deadline_))
            retry(now);
        return;
    }
    if (phase_ == 0) {
        if (!reached(now, deadline_))
            return;
        std::uint8_t payload[6]{};
        std::uint8_t id = 1;
        std::size_t size = 3;
        if (step_ < 6) {
            payload[0] = 0xf0;
            payload[1] = step_;
        } else if (step_ < 10) {
            constexpr std::uint8_t navigation[]{0x21, 3, 2, 0x12};
            payload[0] = 1;
            payload[1] = navigation[step_ - 6];
            payload[2] = 1;
        } else {
            id = 8;
            size = 6;
            payload[0] = 0xe8;
            payload[1] = 3; // 1000 ms measurement period.
            payload[2] = 1;
            payload[4] = 1; // One measurement per nav solution, GPS time.
        }
        command_size_ = ubx::encode(6, id, payload, size, command_.data(), command_.size());
        sent_ = 0;
        phase_ = 1;
        deadline_ = now + 1500;
    }
    if (phase_ == 1) {
        if (reached(now, deadline_)) {
            retry(now);
            return;
        }
        const auto result =
            serial::write_some(handle_, command_.data() + sent_, command_size_ - sent_);
        if (result.result != serial::Result::success && result.result != serial::Result::busy) {
            retry(now);
            return;
        }
        sent_ += result.count;
        if (sent_ == command_size_) {
            phase_ = 2;
            deadline_ = now + 1500;
        }
    }
}

void Service::receive(const ubx::Frame& frame, std::uint32_t now) {
    if (frame.message_class == 5) {
        if (phase_ != 2 || frame.payload[0] != 6 || frame.payload[1] != (step_ == 10 ? 8 : 1))
            return;
        if (frame.id == 0) {
            ++retained_.naks;
            retry(now);
            return;
        }
        ++step_;
        attempt_ = 0;
        phase_ = 0;
        deadline_ = now + 150;
        if (step_ == kSteps)
            retained_.state = "setup_sent";
        return;
    }
    navigation_at_ = now;
    switch (frame.id) {
    case 0x21: {
        ubx::Utc sample{};
        time_seen_ = ubx::decode_utc(frame, sample);
        if (!time_seen_) {
            ++retained_.rejected;
            time_->invalidate_gps(now);
            break;
        }
        time_at_ = now;
        retained_.utc = sample.seconds;
        retained_.time_accuracy_ns = sample.accuracy_ns;
        (void)time_->accept_gps_seconds(sample.seconds, sample.millisecond, now);
        break;
    }
    case 3:
        fix_seen_ = ubx::decode_fix(frame, retained_.fix);
        fix_at_ = now;
        if (!fix_seen_)
            ++retained_.rejected;
        break;
    case 2:
        position_seen_ = ubx::decode_position(frame, retained_.position);
        position_at_ = now;
        if (!position_seen_)
            ++retained_.rejected;
        break;
    case 0x12:
        velocity_seen_ = ubx::decode_velocity(frame, retained_.velocity);
        velocity_at_ = now;
        if (!velocity_seen_)
            ++retained_.rejected;
        break;
    default:
        break;
    }
}

void Service::poll(std::uint32_t now) {
    if (!retained_.open)
        return;
    if (!serial::active(handle_)) {
        retained_.open = false;
        retained_.state = "uart_fault";
        invalidate(now);
        return;
    }
    const auto status = serial::status(handle_);
    if (status.dropped != retained_.dropped || status.errors != retained_.uart_errors) {
        retained_.dropped = status.dropped;
        retained_.uart_errors = status.errors;
        parser_.reset();
        invalidate(now);
        (void)serial::discard_rx(handle_);
        return;
    }
    configure(now);
    std::uint8_t bytes[256]{};
    const auto result = serial::read(handle_, bytes, sizeof bytes);
    if (result.result != serial::Result::success && result.result != serial::Result::busy) {
        parser_.reset();
        invalidate(now);
        ++read_errors_;
        return;
    }
    parser_.expire(now);
    for (std::size_t i = 0; i < result.count; ++i)
        if (parser_.feed(bytes[i], now))
            receive(parser_.frame(), now);
}

Snapshot Service::snapshot(std::uint32_t now) const {
    auto result = retained_;
    result.uart_errors += read_errors_;
    result.framing = parser_.errors();
    result.time_age = time_seen_ ? now - time_at_ : UINT32_MAX;
    result.fix_age = fix_seen_ ? now - fix_at_ : UINT32_MAX;
    result.position_age = position_seen_ ? now - position_at_ : UINT32_MAX;
    result.velocity_age = velocity_seen_ ? now - velocity_at_ : UINT32_MAX;
    result.time_valid = result.open && time_seen_ && result.time_age < kFresh;
    result.fix_valid = result.open && fix_seen_ && result.fix_age < kFresh && result.fix.valid;
    result.position_valid = result.fix_valid && position_seen_ && result.position_age < kFresh &&
                            result.position.itow == result.fix.itow;
    result.velocity_valid = result.position_valid && velocity_seen_ &&
                            result.velocity_age < kFresh &&
                            result.velocity.itow == result.position.itow;
    return result;
}

Service& service() {
    static Service owner;
    return owner;
}
} // namespace newmota::gps
