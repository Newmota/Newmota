/*
  cse7761.cpp - Portable CSE7761 Dual R3 and PowCT metering

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol/calibration adapted from xnrg_19_cse7761.ino.
  Copyright (C) 2021 Theo Arends

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file cse7761.cpp
 * @brief Runs verified UART initialization and independent channel calibration without RX waits.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "core/command/command.h"
#include "drivers/serial/serial_service.h"
#include "newmota/log.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::uint32_t frequency_reference = 3579545;
constexpr std::uint32_t fresh_ms = 3000;

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

enum class Wire : std::uint8_t { idle, transmit, drain, receive, quiet };

/** @brief One independent UART owner; only configured phase state is allocated.
 * OTP defaults deliberately use channel A coefficients for both phases, matching
 * the pinned source. Voltage/frequency conversion is common and uses phase one.
 */
class Cse7761 final : public Device {
    struct Channel {
        Calibration calibration{};
        Sample sample{};
        ObservationAge age{};
        std::int64_t energy_watts{};
        std::uint32_t energy_updates{};
        bool observed{};
        std::uint32_t current{}, power{}, factor{}, current_at{}, power_at{}, factor_at{};
        std::uint8_t seen{};
    };

    std::unique_ptr<Channel[]> channels_;
    PhysicalIdentity identity_{};
    serial::Handle serial_{};
    serial::Status serial_status_{};
    std::array<std::uint16_t, 8> coefficients_{52241, 0, 42563, 44513, 0, 0, 0, 0};
    std::array<std::uint8_t, 5> transmit_{}, response_{};
    std::size_t transmitted_{}, received_{}, transmit_size_{};
    std::uint32_t now_{}, due_{}, deadline_{}, close_at_{}, last_response_{}, quiet_until_{};
    std::uint32_t voltage_{}, frequency_{}, voltage_at_{}, frequency_at_{};
    std::uint8_t count_{}, init_step_{}, field_{}, requested_register_{}, expected_{}, attempts_{};
    Wire wire_{Wire::idle};
    bool powct_{}, ready_{}, voltage_seen_{}, frequency_seen_{};
    bool primary_controlled_{}, primary_on_{};

    bool fresh(std::uint32_t at) const { return now_ - at <= fresh_ms; }

    void invalidate() {
        if (!primary_controlled_)
            voltage_seen_ = frequency_seen_ = false;
        for (unsigned c = 0; c < count_; ++c) {
            channels_[c].sample.valid = false;
            if (!primary_controlled_)
                channels_[c].seen = 0;
        }
    }

    void restart() {
        invalidate();
        ready_ = false;
        init_step_ = 0;
        field_ = attempts_ = 0;
        due_ = now_ + 2000;
        close_at_ = now_ + 4000;
    }

    void failed() {
        wire_ = Wire::quiet;
        quiet_until_ = now_ + 10;
        if (++attempts_ >= 3) {
            attempts_ = 0;
            if (!ready_) {
                if (init_step_ >= 2 && init_step_ <= 10) {
                    accept(0); // Unreadable OTP participates in the source's checksum/default path.
                    return;
                }
                AddLog(LogLevel::info, "CSE7761: startup failed; retrying reset");
                restart();
            } else {
                if (primary_controlled_)
                    complete_field(); // Source ReadFallback keeps the previous raw register.
                else {
                    field_ = (field_ + 1) % (2 + count_ * (powct_ ? 3 : 2));
                    due_ = now_ + 10;
                }
            }
        }
    }

    void queue(std::uint8_t reg, std::uint32_t data, std::uint8_t read_size) {
        if (serial::discard_rx(serial_) != serial::Result::success) {
            failed();
            return;
        }
        requested_register_ = reg;
        expected_ = read_size;
        transmit_ = {0xa5, reg, 0, 0, 0};
        transmit_size_ = 2;
        if (!read_size) {
            if (data < 0xff)
                transmit_[transmit_size_++] = data;
            else {
                transmit_[transmit_size_++] = data >> 8;
                transmit_[transmit_size_++] = data;
            }
            std::uint8_t sum{};
            for (std::size_t i = 0; i < transmit_size_; ++i)
                sum += transmit_[i];
            transmit_[transmit_size_++] = static_cast<std::uint8_t>(~sum);
        }
        received_ = transmitted_ = 0;
        deadline_ = now_ + 100;
        wire_ = Wire::transmit;
    }

    void write_done() {
        attempts_ = 0;
        ++init_step_;
        if (init_step_ == 1)
            due_ = now_ + 1000;
        else if (init_step_ == 19) {
            ready_ = true;
            field_ = 0;
            due_ = now_;
            last_response_ = now_;
        }
    }

    void accept(std::uint32_t value) {
        attempts_ = 0;
        last_response_ = now_;
        if (!ready_) {
            if (init_step_ == 1) {
                if (value != 0x0a04) {
                    restart();
                    return;
                }
            } else if (init_step_ >= 2 && init_step_ < 10)
                coefficients_[init_step_ - 2] = value;
            else if (init_step_ == 10) {
                std::uint16_t sum = 0xffff;
                for (const auto coefficient : coefficients_)
                    sum += coefficient;
                sum = static_cast<std::uint16_t>(~sum);
                if (!sum || sum != value || !coefficients_[0] || !coefficients_[2] ||
                    !coefficients_[3]) {
                    coefficients_[0] = 52241;
                    coefficients_[2] = 42563;
                    coefficients_[3] = 44513;
                    AddLog(LogLevel::info,
                        "CSE7761: invalid OTP coefficient checksum; using defaults");
                }
            } else if (init_step_ == 12 && !(value & 0x10)) {
                restart();
                return;
            }
            ++init_step_;
            return;
        }
        if (!field_) {
            if (value || !voltage_seen_ || !voltage_) {
                voltage_ = value < 0x800000 ? value : 0;
                voltage_at_ = now_;
                voltage_seen_ = true;
            }
        } else if (field_ == 1) {
            if (value || !frequency_seen_ || !frequency_) {
                frequency_ = value < 0x8000 ? value : 0;
                frequency_at_ = now_;
                frequency_seen_ = true;
            }
        } else {
            const auto width = powct_ ? 3U : 2U;
            const auto channel = (field_ - 2) / width;
            const auto part = (field_ - 2) % width;
            auto& state = channels_[channel];
            if (powct_ && !part) {
                if (value || !(state.seen & 4) || !state.factor) {
                    state.factor = value;
                    state.factor_at = now_;
                    state.seen |= 4;
                }
            } else if (part == (powct_ ? 1U : 0U)) {
                if (value || !(state.seen & 1) || !state.current) {
                    state.current = value >= 0x800000 || value < 1600 ? 0 : value;
                    state.current_at = now_;
                    state.seen |= 1;
                }
            } else if (value || !(state.seen & 2) || !state.current || !state.power) {
                state.power = value;
                state.power_at = now_;
                state.seen |= 2;
            }
        }
        complete_field();
    }

    /** @brief A completed register scan is the source GetData publication/bucket boundary.
     * Exhausted primary reads use retained raw registers, matching ReadFallback; no partial
     * scan or repeated application poll adds a subsecond observation.
     */
    void complete_field() {
        if (++field_ == 2 + count_ * (powct_ ? 3 : 2)) {
            field_ = 0;
            due_ = now_ + 200;
            if (primary_controlled_)
                publish_primary();
        }
    }

    void next() {
        if (!ready_) {
            if (init_step_ == 0)
                queue(0xea, 0x96, 0);
            else if (init_step_ == 1)
                queue(0, 0, 2);
            else if (init_step_ < 10)
                queue(0x70 + init_step_ - 2, 0, 2);
            else if (init_step_ == 10)
                queue(0x6f, 0, 2);
            else if (init_step_ == 11)
                queue(0xea, 0xe5, 0);
            else if (init_step_ == 12)
                queue(0x43, 0, 1);
            else if (init_step_ == 13)
                queue(0x80, powct_ ? 0xfe00 : 0xff04, 0);
            else if (init_step_ == 14)
                queue(0x81, 0x1183, 0);
            else if (init_step_ == 15)
                queue(0x93, 0x0fe5, 0);
            else if (init_step_ == 16)
                queue(0x9d, 0x3290, 0);
            else if (init_step_ == 17) {
                if (reached(now_, close_at_))
                    ++init_step_;
            } else if (init_step_ == 18)
                queue(0xea, 0xdc, 0);
        } else {
            if (!field_)
                queue(0x26, 0, 3);
            else if (field_ == 1)
                queue(0x23, 0, 2);
            else {
                const auto width = powct_ ? 3U : 2U;
                const auto channel = (field_ - 2) / width;
                const auto part = (field_ - 2) % width;
                if (powct_ && !part)
                    queue(0x27, 0, 3);
                else if (part == (powct_ ? 1U : 0U))
                    queue(channel ? 0x25 : 0x24, 0, 3);
                else
                    queue(channel ? 0x2d : 0x2c, 0, 4);
            }
        }
    }

    void pump() {
        if (wire_ == Wire::idle) {
            if (!primary_controlled_ && ready_ && now_ - last_response_ > 6000)
                restart();
            if (reached(now_, due_))
                next();
        } else if (wire_ == Wire::transmit) {
            const auto result = serial::write_some(
                serial_, transmit_.data() + transmitted_, transmit_size_ - transmitted_);
            transmitted_ += result.count;
            if (result.result != serial::Result::success || reached(now_, deadline_))
                failed();
            else if (transmitted_ == transmit_size_)
                wire_ = Wire::drain;
        } else if (wire_ == Wire::drain) {
            if (serial::tx_idle(serial_)) {
                if (!expected_) {
                    write_done();
                    wire_ = Wire::idle;
                } else {
                    deadline_ = now_ + 6;
                    wire_ = Wire::receive;
                }
            } else if (reached(now_, deadline_))
                failed();
        } else if (wire_ == Wire::receive) {
            const auto result =
                serial::read(serial_, response_.data() + received_, expected_ + 1 - received_);
            received_ += result.count;
            if (result.result != serial::Result::success) {
                failed();
                return;
            }
            if (received_ == expected_ + 1U) {
                std::uint8_t sum = std::uint8_t(0xa5 + requested_register_);
                std::uint32_t value{};
                for (unsigned i = 0; i < expected_; ++i) {
                    sum += response_[i];
                    value = (value << 8) | response_[i];
                }
                if (response_[expected_] != static_cast<std::uint8_t>(~sum) ||
                    serial::available(serial_))
                    failed();
                else {
                    accept(value);
                    wire_ = Wire::idle;
                }
            } else if (reached(now_, deadline_))
                failed();
        } else if (reached(now_, quiet_until_) && serial::tx_idle(serial_)) {
            (void)serial::discard_rx(serial_);
            wire_ = Wire::idle;
        }
    }

    /** @brief Converts retained raw registers; physical freshness is separate from source
     * fallback publication. The service supplies the selected provider Power policy.
     */
    Sample converted(std::size_t c) {
        const auto voltage_calibration = channels_[0].calibration.voltage
                                             ? channels_[0].calibration.voltage
                                             : default_coefficient(CalibrationField::voltage);
        const auto frequency_calibration = channels_[0].calibration.frequency
                                               ? channels_[0].calibration.frequency
                                               : frequency_reference;
        auto& state = channels_[c];
        Sample sample{};
        const bool voltage = voltage_seen_ && fresh(voltage_at_);
        const bool current = (state.seen & 1) && fresh(state.current_at);
        const bool power = (state.seen & 2) && fresh(state.power_at);
        const bool factor =
            !powct_ || !state.current || ((state.seen & 4) && fresh(state.factor_at));
        const auto pc = state.calibration.power ? state.calibration.power
                                                : default_coefficient(CalibrationField::power);
        const auto ic = state.calibration.current ? state.calibration.current
                                                  : default_coefficient(CalibrationField::current);
        sample.voltage = saturate_i32(std::uint64_t(voltage_) * 1000 / voltage_calibration);
        const auto magnitude =
            state.power & 0x80000000U ? 0x100000000ULL - state.power : state.power;
        sample.power = state.current ? std::uint64_t(magnitude) * 1000 / pc : 0;
        if (powct_ && (state.factor & 0x800000U))
            sample.power = -sample.power;
        sample.current = sample.power ? saturate_i32(std::uint64_t(state.current) * 1000 / ic) : 0;
        sample.available_fields = (voltage ? 1 : 0) | (current ? 2 : 0) | (power && factor ? 4 : 0);
        sample.valid = ready_ && voltage && current && power && factor;
        sample.auxiliary_fields = 0;
        if (frequency_seen_ && (primary_controlled_ || fresh(frequency_at_))) {
            sample.frequency_millihz =
                frequency_ ? std::uint64_t(frequency_calibration) * 1000 / (8 * frequency_) : 0;
            sample.auxiliary_fields |= 1;
        }
        sample.apparent_mva = std::int64_t(sample.voltage / 1000) * sample.current +
                              std::int64_t(sample.voltage % 1000) * sample.current / 1000;
        sample.power_factor_milli =
            sample.apparent_mva
                ? static_cast<std::int32_t>(std::clamp(
                      static_cast<long double>(sample.power) * 1000 / sample.apparent_mva, -1000.0L,
                      1000.0L))
                : 0;
        sample.reactive_mvar = static_cast<std::int64_t>(std::sqrt(
            std::max(0.0L, static_cast<long double>(sample.apparent_mva) * sample.apparent_mva -
                               static_cast<long double>(sample.power) * sample.power)));
        sample.auxiliary_fields |= 2 | (1U << 4) | (1U << 5);
        return sample;
    }

    /** @brief ON scans renew source age and add nonzero power to the subsecond average.
     * OFF scans leave published arrays and existing energy buckets untouched.
     */
    void publish_primary() {
        if (!primary_on_)
            return;
        for (std::size_t c = 0; c < count_; ++c) {
            auto& state = channels_[c];
            state.sample = converted(c);
            state.sample.available_fields = 7;
            state.sample.auxiliary_fields |= 1;
            state.sample.source_retained = !state.sample.valid;
            state.observed = true;
            if (state.current && state.power) {
                /** Source energy[] is signed whole watts: compound-add truncates each scan.
                 * Use the raw/coefficient ratio before typed mW quantization, including nonzero
                 * sub-mW observations in the average count. Preserve truncation across zero.
                 */
                const auto pc = state.calibration.power
                                    ? state.calibration.power
                                    : default_coefficient(CalibrationField::power);
                auto raw = std::int64_t(
                    state.power & 0x80000000U ? 0x100000000ULL - state.power : state.power);
                if (powct_ && (state.factor & 0x800000U))
                    raw = -raw;
                state.energy_watts = (state.energy_watts * pc + raw) / pc;
                ++state.energy_updates;
            }
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies the existing source age without changing watchdog or accounting state. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!primary_controlled_ || !channels_ || channel >= count_)
            return false;
        output = channels_[channel].age.value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    /** @brief Source Cse7761EverySecond calls EnergyUpdateToday after each channel. */
    std::uint8_t accounting_update_group(std::size_t channel) const override {
        return static_cast<std::uint8_t>(channel);
    }

    /** @brief Matches the source model's shared voltage/frequency topology. */
    bool common_voltage() const override { return true; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return true; }

    /** @brief Restores the pinned model's runtime local-export reporting default. */
    bool export_reporting_default() const override { return powct_; }

    Cse7761(const Configuration& config, bool powct)
        : channels_(new (std::nothrow) Channel[config.channels]), identity_(config.identity),
          count_(config.channels), powct_(powct) {}

    ~Cse7761() override { (void)serial::request_close(serial_); }

    bool open(const Configuration& config) {
        if (!channels_ || count_ != (powct_ ? 1 : 2) || identity_.bus || identity_.address ||
            !calibrate(config.calibration) || !set_options(config.options))
            return false;
        for (const auto& pin : identity_.pins)
            if (pin.function != PinFunction::none && pin.function != PinFunction::uart_rx &&
                pin.function != PinFunction::uart_tx)
                return false;
        serial::Config uart{};
        uart.rx = identity_.pin(PinFunction::uart_rx);
        uart.tx = identity_.pin(PinFunction::uart_tx);
        uart.baud = 38400;
        uart.parity = platform::serial::Parity::even;
        if (!uart.rx.valid() || !uart.tx.valid() ||
            serial::open(uart, serial_) != serial::Result::success)
            return false;
        now_ = platform::monotonic_milliseconds();
        serial_status_ = serial::status(serial_);
        restart();
        return true;
    }

    bool online() const override { return ready_ && serial::active(serial_); }

    bool independent_calibration() const override { return true; }

    bool coefficient_reset_alias() const override { return true; }

    std::uint32_t current_reference_scale() const override { return 100; }

    std::uint32_t default_coefficient(CalibrationField field) const override {
        const auto scale = powct_ ? 5U : 1U;
        switch (field) {
        case CalibrationField::power:
            return 0x80000000U /
                   (std::uint32_t(coefficients_[3] ? coefficients_[3] : 44513) * scale);
        case CalibrationField::voltage:
            return 0x400000U * 100 / (coefficients_[2] ? coefficients_[2] : 42563);
        case CalibrationField::current:
            return (0x800000U * 100 /
                       (std::uint32_t(coefficients_[0] ? coefficients_[0] : 52241) * scale)) *
                   10;
        case CalibrationField::frequency:
            return frequency_reference;
        }
        return 0;
    }

    bool calibrate(const Calibration& value) override {
        if (!channels_ || !valid_calibration(value))
            return false;
        for (unsigned c = 0; c < count_; ++c)
            channels_[c].calibration = value;
        return true;
    }

    static bool valid_calibration(const Calibration& value) {
        return value.power <= INT32_MAX && value.voltage <= INT32_MAX &&
               value.current <= INT32_MAX && value.frequency <= INT32_MAX;
    }

    bool calibrate_channel(std::size_t channel, const Calibration& value) override {
        if (channel >= count_ || !valid_calibration(value))
            return false;
        channels_[channel].calibration = value;
        return true;
    }

    bool set_options(const Options& options) override {
        return !options.flags && options.invalid_power_frames == 128;
    }

    bool calibration_for_channel(std::size_t channel, CalibrationField field,
        std::uint32_t reference, Calibration& output) const override {
        if (channel >= count_ || !ready_ ||
            (!primary_controlled_ && !channels_[channel].sample.valid))
            return false;
        const auto& state = channels_[channel];
        std::uint64_t coefficient{};
        if (field == CalibrationField::power && reference > 1000 && reference < 20000000 &&
            state.power && (!primary_controlled_ || state.current) && fresh(state.power_at)) {
            const auto magnitude = state.power & 0x80000000U
                                       ? std::uint64_t(0x100000000ULL - state.power)
                                       : state.power;
            coefficient = (magnitude / (reference / 10)) * 100;
        } else if (field == CalibrationField::voltage && reference > 100000 && reference < 400000 &&
                   voltage_ && fresh(voltage_at_))
            coefficient = std::uint64_t(voltage_) * 100 / (reference / 10);
        else if (field == CalibrationField::current && reference > 1000 && reference < 10000000 &&
                 state.current && fresh(state.current_at))
            coefficient = (std::uint64_t(state.current) * 100 / reference) * 1000;
        else if (field == CalibrationField::frequency && reference > 45000 && reference < 65000 &&
                 frequency_ && fresh(frequency_at_))
            coefficient = std::uint64_t(frequency_) * 8 * (reference / 10) / 100;
        else
            return false;
        if (!coefficient || coefficient > INT32_MAX)
            return false;
        if (field == CalibrationField::power)
            output.power = coefficient;
        else if (field == CalibrationField::voltage)
            output.voltage = coefficient;
        else if (field == CalibrationField::current)
            output.current = coefficient;
        else
            output.frequency = coefficient;
        return true;
    }

#if USE_ENERGY_POWER_POLICY
    /** @brief Primary source gate controls publication/observation, never raw UART reception. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        now_ = now;
        for (unsigned c = 0; c < count_; ++c)
            channels_[c].observed = false;
        const auto status = serial::status(serial_);
        if (!serial::active(serial_))
            invalidate();
        else {
            if (status.dropped != serial_status_.dropped ||
                status.errors != serial_status_.errors) {
                invalidate();
                failed();
            }
            serial_status_ = status;
            pump();
        }
        for (std::size_t c = 0; c < count && c < count_; ++c) {
            auto& state = channels_[c];
            if (!primary_controlled_) {
                state.sample = converted(c);
                output[c] = state.sample;
                continue;
            }
            const bool second = state.age.poll(now_, state.observed);
            if (state.age.expired_now()) {
                state.sample.voltage = state.sample.current = state.sample.power = 0;
                state.sample.frequency_millihz = 0;
                state.sample.apparent_mva = state.sample.reactive_mvar =
                    state.sample.power_factor_milli = 0;
                state.sample.valid = false;
                state.sample.watchdog_reset = true;
                state.sample.source_retained = false;
            }
            auto sample = state.sample;
            sample.available_fields = 7;
            sample.auxiliary_fields |= 1;
            sample.use_absolute = true;
            if (!sample.valid && !sample.watchdog_reset)
                sample.source_retained = true;
            /** The source drains already observed ON buckets even if the gate is now OFF.
             * Each channel stays a distinct accounting-update group; missed seconds are not
             * replayed and no generic elapsed-power integration can duplicate this increment.
             */
            if (second && ready_ && state.energy_updates) {
                const bool exporting = state.energy_watts < 0;
                const auto watts = exporting ? std::uint64_t(-(state.energy_watts + 1)) + 1
                                             : std::uint64_t(state.energy_watts);
                const auto increment = (watts * 1000 / state.energy_updates / 36) * 10;
                if (exporting)
                    sample.export_increment = increment;
                else
                    sample.import_increment = increment;
                sample.increment_fields = exporting ? 2 : 1;
                state.energy_watts = 0;
                state.energy_updates = 0;
            }
            output[c] = sample;
        }
    }
};

std::unique_ptr<Device> create(const Configuration& config, bool powct) {
    auto device = std::unique_ptr<Cse7761>(new (std::nothrow) Cse7761(config, powct));
    if (!device || !device->open(config))
        return {};
    return device;
}
} // namespace
#if USE_CSE7761
/** @brief Creates a Dual R3 owner with two independently calibrated shunt inputs. */
std::unique_ptr<Device> create_cse7761(const Configuration& config) {
    return create(config, false);
}
#endif
#if USE_CSE7761_POWCT
/** @brief Creates a PowCT owner with signed CT power and export accounting. */
std::unique_ptr<Device> create_cse7761_powct(const Configuration& config) {
    return create(config, true);
}
#endif
} // namespace newmota::energy
