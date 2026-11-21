/*
  gps_service.h - Cooperative UBX GPS owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file gps_service.h
 * @brief Retained navigation diagnostics and bounded GPS UART configuration.
 */
#pragma once
#include "drivers/gps/ubx_protocol.h"
#include "drivers/serial/serial_service.h"

namespace newmota {
class Hardware;
class TimeService;

namespace gps {
/** @brief Retained raw values; validity expires independently for each observation. */
struct Snapshot {
    const char* state{"unconfigured"};
    std::uint32_t baud{}, utc{}, time_age{}, fix_age{}, position_age{}, velocity_age{};
    std::uint32_t time_accuracy_ns{};
    std::uint32_t rejected{}, retries{}, naks{}, dropped{}, uart_errors{};
    ubx::Errors framing{};
    ubx::Fix fix{};
    ubx::Position position{};
    ubx::Velocity velocity{};
    bool open{}, time_valid{}, fix_valid{}, position_valid{}, velocity_valid{};
};

/** @brief One application-task GPS owner, with bounded work and no persistent changes. */
class Service final {
  public:
    /** @brief Opens exactly one active RX and TX binding and starts volatile setup. */
    void initialize(const Hardware& hardware, TimeService& time, std::uint32_t now);
    /** @brief Reads at most 256 bytes and advances one nonblocking setup transaction. */
    void poll(std::uint32_t now);
    /** @brief Returns diagnostics without UART I/O or accepting old time samples. */
    [[nodiscard]] Snapshot snapshot(std::uint32_t now) const;

  private:
    void receive(const ubx::Frame&, std::uint32_t now);
    void configure(std::uint32_t now);
    void retry(std::uint32_t now);
    void invalidate(std::uint32_t now);
    serial::Handle handle_{};
    TimeService* time_{};
    ubx::Parser parser_{};
    Snapshot retained_{};
    std::uint32_t time_at_{}, fix_at_{}, position_at_{}, velocity_at_{}, deadline_{},
        navigation_at_{};
    std::uint32_t read_errors_{};
    std::array<std::uint8_t, 16> command_{};
    std::size_t command_size_{}, sent_{};
    std::uint8_t step_{}, attempt_{}, phase_{};
    bool time_seen_{}, fix_seen_{}, position_seen_{}, velocity_seen_{};
};

/** @brief Returns the static GPS owner. */
Service& service();
} // namespace gps
} // namespace newmota
