/*
  bl0906.cpp - Portable BL0906 multi-channel UART energy meter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol and calibration adapted from xnrg_24_bl0906.ino:
  SPDX-FileCopyrightText: 2024 Theo Arends
  SPDX-License-Identifier: GPL-3.0-only
  Upstream implementation references Athom's ESP32 BL0906 component.
*/

/** @file bl0906.cpp
 * @brief Poll-driven 24-bit register acquisition, factory offset setup and hardware counters.
 */
#include "services/energy/energy_service.h"
#if USE_BERRY_ENERGY_API
#include "services/energy/energy_observation_age.h"
#endif
#if USE_GLOBAL_TEMPERATURE
#include "services/temperature/global_temperature.h"
#endif
#include "drivers/serial/serial_service.h"
#include "drivers/sensors/sensor_service.h"
#include "core/command/command.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include <array>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::array<std::uint8_t, 6> current_registers{0x0d, 0x0e, 0x0f, 0x10, 0x13, 0x14};
constexpr std::array<std::uint8_t, 6> power_registers{0x23, 0x24, 0x25, 0x26, 0x29, 0x2a};
constexpr std::array<std::uint8_t, 6> counter_registers{0x30, 0x31, 0x32, 0x33, 0x36, 0x37};
constexpr std::array<std::uint8_t, 6> offset_registers{0x78, 0x79, 0x7a, 0x7b, 0x7e, 0x7f};
constexpr std::array<float, 6> bias_amperes{0.016f, 0.015f, 0.014f, 0.013f, 0.012f, 0.012f};
constexpr std::uint32_t poll_interval_ms = 2000, response_timeout_ms = 100, freshness_ms = 6000;
constexpr std::uint8_t maximum_retries = 2;

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

std::uint8_t checksum(std::uint8_t reg, const std::uint8_t* data) {
    return static_cast<std::uint8_t>((unsigned(reg) + data[0] + data[1] + data[2]) ^ 0xffU);
}

std::int32_t signed24(std::uint32_t value) {
    return static_cast<std::int32_t>((value & 0x800000U) ? value | 0xff000000U : value);
}

/** @brief Converts the pinned CF formula exactly to a normalized absolute energy.
 * Split quotient/remainder multiplication avoids overflowing uint64 for 24-bit input.
 * The result truncates below one micro-Wh; absolute-baseline differences avoid drift.
 */
EnergyValue counter_value(std::uint32_t raw) {
    constexpr std::uint64_t numerator = 5168602298712064ULL, denominator = 23186671875ULL;
    const auto micro = std::uint64_t(raw) * (numerator / denominator) +
                       std::uint64_t(raw) * (numerator % denominator) / denominator;
    return {micro / 1000000, std::uint32_t(micro % 1000000)};
}

/** @brief Reproduces the active pinned RMS-offset setup without signed-shift undefined behavior. */
std::uint32_t bias_value(unsigned channel) {
    constexpr float ki = 12875 * (5.1 + 5.1) * 1000 / 2000 / 1.097;
    const float measured = bias_amperes[channel] * ki;
    const auto value = static_cast<std::int32_t>(-measured * measured / 256);
    return (static_cast<std::uint32_t>(value) & 0xffffffU) | (value < 0 ? 0x800000U : 0);
}

enum class Stage : std::uint8_t { idle, transmit, drain, response, quiet };

/** @brief One exclusive UART and only the configured channel states.
 * Read requests are two bytes; responses contain three little-endian data bytes
 * and the complemented register/data sum. Setup writes six-byte unacknowledged
 * frames. No application poll waits for input or UART drain.
 */
class Bl0906 final : public Device {
    struct Channel {
#if USE_BERRY_ENERGY_API
        /** @brief Source triplet-attempt age for Berry; never a measurement validity input. */
        ObservationAge observation_age{};
#endif
        Sample sample{};
        std::array<std::uint32_t, 3> received{};
        std::uint8_t seen{};
        bool counter_pending{};
    };

    std::unique_ptr<Channel[]> channels_;
    serial::Handle handle_{};
    serial::Status status_{};
    std::array<std::uint8_t, 6> transmit_{};
    std::array<std::uint8_t, 4> response_{};
    std::size_t transmit_size_{}, transmitted_{}, received_{};
    std::uint32_t now_{}, due_{}, deadline_{}, scan_started_{}, quiet_until_{};
    std::uint32_t voltage_{}, frequency_{}, voltage_at_{}, frequency_at_{}, temperature_at_{};
    std::int64_t temperature_{};
    std::uint8_t count_{}, setup_step_{}, read_step_{}, requested_register_{}, retries_{},
        common_seen_{};
    Stage stage_{Stage::idle};
    bool setup_complete_{}, exporting_{};

    bool fresh(std::uint32_t when) const { return now_ - when <= freshness_ms; }

    std::uint8_t read_register() const {
        if (!read_step_)
            return 0x5e;
        if (read_step_ <= 3 * count_) {
            const auto channel = (read_step_ - 1) / 3;
            switch ((read_step_ - 1) % 3) {
            case 0:
                return current_registers[channel];
            case 1:
                return power_registers[channel];
            default:
                return counter_registers[channel];
            }
        }
        return read_step_ == 3 * count_ + 1 ? 0x4e : 0x16;
    }

    void advance_read() {
#if USE_BERRY_ENERGY_API
        /** @brief The pinned void read helper reports no success status: completing a phase's
         * current/power/CF attempt renews its byte even after exhausted register retries.
         * Common temperature/frequency/voltage steps and intermediate replies do not renew.
         */
        if (read_step_ && read_step_ <= 3 * count_ && read_step_ % 3 == 0)
            (void)channels_[(read_step_ - 1) / 3].observation_age.poll(now_, true);
#endif
        retries_ = 0;
        if (++read_step_ == 3 * count_ + 3) {
            read_step_ = 0;
            // Retain the two-second cadence without overlapping scans after slow/error replies.
            due_ = reached(now_, scan_started_ + poll_interval_ms)
                       ? now_ + 1
                       : scan_started_ + poll_interval_ms;
        } else
            due_ = now_;
    }

    void invalidate() {
        common_seen_ = 0;
        for (unsigned channel = 0; channel < count_; ++channel) {
            channels_[channel].seen = 0;
            channels_[channel].sample.valid = false;
        }
    }

    void fail() {
        // A response carries no address, so drain and quarantine late bytes before retrying.
        stage_ = Stage::quiet;
        quiet_until_ = now_ + response_timeout_ms + 5;
        received_ = 0;
        if (setup_complete_ && ++retries_ > maximum_retries)
            advance_read();
        AddLog(LogLevel::info,
            "BL0906: UART/checksum/response failure; retrying after quiet interval");
    }

    void queue() {
        if (serial::discard_rx(handle_) != serial::Result::success) {
            fail();
            return;
        }
        transmit_ = {};
        if (!setup_complete_) {
            transmit_[0] = 0xca;
            transmit_[1] =
                setup_step_ == 0 || setup_step_ == 7 ? 0x9e : offset_registers[setup_step_ - 1];
            const auto value = setup_step_ == 0   ? 0x5555U
                               : setup_step_ == 7 ? 0U
                                                  : bias_value(setup_step_ - 1);
            transmit_[2] = value;
            transmit_[3] = value >> 8;
            transmit_[4] = value >> 16;
            transmit_[5] = checksum(transmit_[1], transmit_.data() + 2);
            transmit_size_ = 6;
        } else {
            requested_register_ = read_register();
            transmit_[0] = 0x35;
            transmit_[1] = requested_register_;
            transmit_size_ = 2;
            if (!read_step_)
                scan_started_ = now_;
        }
        transmitted_ = received_ = 0;
        deadline_ = now_ + response_timeout_ms;
        stage_ = Stage::transmit;
    }

    void accept(std::uint32_t value) {
        if (!read_step_) {
            temperature_ = (std::int64_t(signed24(value)) - 64) * 12500 / 59 - 40000;
#if USE_GLOBAL_TEMPERATURE
            global_temperature().observe(static_cast<std::int32_t>(temperature_), now_);
#endif
            temperature_at_ = now_;
            common_seen_ |= 4;
        } else if (read_step_ <= 3 * count_) {
            const auto phase = (read_step_ - 1) / 3, field = (read_step_ - 1) % 3;
            auto& channel = channels_[phase];
            if (!field)
                channel.sample.current = saturate_i32(std::uint64_t(value) * 2194 / 131325);
            else if (field == 1)
                channel.sample.power = std::int64_t(signed24(value)) * 1203409000LL / 206104209;
            else {
                channel.sample.import_counter = counter_value(value);
                channel.counter_pending = true;
            }
            channel.received[field] = now_;
            channel.seen |= 1U << field;
        } else if (read_step_ == 3 * count_ + 1) {
            const auto frequency = value ? 10000000000ULL / value : 0;
            if (frequency && frequency <= UINT32_MAX) {
                frequency_ = static_cast<std::uint32_t>(frequency);
                frequency_at_ = now_;
                common_seen_ |= 2;
            } else
                common_seen_ &= ~2U;
        } else {
            voltage_ = std::uint64_t(value) * 1097 / 13162;
            voltage_at_ = now_;
            common_seen_ |= 1;
        }
    }

    void pump() {
        switch (stage_) {
        case Stage::idle:
            if (reached(now_, due_))
                queue();
            break;
        case Stage::transmit: {
            const auto result = serial::write_some(
                handle_, transmit_.data() + transmitted_, transmit_size_ - transmitted_);
            transmitted_ += result.count;
            if (result.result != serial::Result::success || reached(now_, deadline_))
                fail();
            else if (transmitted_ == transmit_size_)
                stage_ = Stage::drain;
            break;
        }
        case Stage::drain:
            if (serial::tx_idle(handle_)) {
                if (!setup_complete_) {
                    if (++setup_step_ == 8) {
                        setup_complete_ = true;
                        due_ = now_ + poll_interval_ms;
                    }
                    stage_ = Stage::idle;
                } else {
                    deadline_ = now_ + response_timeout_ms;
                    stage_ = Stage::response;
                }
            } else if (reached(now_, deadline_))
                fail();
            break;
        case Stage::response: {
            const auto result =
                serial::read(handle_, response_.data() + received_, response_.size() - received_);
            received_ += result.count;
            if (result.result != serial::Result::success) {
                fail();
                break;
            }
            if (received_ == response_.size()) {
                if (checksum(requested_register_, response_.data()) != response_[3]) {
                    fail();
                    break;
                }
                const std::uint32_t value = std::uint32_t(response_[0]) |
                                            (std::uint32_t(response_[1]) << 8) |
                                            (std::uint32_t(response_[2]) << 16);
                accept(value);
                advance_read();
                stage_ = Stage::idle;
            } else if (reached(now_, deadline_))
                fail();
            break;
        }
        case Stage::quiet:
            if (reached(now_, quiet_until_) && serial::tx_idle(handle_)) {
                if (serial::discard_rx(handle_) == serial::Result::success)
                    stage_ = Stage::idle;
            }
            break;
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies one configured phase's triplet-attempt age without ticking or renewing. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!channels_ || channel >= count_)
            return false;
        output = channels_[channel].observation_age.value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Marks native import totals handled by the source EnergyUpdateTotal core. */
    bool source_total_counter() const override { return true; }
#endif

    /** @brief Matches the source model's shared voltage/frequency topology. */
    bool common_voltage() const override { return true; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return true; }

    explicit Bl0906(const Configuration& configuration)
        : channels_(new (std::nothrow) Channel[configuration.channels]),
          count_(configuration.channels) {}

    ~Bl0906() override { (void)serial::request_close(handle_); }

    bool open(const Configuration& configuration) {
        if (!channels_ || !count_ || count_ > 6 || configuration.identity.bus ||
            configuration.identity.address || configuration.calibration.power ||
            configuration.calibration.voltage || configuration.calibration.current ||
            !set_options(configuration.options))
            return false;
        for (const auto& binding : configuration.identity.pins)
            if (binding.function != PinFunction::none && binding.function != PinFunction::uart_rx &&
                binding.function != PinFunction::uart_tx)
                return false;
        serial::Config uart{};
        uart.rx = configuration.identity.pin(PinFunction::uart_rx);
        uart.tx = configuration.identity.pin(PinFunction::uart_tx);
        uart.baud = 19200;
        if (!uart.rx.valid() || !uart.tx.valid() ||
            serial::open(uart, handle_) != serial::Result::success)
            return false;
        status_ = serial::status(handle_);
        now_ = due_ = platform::monotonic_milliseconds();
        for (unsigned channel = 0; channel < count_; ++channel) {
            channels_[channel].sample.use_absolute = true;
            channels_[channel].sample.available_fields = 0;
        }
        return true;
    }

    bool online() const override { return setup_complete_ && serial::active(handle_); }

    /** @brief The die temperature, while a reading is fresh. */
    bool chip_temperature(std::int32_t& millicelsius) const override {
        if (!(common_seen_ & 4) || !fresh(temperature_at_))
            return false;
        millicelsius = saturate_i32(temperature_);
        return true;
    }

    /** @brief Appends the chip temperature as the source does, in the shared temperature unit,
     * offset and resolution: "BL0906":{"Temperature":t}.
     */
    bool write_json_fields(CommandOutput& output) const override {
        std::int32_t temperature{};
        if (!chip_temperature(temperature))
            return true;
        return output.write(",\"BL0906\":{\"Temperature\":") &&
               sensor_service().write_value(output, SensorQuantity::temperature, temperature) &&
               output.write("}");
    }

    bool set_options(const Options& options) override {
        if (options.flags & ~1U || options.invalid_power_frames != 128)
            return false;
        exporting_ = options.flags & 1;
        return true;
    }

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        now_ = now;
#if USE_BERRY_ENERGY_API
        for (std::size_t c = 0; c < count_; ++c)
            (void)channels_[c].observation_age.poll(now, false);
#endif
        if (!serial::active(handle_)) {
            invalidate();
        } else {
            const auto status = serial::status(handle_);
            if (status.dropped != status_.dropped || status.errors != status_.errors) {
                invalidate();
                fail();
            }
            status_ = status;
            pump();
        }
        for (std::size_t i = 0; i < count && i < count_; ++i) {
            auto& channel = channels_[i];
            auto& sample = channel.sample;
            const bool voltage = (common_seen_ & 1) && fresh(voltage_at_);
            const bool current = (channel.seen & 1) && fresh(channel.received[0]);
            const bool power = (channel.seen & 2) && fresh(channel.received[1]);
            const bool counter = (channel.seen & 4) && fresh(channel.received[2]);
            sample.voltage = voltage_;
            sample.available_fields = (voltage ? 1 : 0) | (current ? 2 : 0) | (power ? 4 : 0);
            sample.valid = voltage && current && power && counter;
            sample.counter_fields = sample.valid && channel.counter_pending ? 1 : 0;
#if USE_ENERGY_HARDWARE_TOTAL
            sample.import_reference_valid = sample.counter_fields & 1;
            sample.import_reference = sample.import_counter;
#endif
            sample.import_counter_exporting = exporting_ && sample.power < 0;
            if (sample.counter_fields)
                channel.counter_pending = false;
            sample.auxiliary_fields = 0;
            if ((common_seen_ & 2) && fresh(frequency_at_)) {
                sample.frequency_millihz = frequency_;
                sample.auxiliary_fields |= 1;
            }
            if (!i && (common_seen_ & 4) && fresh(temperature_at_)) {
                sample.temperature_millicelsius = saturate_i32(temperature_);
                sample.auxiliary_fields |= 1U << 16;
            }
            output[i] = sample;
        }
    }
};
} // namespace

/** @brief Opens a separately selectable 1–6-channel BL0906 on an owned 19200-8N1 UART. */
std::unique_ptr<Device> create_bl0906(const Configuration& configuration) {
    auto meter = std::unique_ptr<Bl0906>(new (std::nothrow) Bl0906(configuration));
    if (!meter || !meter->open(configuration))
        return {};
    return meter;
}
} // namespace newmota::energy
