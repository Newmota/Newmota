/*
  v9240.cpp - Portable V9240 UART metering and native calibration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol and initialization adapted from Tasmota xnrg_25_v9240.ino:
  SPDX-FileCopyrightText: 2025 Ivan Chopa (@aquaforum)
  SPDX-License-Identifier: GPL-3.0-only
*/

/** @file v9240.cpp
 * @brief Cooperative echoed UART register transactions, DC startup calibration and metrology.
 * Frames are encoded explicitly; no packed bitfields, unaligned casts, signed
 * checksum overflow or host-endian register layouts cross the UART boundary.
 */
#include "services/energy/energy_service.h"
#include "drivers/serial/serial_service.h"
#include "newmota/platform/platform.h"
#include <array>
#include <cmath>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::uint16_t first_calibration = 0x00f6;
constexpr std::uint32_t freshness_ms = 5000, response_timeout_ms = 1000;

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

/** @brief Modulo-256 sum complement plus the protocol's 0x33 offset. */
std::uint8_t checksum(const std::uint8_t* bytes, std::size_t size) {
    std::uint8_t sum{};
    for (std::size_t i = 0; i < size; ++i)
        sum = static_cast<std::uint8_t>(sum + bytes[i]);
    return static_cast<std::uint8_t>(std::uint8_t(~sum) + 0x33U);
}

std::uint32_t get32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

enum class Stage : std::uint8_t {
    request_reset,
    await_reset,
    idle,
    transmit,
    drain,
    receive,
    quiet
};

/** @brief One UART, bounded 100-byte maximum response and one independent channel.
 * Register writes become effective on the chip after cooperative startup; saved
 * software coefficients are authoritative and are reapplied after every recovery.
 */
class V9240 final : public Device {
    serial::Config uart_{};
    serial::Handle port_{};
    serial::Status status_{};
    Calibration calibration_{};
    std::array<std::uint32_t, 23> rw_{};
    std::array<std::uint32_t, 22> ro_{};
    std::array<std::uint8_t, 8> request_{};
    std::array<std::uint8_t, 100> response_{};
    std::size_t transmitted_{}, received_{}, expected_{};
    std::uint32_t now_{}, due_{}, deadline_{}, measured_at_{}, integrated_at_{};
    std::int64_t previous_power_{};
#if USE_ENERGY_POWER_POLICY
    Sample powered_sample_{};
    std::uint32_t primary_tick_at_{};
    std::uint8_t primary_data_age_{};
    bool primary_controlled_{}, primary_on_{}, primary_watchdog_armed_{};
#endif
    std::uint16_t address_{};
    std::uint8_t step_{}, words_{};
    Stage stage_{Stage::request_reset};
    bool reading_{}, ready_{}, restart_requested_{}, reset_complete_{}, resource_fault_{};

    std::uint32_t& reg(std::uint16_t address) { return rw_[3 + address - first_calibration]; }

    /** @brief Suspends the observation interval without erasing the primary static endpoint.
     * Source last_power survives OFF, UART failures/reinitialization and calibration.
     */
    void pause_accounting() {
        integrated_at_ = now_;
#if USE_ENERGY_POWER_POLICY
        if (!primary_controlled_)
#endif
            previous_power_ = 0;
    }

    void restart() {
        ready_ = false;
        pause_accounting();
        step_ = 0;
        restart_requested_ = false;
        due_ = now_ + 11;
        stage_ = Stage::idle;
        (void)serial::discard_rx(port_);
    }

    void fail() {
        ready_ = false;
        pause_accounting();
        stage_ = Stage::quiet;
        due_ = now_ + response_timeout_ms;
    }

    void queue() {
        if (restart_requested_) {
            restart();
            return;
        }
        reading_ = false;
        words_ = 1;
        std::uint32_t value{};
        switch (step_) {
        case 0:
            address_ = 0x00ca;
            reading_ = true;
            break;
        case 1:
            address_ = 0x0180;
            reading_ = true;
            break;
        case 2:
            address_ = 0x0182;
            value = rw_[1];
            break;
        case 3:
            address_ = 0x0183;
            value = rw_[2] = 0x10000000;
            break;
        case 4:
            address_ = 0x0180;
            value = rw_[0] |= 0x2cc00000;
            break;
        case 5:
            address_ = 0x0180;
            value = rw_[0] |= 0x3000;
            break;
        case 6:
            address_ = 0x00ca;
            reading_ = true;
            words_ = 22;
            break;
        case 7:
            address_ = 0x0180;
            value = rw_[0] &= ~0x3000U;
            break;
        case 8:
            address_ = 0x0106;
            value = reg(address_);
            break;
        case 9:
            address_ = 0x0104;
            value = reg(address_);
            break;
        case 10:
            address_ = 0x00f6;
            value = reg(address_) = calibration_.power;
            break;
        case 11:
            address_ = 0x00f8;
            value = reg(address_) = 0;
            break;
        case 12:
            address_ = 0x00f9;
            value = reg(address_) = UINT32_MAX;
            break;
        case 13:
            address_ = 0x00fb;
            value = reg(address_) = 0;
            break;
        case 14:
            address_ = 0x00f7;
            value = reg(address_) = calibration_.frequency;
            break;
        case 15:
            address_ = 0x00fd;
            value = reg(address_) = calibration_.current;
            break;
        case 16:
            address_ = 0x00fe;
            value = reg(address_) = 75969;
            break;
        case 17:
            address_ = 0x00ff;
            value = reg(address_) = calibration_.voltage;
            break;
        case 18:
            address_ = 0x0108;
            value = reg(address_) = 3596949;
            break;
        case 19:
            address_ = 0x0107;
            value = reg(address_) = 0x806764b6;
            break;
        case 20: {
            std::uint32_t sum{};
            for (std::size_t i = 0; i + 1 < rw_.size(); ++i)
                sum += rw_[i];
            address_ = 0x0109;
            value = reg(address_) = ~sum;
            break;
        }
        case 21:
            address_ = 0x00e0;
            reading_ = true;
            break;
        default:
            address_ = 0x00ca;
            reading_ = true;
            words_ = 21;
            break;
        }
        if (reading_)
            value = words_;
        request_[0] = 0x7d;
        request_[1] = static_cast<std::uint8_t>(((address_ >> 8) << 4) | (reading_ ? 1 : 2));
        request_[2] = static_cast<std::uint8_t>(address_);
        for (unsigned i = 0; i < 4; ++i)
            request_[3 + i] = static_cast<std::uint8_t>(value >> (8 * i));
        request_[7] = checksum(request_.data(), 7);
        expected_ = 12 + (reading_ ? 4 * words_ : 0);
        transmitted_ = received_ = 0;
        if (serial::discard_rx(port_) != serial::Result::success) {
            fail();
            return;
        }
        deadline_ = now_ + response_timeout_ms;
        stage_ = Stage::transmit;
    }

    bool accept() {
        for (std::size_t i = 0; i < request_.size(); ++i)
            if (response_[i] != request_[i])
                return false;
        const auto* reply = response_.data() + 8;
        const auto reply_size = expected_ - 8;
        if (reply[0] != 0x7d || (reply[1] & 0x0f) != (reading_ ? 1 : 2) ||
            checksum(reply, reply_size - 1) != reply[reply_size - 1])
            return false;
        if (reading_) {
            if (reply[2] != words_ && !(words_ == 1 && reply[2] == 0))
                return false;
            if (step_ == 1)
                rw_[0] = get32(reply + 3);
            else if (step_ == 21)
                ro_[21] = get32(reply + 3);
            else {
                for (unsigned i = 0; i < words_; ++i)
                    ro_[i] = get32(reply + 3 + 4 * i);
                if (step_ == 6) {
                    reg(0x0106) = ro_[15];
                    reg(0x0104) = ro_[16];
                }
                if (step_ == 22) {
                    if (!ready_) {
                        pause_accounting();
                    }
                    ready_ = true;
                    measured_at_ = now_;
                }
            }
        }
        step_ = step_ >= 22 ? 21 : step_ + 1;
        due_ = now_ + 11;
        stage_ = Stage::idle;
        return true;
    }

    void pump() {
        if (resource_fault_ || !serial::active(port_)) {
            ready_ = false;
            return;
        }
        const auto status = serial::status(port_);
        if (reset_complete_ &&
            (status.errors != status_.errors || status.dropped != status_.dropped))
            fail();
        status_ = status;
        switch (stage_) {
        case Stage::request_reset: {
            // A 100-baud 8O1 zero byte is 90 ms low, then 20 ms high.
            const auto result = serial::request_break(port_, 90, 20);
            if (result == serial::Result::success)
                stage_ = Stage::await_reset;
            else if (result != serial::Result::busy || reached(now_, deadline_))
                resource_fault_ = true;
            break;
        }
        case Stage::await_reset:
            if (!serial::break_pending(port_)) {
                reset_complete_ = true;
                status_ = serial::status(port_);
                restart();
            }
            break;
        case Stage::idle:
            if (reached(now_, due_))
                queue();
            break;
        case Stage::transmit: {
            const auto result = serial::write_some(
                port_, request_.data() + transmitted_, request_.size() - transmitted_);
            transmitted_ += result.count;
            if (result.result != serial::Result::success || reached(now_, deadline_))
                fail();
            else if (transmitted_ == request_.size())
                stage_ = Stage::drain;
            break;
        }
        case Stage::drain:
            if (serial::tx_idle(port_)) {
                deadline_ = now_ + response_timeout_ms;
                stage_ = Stage::receive;
            } else if (reached(now_, deadline_))
                fail();
            break;
        case Stage::receive: {
            const auto result =
                serial::read(port_, response_.data() + received_, expected_ - received_);
            received_ += result.count;
            if (result.result != serial::Result::success)
                fail();
            else if (received_ == expected_) {
                if (!accept())
                    fail();
            } else if (reached(now_, deadline_))
                fail();
            break;
        }
        case Stage::quiet:
            if (reached(now_, due_) && serial::tx_idle(port_))
                restart();
            break;
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies the existing source age without changing watchdog or accounting state. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
#if USE_ENERGY_POWER_POLICY
        if (!primary_controlled_ || channel)
            return false;
        output = primary_data_age_;
        return true;
#else
        (void)channel;
        (void)output;
        return false;
#endif
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    /** @brief Matches the source model's shared voltage/frequency topology. */
    bool common_voltage() const override { return true; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return true; }

    ~V9240() override { (void)serial::request_close(port_); }

    /** @brief Acquires 19200-8O1 and schedules the equivalent low-baud reset waveform. */
    bool open(const Configuration& config) {
        if (config.channels != 1 || config.identity.bus || config.identity.address ||
            !set_options(config.options))
            return false;
        for (const auto& pin : config.identity.pins)
            if (pin.function != PinFunction::none && pin.function != PinFunction::uart_rx &&
                pin.function != PinFunction::uart_tx)
                return false;
        calibration_ = config.calibration;
        uart_.rx = config.identity.pin(PinFunction::uart_rx);
        uart_.tx = config.identity.pin(PinFunction::uart_tx);
        uart_.baud = 19200;
        uart_.parity = platform::serial::Parity::odd;
        if (!uart_.rx.valid() || !uart_.tx.valid() ||
            serial::open(uart_, port_) != serial::Result::success)
            return false;
        now_ = platform::monotonic_milliseconds();
        deadline_ = now_ + 1000;
        status_ = serial::status(port_);
        return true;
    }

    bool online() const override { return ready_ && !restart_requested_ && serial::active(port_); }

    bool signed_calibration() const override { return true; }

    bool initial_calibration(Calibration& value) const override {
        value = {12530, 1950, 3500, 0};
        return true;
    }

    bool primary_current_in_amperes() const override { return true; }

    bool calibrate(const Calibration& value) override {
        calibration_ = value;
        if (port_.generation) {
            ready_ = false;
            restart_requested_ = true;
            pause_accounting();
        }
        return true;
    }

    bool set_options(const Options& value) override {
        return !value.flags && value.invalid_power_frames == 128;
    }

    /** @brief Applies the pinned signed affine correction in matching canonical units. */
    bool calibration_for(
        CalibrationField field, std::uint32_t reference, Calibration& output) const override {
        if (!ready_ || restart_requested_ || now_ - measured_at_ > freshness_ms ||
            field == CalibrationField::frequency)
            return false;
        const auto measured = field == CalibrationField::power
                                  ? static_cast<long double>(std::int32_t(ro_[6]))
                              : field == CalibrationField::voltage
                                  ? static_cast<long double>(std::int32_t(ro_[10])) / 1000
                                  : static_cast<long double>(std::int32_t(ro_[9])) / 1000;
        if (!measured)
            return false;
        const auto goal = std::int32_t(reference);
        const auto native = field == CalibrationField::power     ? calibration_.power
                            : field == CalibrationField::voltage ? calibration_.voltage
                                                                 : calibration_.current;
        const auto corrected =
            (static_cast<long double>(INT32_MAX) * (std::int64_t(goal) - measured) +
                static_cast<long double>(std::int32_t(native)) * goal) /
            measured;
        if (!std::isfinite(corrected) || corrected < INT32_MIN || corrected > INT32_MAX)
            return false;
        const auto value = std::uint32_t(static_cast<std::int32_t>(corrected));
        if (field == CalibrationField::power)
            output.power = value;
        else if (field == CalibrationField::voltage)
            output.voltage = value;
        else
            output.current = value;
        return true;
    }

#if USE_ENERGY_POWER_POLICY
    /** @brief Follows the source global Power/SetOption21 gate. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
        now_ = now;
        pump();
        if (!count)
            return;
#if USE_ENERGY_POWER_POLICY
        if (primary_controlled_) {
            /** Data validity counts source one-second observation ticks, not UART arrivals.
             * The core starts aging after uptime4; age0 becomes invalid when incremented past4.
             * Keep the armed state through monotonic wrap and do not replay missed ticks.
             */
            if (now_ >= 5000)
                primary_watchdog_armed_ = true;
            const bool tick = now_ - primary_tick_at_ >= 1000;
            if (tick) {
                primary_tick_at_ = now_;
                if (primary_watchdog_armed_ && primary_data_age_ <= 4)
                    ++primary_data_age_;
            }
            if (!tick || !primary_on_) {
                /** Source OFF leaves published arrays and last_power unchanged. UART work
                 * continues, but cannot refresh measurement age. Never replay an increment or
                 * integrate the OFF interval. Source watchdog reset zeros remain available,
                 * separately from physical measurement validity and confirmed increments.
                 */
                auto held = powered_sample_;
                held.use_absolute = true;
                held.increment_fields = 0;
                held.valid = held.valid && primary_data_age_ <= 4;
                if (primary_data_age_ > 4) {
                    held.watchdog_reset = true;
                    held.voltage = held.current = held.power = 0;
                    held.frequency_millihz = 0;
                    held.apparent_mva = held.reactive_mvar = held.power_factor_milli = 0;
                }
                samples[0] = held;
                if (!primary_on_)
                    integrated_at_ = now_;
                return;
            }
        }
#endif
        Sample sample{};
        sample.use_absolute = true; // Only the once-per-second trapezoid increment is accounted.
        sample.voltage = std::int32_t(ro_[10]) / 1000;
        sample.current = std::int32_t(ro_[9]) / 1000;
        sample.power = std::int32_t(ro_[6]);
        sample.valid = ready_ && !restart_requested_ && now_ - measured_at_ <= freshness_ms;
        sample.available_fields = sample.valid ? 7 : 0;
        if (sample.valid) {
            if (std::int32_t(ro_[8]) > 0 && std::int32_t(ro_[21]) > 0) {
                const auto frequency = std::uint64_t(ro_[21]) * 75000 / ro_[8];
                if (frequency <= UINT32_MAX) {
                    sample.frequency_millihz = frequency;
                    sample.auxiliary_fields |= 1;
                }
            }
            sample.reactive_mvar = std::int32_t(ro_[7]);
            sample.apparent_mva = static_cast<std::int64_t>(std::hypot(
                static_cast<double>(sample.power), static_cast<double>(sample.reactive_mvar)));
            sample.power_factor_milli = sample.apparent_mva
                                            ? saturate_i32(static_cast<std::int64_t>(sample.power) *
                                                           1000 / sample.apparent_mva)
                                            : 0;
            sample.auxiliary_fields |= 2 | (1U << 4) | (1U << 5);
        }
        const auto elapsed = now_ - integrated_at_;
        const bool integration_due =
            elapsed >= 1000
#if USE_ENERGY_POWER_POLICY
            || primary_controlled_ /**< Primary already admits only one source second. */
#endif
            ;
#if USE_ENERGY_POWER_POLICY
        const bool local_power_policy = !primary_controlled_;
#else
        constexpr bool local_power_policy = true;
#endif
        if (!sample.valid || restart_requested_ || (local_power_policy && elapsed > freshness_ms)) {
            pause_accounting();
        } else if (integration_due) {
#if USE_ENERGY_POWER_POLICY
            /** The source primary hook integrates one fixed second, never elapsed OFF time. */
            const auto interval = primary_controlled_ ? 1000U : elapsed;
#else
            const auto interval = elapsed;
#endif
            const auto numerator = (sample.power + previous_power_) * interval;
            sample.increment_denominator = 7200;
            if (numerator >= 0) {
                sample.import_increment = std::uint64_t(numerator);
                sample.increment_fields = 1;
            } else {
                sample.export_increment = std::uint64_t(-numerator);
                sample.increment_fields = 2;
            }
            integrated_at_ = now_;
            previous_power_ = sample.power;
        }
#if USE_ENERGY_POWER_POLICY
        if (primary_controlled_) {
            powered_sample_ = sample;
            powered_sample_.increment_fields = 0;
            if (sample.valid)
                primary_data_age_ = 0;
        }
#endif
        samples[0] = sample;
    }
};
} // namespace

/** @brief Creates one selectable V9240 with independent UART, calibration and trapezoid state.
 * @param config Physical UART resources and retained operating coefficients.
 * @return An acquired instance, or an empty pointer when resources/configuration are unavailable.
 */
std::unique_ptr<Device> create_v9240(const Configuration& config) {
    auto meter = std::unique_ptr<V9240>(new (std::nothrow) V9240);
    if (!meter || !meter->open(config))
        return {};
    return meter;
}
} // namespace newmota::energy
