/*
  ade7880.cpp - Portable ADE7880 three-phase metrology and recovery

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Adapted protocol and calibration behavior:
  xnrg_23_ade7880.ino - ADE7880 energy sensor support for Tasmota
  SPDX-FileCopyrightText: 2022 Theo Arends and AndreKR
  SPDX-License-Identifier: GPL-3.0-only
*/

/** @file ade7880.cpp
 * @brief Services line-cycle interrupts through cooperative verified I2C transactions.
 */
#include "services/energy/energy_service.h"
#include "core/command/command.h"
#include "core/hardware/runtime_pin_claims.h"
#include "core/storage/filesystem.h"
#include "drivers/gpio/edge_capture_service.h"
#include "drivers/i2c/i2c_device_claims.h"
#include "newmota/log.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/i2c.h"
#include "newmota/platform/platform.h"
#include "support/integer_object.h"
#include <algorithm>
#include <array>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::uint8_t address = 0x38;
constexpr std::uint16_t status0 = 0xe502, status1 = 0xe503;
constexpr std::uint16_t last_op = 0xea01, last_address = 0xe9fe;

/** @brief Neutral chip corrections; physical front-end gains/phase must be supplied per meter. */
struct Registers {
    std::array<std::int32_t, 4> current{};
    std::array<std::int32_t, 3> voltage{};
    std::array<std::int32_t, 3> power{};
    std::array<std::uint16_t, 3> angle{};
    bool sixty_hz{};
};

/** @brief Validates into a caller-staged register set, preserving omitted values. */
bool parse_registers(std::string_view text, Registers& output) {
    auto visitor = [&](const std::array<std::string_view, 3>& path, unsigned depth,
                       std::int32_t value) {
        if (depth == 0 && path[0] == "freq") {
            if (value < 0 || value > 1)
                return false;
            output.sixty_hz = value;
        }
        if (depth == 1 && path[0] == "rms" && path[1] == "current_s")
            output.current[3] = value;
        for (unsigned phase = 0; phase < 3; ++phase) {
            const auto letter = static_cast<char>('a' + phase);
            if (depth == 1 && path[0] == "rms" && path[1].size() == 9 && path[1][8] == letter) {
                if (path[1].substr(0, 8) == "current_")
                    output.current[phase] = value;
                else if (path[1].substr(0, 8) == "voltage_")
                    output.voltage[phase] = value;
            }
            if (depth == 1 && path[0] == "angles" && path[1].size() == 6 &&
                path[1].substr(0, 5) == "angle" && path[1][5] == static_cast<char>('0' + phase)) {
                // The pinned driver verifies only the low byte of PHCAL.
                if (value < 0 || value > 255)
                    return false;
                output.angle[phase] = static_cast<std::uint16_t>(value);
            }
            if (depth == 2 && path[0] == "powers" && path[1] == "totactive" &&
                path[2].size() == 1 && path[2][0] == letter)
                output.power[phase] = value;
        }
        return true;
    };
    return text.size() <= 2048 &&
           support::IntegerObjectParser<decltype(visitor)>(text, visitor).parse();
}

Registers defaults(std::uint8_t bus) {
    Registers result{};
    auto text = std::unique_ptr<char[]>(new (std::nothrow) char[2048]);
    if (!text) {
        AddLog(LogLevel::info, "ADE7880: calib.dat allocation failed; using defaults");
        return result;
    }
    std::size_t size{};
    // A bus-specific file allows two independently calibrated meters to restart together.
    char path[] = "/ade7880-1.dat";
    path[9] = static_cast<char>('0' + bus);
    auto read = filesystem::read_file(path, text.get(), 2048, size);
    if (read == filesystem::ReadResult::not_found)
        read = filesystem::read_file("/calib.dat", text.get(), 2048, size);
    if (read == filesystem::ReadResult::not_found)
        return result;
    auto updated = result;
    if (read != filesystem::ReadResult::succeeded || !parse_registers({text.get(), size}, updated))
        AddLog(LogLevel::info, "ADE7880: calib.dat unavailable or invalid; using defaults");
    else
        result = updated;
    return result;
}

unsigned register_size(std::uint16_t reg) {
    switch ((reg >> 8) & 15) {
    case 3:
    case 4:
    case 5:
    case 8:
        return 4;
    case 1:
    case 2:
    case 6:
    case 9:
        return 2;
    case 0:
    case 7:
    case 10:
    case 11:
    case 12:
        return 1;
    default:
        return 0;
    }
}

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}
enum class Stage : std::uint8_t { reset, release_reset, reset_done, initialize, running, retry };

/** @brief One physical ADE7880, three phase accounts and one shared neutral reading.
 * Interrupts only update the shared capture owner. Each poll performs at most one
 * verified protocol operation; GPIO reset and startup waits are deadline driven.
 */
class Ade7880 final : public Device {
    PhysicalIdentity identity_{};
    Registers registers_{};
    std::array<Sample, 3> samples_{};
    i2c_devices::Lease lease_{};
    edges::Handle irq_{};
    runtime_pins::Owner gpio_owner_{};
    std::uint32_t edges_seen_{};
    std::uint64_t missed_cycles_{}, discarded_cycles_{};
    std::uint32_t deadline_{}, last_cycle_{}, received_at_{}, cycle_started_{};
    std::uint8_t bus_{}, init_step_{}, cycle_step_{}, skip_cycles_{2};
    std::uint32_t cycle_budget_ms_{850};
    Stage stage_{Stage::reset};
    ControlStatus control_{};
    std::array<std::array<std::uint32_t, 6>, 3> raw_{};
    std::uint32_t neutral_{};
    bool gpio_claimed_{}, cycle_active_{};

    bool read(std::uint16_t reg, std::uint32_t& output) {
        const auto size = register_size(reg);
        const std::uint8_t tx[]{std::uint8_t(reg >> 8), std::uint8_t(reg)};
        std::array<std::uint8_t, 4> rx{};
        if (!size || platform::i2c::transfer(bus_, address, tx, 2, rx.data(), size) !=
                         platform::i2c::Result::success)
            return false;
        output = 0;
        for (unsigned i = 0; i < size; ++i)
            output = (output << 8) | rx[i];
        return true;
    }

    bool verify(std::uint16_t reg, std::uint8_t operation) {
        std::uint32_t op{}, accessed{};
        return read(last_op, op) && read(last_address, accessed) && op == operation &&
               accessed == reg;
    }

    bool read_verified(std::uint16_t reg, std::uint32_t& output) {
        return read(reg, output) && verify(reg, 0x35);
    }

    bool write(std::uint16_t reg, std::uint32_t value, bool verified = true) {
        const auto size = register_size(reg);
        std::array<std::uint8_t, 6> tx{};
        tx[0] = reg >> 8;
        tx[1] = reg;
        for (unsigned i = 0; i < size; ++i)
            tx[2 + i] = value >> (8 * (size - i - 1));
        if (!size || platform::i2c::transfer(bus_, address, tx.data(), 2 + size, nullptr, 0) !=
                         platform::i2c::Result::success)
            return false;
        platform::delay_microseconds(5);
        return !verified || verify(reg, 0xca);
    }

    bool write_checked(std::uint16_t reg, std::uint32_t value, std::uint32_t mask) {
        std::uint32_t actual{};
        return write(reg, value) && read_verified(reg, actual) && actual == (value & mask);
    }

    void invalidate() {
        for (auto& sample : samples_) {
            sample.valid = false;
            sample.available_fields = 0;
        }
    }

    void retry(std::uint32_t now) {
        invalidate();
        cycle_active_ = false;
        stage_ = Stage::retry;
        deadline_ = now + 1000;
        if (control_.phase == ControlPhase::queued)
            control_.phase = ControlPhase::failed;
        AddLog(LogLevel::info, "ADE7880: initialization or communication failed; retrying reset");
    }

    /** @brief Applies one step of the verified DSP startup/calibration sequence. */
    bool initialize_step() {
        if (init_step_ >= 4 && init_step_ < 16) {
            const auto phase = (init_step_ - 4) / 4;
            switch ((init_step_ - 4) % 4) {
            case 0:
                return write_checked(0x4381 + phase * 2, registers_.voltage[phase], 0x0fffffff);
            case 1:
                return write_checked(0x4380 + phase * 2, registers_.current[phase], 0x0fffffff);
            case 2:
                return write_checked(0x4389 + phase * 2, registers_.power[phase], 0x0fffffff);
            default:
                return write_checked(0xe614 + phase, registers_.angle[phase], 0xff);
            }
        }
        switch (init_step_) {
        case 0:
            return write(0xec01, 2); // Lock I2C interface.
        case 1:
            return write(status1, 0x3ffe8930); // Acknowledge RSTDONE.
        case 2:
            return write(0xe60f, 0); // PGA gain one.
        case 3:
            return write(0xe60e, registers_.sixty_hz ? 0x41ff : 0x01ff);
        case 16:
        case 17:
        case 18:
            // Last DSP RAM address must be written three times before protection.
            return write_checked(0x4386, registers_.current[3], 0x0fffffff);
        case 19:
            return write_checked(0xe702, 0x09, 0xff);
        case 20:
            return write_checked(0xe60c, registers_.sixty_hz ? 120 : 100, 0xffff);
        case 21:
            return write_checked(0xe50a, 0x20, UINT32_MAX);
        case 22:
            return write(0xe50a, 0x20, false);
        case 23:
            return write(0xe7fe, 0xad, false);
        case 24:
            return write(0xe7e3, 0x80, false);
        case 25:
            return write(0xe228, 0x0201);
        default:
            return false;
        }
    }

    /** @brief Publishes signed line-cycle energy once, with exact fractional micro-Wh carry. */
    void publish(std::uint32_t now) {
        for (unsigned phase = 0; phase < 3; ++phase) {
            const auto& raw = raw_[phase];
            auto& sample = samples_[phase];
            sample.voltage = raw[0] / 10;  // raw/10000 V -> milli-V.
            sample.current = raw[1] / 100; // raw/100000 A -> milli-A.
            sample.power = std::int64_t(static_cast<std::int32_t>(raw[2])) * 10;
            sample.apparent_mva = std::int64_t(static_cast<std::int32_t>(raw[3])) * 10;
            sample.frequency_millihz = raw[4] ? 256000000U / raw[4] : 0;
            sample.power_factor_milli =
                sample.apparent_mva
                    ? static_cast<std::int32_t>(std::clamp(
                          static_cast<long double>(sample.power) * 1000 / sample.apparent_mva,
                          -1000.0L, 1000.0L))
                    : 0;
            sample.auxiliary_fields = 1 | 2 | (1U << 4);
            if (!phase) {
                sample.neutral_current_ma = neutral_ / 100;
                sample.auxiliary_fields |= 1U << 15;
            }
            const std::int64_t energy = static_cast<std::int32_t>(raw[5]);
            const auto magnitude = energy < 0 ? -energy : energy;
            if (energy < 0)
                sample.export_increment = std::uint64_t(magnitude) * 1024;
            else
                sample.import_increment = std::uint64_t(magnitude) * 1024;
            sample.increment_denominator = 15; // 24576 * 10 / 3600 micro-Wh per signed LSB.
            sample.increment_fields = energy < 0 ? 2 : 1;
            sample.available_fields = 7;
            sample.valid = raw[4] != 0;
        }
        received_at_ = now;
    }

    /** @brief Reads one verified register per poll, acknowledging the cycle interrupt first. */
    bool cycle_step(std::uint32_t now) {
        if (!cycle_step_) {
            std::uint32_t status{};
            if (!read_verified(status0, status) || !(status & 0x20))
                return false;
        } else if (cycle_step_ == 1) {
            if (!write(status0, 0x20))
                return false;
            if (skip_cycles_) {
                --skip_cycles_;
                cycle_active_ = false;
            }
        } else if (cycle_step_ == 2) {
            if (!read_verified(0x43c6, neutral_))
                return false;
        } else {
            const auto phase = (cycle_step_ - 3) / 6;
            const auto field = (cycle_step_ - 3) % 6;
            const std::uint16_t registers[]{std::uint16_t(0x43c1 + phase * 2),
                std::uint16_t(0x43c0 + phase * 2), std::uint16_t(0xe513 + phase),
                std::uint16_t(0xe519 + phase), std::uint16_t(0xe905 + phase),
                std::uint16_t(0xe400 + phase)};
            if (!read_verified(registers[field], raw_[phase][field]))
                return false;
            if (cycle_step_ == 20) {
                publish(now);
                cycle_active_ = false;
            }
        }
        ++cycle_step_;
        return true;
    }

    void advance(std::uint32_t now) {
        const auto reset = identity_.pin(PinFunction::reset);
        if (stage_ == Stage::retry) {
            if (reached(now, deadline_))
                stage_ = Stage::reset;
            return;
        }
        if (stage_ == Stage::reset) {
            invalidate();
            cycle_active_ = false;
            skip_cycles_ = 2;
            if (reset.valid() &&
                platform::gpio_configure_output(reset, false) != platform::GpioResult::success) {
                retry(now);
                return;
            }
            deadline_ = now + 1;
            stage_ = Stage::release_reset;
        } else if (stage_ == Stage::release_reset && reached(now, deadline_)) {
            if (reset.valid() &&
                (platform::gpio_write(reset, true) != platform::GpioResult::success ||
                    platform::gpio_configure_input(reset, platform::GpioPull::none) !=
                        platform::GpioResult::success)) {
                retry(now);
                return;
            }
            deadline_ = now + 100;
            stage_ = Stage::reset_done;
        } else if (stage_ == Stage::reset_done) {
            bool high{};
            if (platform::gpio_read(identity_.pin(PinFunction::interrupt2), high) !=
                platform::GpioResult::success) {
                retry(now);
                return;
            }
            if (!high) {
                std::uint32_t status{};
                if (read_verified(status1, status) && (status & 0x8000)) {
                    init_step_ = 0;
                    stage_ = Stage::initialize;
                    return;
                }
            }
            if (reached(now, deadline_))
                retry(now);
        } else if (stage_ == Stage::initialize) {
            if (!initialize_step()) {
                retry(now);
                return;
            }
            if (++init_step_ == 26) {
                edges::Snapshot snapshot{};
                if (edges::snapshot(irq_, snapshot) != edges::Result::success ||
                    snapshot.overflow) {
                    retry(now);
                    return;
                }
                edges_seen_ = snapshot.count;
                last_cycle_ = now;
                stage_ = Stage::running;
                if (control_.phase == ControlPhase::queued)
                    control_.phase = ControlPhase::succeeded;
            }
        } else if (stage_ == Stage::running) {
            if (now - last_cycle_ > 6000) {
                AddLog(LogLevel::info, "ADE7880: interrupt watchdog reset");
                stage_ = Stage::reset;
                invalidate();
                return;
            }
            if (cycle_active_) {
                edges::Snapshot snapshot{};
                if (now - cycle_started_ > cycle_budget_ms_ ||
                    edges::snapshot(irq_, snapshot) != edges::Result::success ||
                    snapshot.overflow || snapshot.count != edges_seen_) {
                    ++discarded_cycles_;
                    AddLog(LogLevel::info, "ADE7880: incomplete cycle exceeded acquisition window");
                    retry(now);
                    return;
                }
            }
            if (!cycle_active_) {
                edges::Snapshot snapshot{};
                if (edges::snapshot(irq_, snapshot) != edges::Result::success ||
                    snapshot.overflow) {
                    retry(now);
                    return;
                }
                if (snapshot.count == edges_seen_)
                    return;
                const auto clock = platform::edge_capture::monotonic_microseconds();
                const auto age =
                    clock >= snapshot.last_edge_us ? clock - snapshot.last_edge_us : UINT64_MAX;
                if (!snapshot.seen || age >= 850000) {
                    // IRQ0 remains asserted until acknowledged, so edge counts alone cannot
                    // reveal multiple line-cycle overwrites while the app was stalled.
                    ++discarded_cycles_;
                    AddLog(
                        LogLevel::info, "ADE7880: stale latched IRQ; energy intervals may be lost");
                    retry(now);
                    return;
                }
                cycle_budget_ms_ = 850 - static_cast<std::uint32_t>(age / 1000);
                if (snapshot.count - edges_seen_ > 1) {
                    missed_cycles_ += snapshot.count - edges_seen_ - 1;
                    AddLog(
                        LogLevel::info, "ADE7880: missed line cycles; energy interval unavailable");
                }
                edges_seen_ = snapshot.count;
                last_cycle_ = now;
                cycle_started_ = now;
                cycle_step_ = 0;
                cycle_active_ = true;
            }
            if (!cycle_step(now))
                retry(now);
        }
    }

  public:
    /** @brief Restores the pinned model's runtime local-export reporting default. */
    bool export_reporting_default() const override { return true; }

    explicit Ade7880(const Configuration& configuration)
        : identity_(configuration.identity), registers_(defaults(configuration.identity.bus)),
          bus_(identity_.bus - 1) {
        for (auto& sample : samples_) {
            sample.available_fields = 0;
            sample.use_absolute = true;
        }
    }

    ~Ade7880() override {
        (void)edges::close(irq_); // Failed native cleanup retains its own pin reservation.
        if (gpio_claimed_) {
            bool clean = true;
            for (const auto role : {PinFunction::interrupt2, PinFunction::reset}) {
                const auto pin = identity_.pin(role);
                if (pin.valid() && platform::gpio_configure_input(pin, platform::GpioPull::none) !=
                                       platform::GpioResult::success)
                    clean = false;
            }
            if (clean)
                runtime_pins::release(gpio_owner_);
        }
        i2c_devices::release(lease_);
    }

    bool open(const Configuration& configuration) {
        if (configuration.channels != 3 || identity_.address != address || !identity_.bus ||
            identity_.bus > platform::i2c::kBusCount || configuration.calibration.power ||
            configuration.calibration.voltage || configuration.calibration.current ||
            configuration.options.flags || configuration.options.invalid_power_frames != 128 ||
            !identity_.pin(PinFunction::interrupt).valid() ||
            !identity_.pin(PinFunction::interrupt2).valid())
            return false;
        for (const auto& binding : identity_.pins)
            if (binding.function != PinFunction::none &&
                binding.function != PinFunction::interrupt &&
                binding.function != PinFunction::interrupt2 &&
                binding.function != PinFunction::reset)
                return false;
        if (!i2c_devices::claim(bus_, address, lease_))
            return false;
        std::array<PinId, 2> pins{
            identity_.pin(PinFunction::interrupt2), identity_.pin(PinFunction::reset)};
        gpio_owner_ = runtime_pins::allocate_energy_owner();
        if (!runtime_pins::claim(gpio_owner_, pins.data(), pins[1].valid() ? 2 : 1))
            return false;
        gpio_claimed_ = true;
        if (platform::gpio_configure_input(pins[0], platform::GpioPull::none) !=
            platform::GpioResult::success)
            return false;
        edges::Configuration capture{};
        capture.pin = identity_.pin(PinFunction::interrupt);
        capture.edge = platform::edge_capture::Edge::falling;
        return edges::open(capture, irq_) == edges::Result::success;
    }

    bool online() const override { return stage_ == Stage::running; }

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        for (auto& sample : samples_) {
            sample.increment_fields = 0;
            sample.import_increment = sample.export_increment = 0;
            if (now - received_at_ > 3000)
                sample.valid = false;
        }
        advance(now);
        for (std::size_t i = 0; i < count && i < samples_.size(); ++i)
            output[i] = samples_[i];
    }

    bool configure_registers(std::string_view payload) override {
        auto updated = defaults(
            identity_.bus); // Upstream EnergyConfig reloads defaults/file for every update.
        if (!parse_registers(payload, updated))
            return false;
        registers_ = updated;
        stage_ = Stage::reset;
        invalidate();
        control_ = {};
        control_.operation = Control::calibration;
        control_.phase = ControlPhase::queued;
        return true;
    }

    bool write_registers(CommandOutput& output) const override {
        return output.format(
            "{\"EnergyConfig\":{\"rms\":{\"current_a\":%ld,\"current_b\":%ld,\"current_c\":"
            "%ld,\"current_s\":%ld,\"voltage_a\":%ld,\"voltage_b\":%ld,\"voltage_c\":%ld},"
            "\"angles\":{\"angle0\":%u,\"angle1\":%u,\"angle2\":%u},\"powers\":{\"totactive\":{"
            "\"a\":%ld,\"b\":%ld,\"c\":%ld}},\"freq\":%u}}",
            static_cast<long>(registers_.current[0]), static_cast<long>(registers_.current[1]),
            static_cast<long>(registers_.current[2]), static_cast<long>(registers_.current[3]),
            static_cast<long>(registers_.voltage[0]), static_cast<long>(registers_.voltage[1]),
            static_cast<long>(registers_.voltage[2]), unsigned(registers_.angle[0]),
            unsigned(registers_.angle[1]), unsigned(registers_.angle[2]),
            static_cast<long>(registers_.power[0]), static_cast<long>(registers_.power[1]),
            static_cast<long>(registers_.power[2]), unsigned(registers_.sixty_hz));
    }

    ControlStatus control_status() const override { return control_; }
};
} // namespace

/** @brief Creates an independently configured ADE7880 with three phase accounts. */
std::unique_ptr<Device> create_ade7880(const Configuration& configuration) {
    auto meter = std::unique_ptr<Ade7880>(new (std::nothrow) Ade7880(configuration));
    if (!meter || !meter->open(configuration))
        return {};
    return meter;
}
} // namespace newmota::energy
