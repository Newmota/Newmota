/*
  cse7766.cpp - Portable CSE7759/CSE7766/HLW8032 UART frame owner

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol conversion reference: Tasmota xnrg_02_cse7766.ino
  Copyright (C) 2021 Theo Arends

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file cse7766.cpp
 * @brief Portable CSE7759/CSE7766/HLW8032 UART frame owner.
 */

#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "drivers/serial/serial_service.h"
#include "newmota/log.h"
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace newmota::energy {
namespace {
/** @brief Decodes the common checksum-valid CSE7759/CSE7766/HLW8032 frame over an exclusively owned
 * UART. */
class Cse7766 final : public Device {
  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies the existing source age without changing watchdog or accounting state. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!primary_controlled_ || channel)
            return false;
        output = primary_age_.value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return true; }
#endif

    bool open(const Configuration& configuration) {
        if (configuration.identity.bus || !calibrate(configuration.calibration) ||
            !set_options(configuration.options))
            return false;
        serial::Config config{};
        config.rx = configuration.identity.pin(PinFunction::uart_rx);
        config.baud = 4800;
        config.parity = platform::serial::Parity::even;
        retained_.available_fields = 0;
        invalid_power_ = options_.invalid_power_frames; // Source startup begins at the retry limit.
        return serial::open(config, handle_) == serial::Result::success;
    }

    ~Cse7766() override { (void)serial::request_close(handle_); }

    /** @brief Accepts native positive signed-32 P/V/I coefficients through primary Cal aliases.
     * Frequency retains the common parser bound; this family has no frequency calibration.
     */
    std::uint32_t maximum_coefficient(CalibrationField field) const override {
        return field == CalibrationField::frequency ? Device::maximum_coefficient(field)
                                                    : INT32_MAX;
    }

    bool calibrate(const Calibration& calibration) override {
        if (calibration.power > INT32_MAX || calibration.voltage > INT32_MAX ||
            calibration.current > INT32_MAX || pending_pulses_)
            return false;
        calibration_ = calibration;
        learned_ = false;
        if (primary_controlled_)
            power_coefficient_ = calibration_.power ? std::uint64_t(calibration_.power) * 1000
                                                    : hardware_power_coefficient_;
        /** Source Cal edits do not erase an already qualified primary power cycle. */
        if (!primary_controlled_)
            first_power_cycle_ = 0;
        return true;
    }

    /** @brief Reports the first valid chip/default triplet already applied to this instance.
     * Source settings latch coefficients once; later frames cannot silently reprice CF energy.
     */
    bool learned_calibration(Calibration& value) const override {
        if (!learned_)
            return false;
        value = calibration_;
        return true;
    }

    /** @brief Applies source global retry policy only to the service-selected primary meter.
     * First binding seeds the startup retry counter; later changes preserve retry progress.
     */
    void set_primary_invalid_power(std::uint8_t value) override {
        if (!primary_threshold_applied_) {
            invalid_power_ = value;
            primary_threshold_applied_ = true;
        }
        options_.invalid_power_frames = value;
    }

    bool set_options(const Options& options) override {
        if (options.flags || !options.invalid_power_frames || options.invalid_power_frames > 255)
            return false;
        options_ = options;
        return true;
    }

    std::uint32_t current_reference_scale() const override { return 1000; }

    bool calibration_for(
        CalibrationField field, std::uint32_t reference, Calibration& calibration) const override {
        if (!retained_.valid)
            return false;
        std::uint64_t value{};
        if (field == CalibrationField::power && power_cycle_ && (reference_fields_ & 4))
            value = std::uint64_t(reference) * power_cycle_ / 1000000;
        else if (field == CalibrationField::voltage && voltage_cycle_ && (reference_fields_ & 1))
            value = std::uint64_t(reference) * voltage_cycle_ / 100000;
        else if (field == CalibrationField::current && current_cycle_ && (reference_fields_ & 2))
            value = std::uint64_t(reference) * current_cycle_ / 1000000;
        else
            return false;
        if (!value || value > INT32_MAX)
            return false;
        if (field == CalibrationField::power)
            calibration.power = value;
        else if (field == CalibrationField::voltage)
            calibration.voltage = value;
        else
            calibration.current = value;
        return true;
    }

#if USE_ENERGY_POWER_POLICY
    /** @brief Applies the source primary output gate; raw CF snapshots remain observable OFF. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
        primary_observed_ = false;
        counter_ready_ = false;
        std::array<std::uint8_t, 96> input{};
        const auto status = serial::status(handle_);
        if (status.dropped != serial_status_.dropped || status.errors != serial_status_.errors) {
            // Discard partial framing after an overrun, but retain the absolute CF baseline so
            // a later checksum-valid frame can recover an unambiguous bounded counter delta.
            used_ = 0;
            if (!primary_controlled_)
                first_power_cycle_ = 0;
            retained_.available_fields = 0;
            reference_fields_ = 0;
            retained_.valid = false;
            serial_status_ = status;
        }
        const auto transfer = serial::read(handle_, input.data(), input.size());
        if (transfer.result != serial::Result::success)
            retained_.valid = false;
        for (std::size_t i = 0; i < transfer.count; ++i) {
            frame_[used_++] = input[i];
            if (used_ < frame_.size())
                continue;
            std::uint8_t checksum{};
            for (std::size_t j = 2; j < 23; ++j)
                checksum += frame_[j];
            if (frame_[1] == 0x5a && checksum == frame_[23]) {
                primary_observed_ = true;
                decode(now);
                used_ = 0;
            } else {
                std::memmove(frame_.data(), frame_.data() + 1, frame_.size() - 1);
                --used_;
            }
        }
        // Buffered frames share a poll timestamp. Validate only the latest absolute CF
        // value against the elapsed poll interval, avoiding a fictitious zero-time overload.
        if (counter_ready_) {
            // Consume this snapshot once: polls without another frame must not move counter_at_.
            counter_ready_ = false;
            if (!primary_controlled_)
                counter(now, latest_counter_);
        }
        if (!primary_controlled_ && now - received_at_ > 3000) {
            retained_.valid = false;
            retained_.available_fields = 0;
            first_power_cycle_ = 0;
            reference_fields_ = 0;
            voltage_cycle_ = current_cycle_ = power_cycle_ = 0;
        }
        if (count && primary_controlled_) {
            /** Source checksum-valid frames renew data_valid even while OFF or with an
             * abnormal hardware header; only parsed CF snapshots replace the raw counter.
             */
            const bool second = primary_age_.poll(now, primary_observed_);
            if (primary_age_.expired_now()) {
                retained_.voltage = retained_.current = retained_.power = 0;
                retained_.valid = false;
                retained_.watchdog_reset = true;
                retained_.source_retained = false;
            }
            if (second && primary_age_.stale()) {
                voltage_cycle_ = current_cycle_ = power_cycle_ = 0;
                reference_fields_ = 0;
            }
            auto sample = retained_;
            sample.available_fields = 7;
            sample.use_absolute = true;
            sample.use_pulses = false;
            if (!sample.valid && !sample.watchdog_reset)
                sample.source_retained = true;
            if (second && !primary_age_.stale()) {
                if (!counter_seen_) {
                    previous_counter_ = latest_counter_;
                    counter_seen_ = true;
                } else {
                    const auto pulses = std::uint16_t(latest_counter_ - previous_counter_);
                    if (pulses && retained_.power > 0) {
                        const auto delta = std::uint64_t(pulses) * (power_coefficient_ / 1000) / 36;
                        if (delta <= 5500U * 1000 / 36) {
                            previous_counter_ = latest_counter_;
                            sample.import_increment = delta * 10;
                            sample.increment_fields = 1;
                        } else {
                            /** Source overload invalidates the baseline; the next second
                             * initializes it instead of charging ambiguous retained pulses.
                             */
                            counter_seen_ = false;
                            AddLog(LogLevel::warning,
                                "NRG: CSE primary counter overload; rebase pending");
                        }
                    }
                }
            }
            samples[0] = sample;
        } else if (count) {
            auto sample = retained_;
            sample.use_pulses = true;
            sample.pulse_scale = power_coefficient_ * 10;
            if (sample.valid && (sample.available_fields & 4) && sample.power > 0) {
                sample.pulse_count = pending_pulses_;
                pending_pulses_ = 0;
            }
            samples[0] = sample;
        }
    }

  private:
    std::uint32_t number(std::size_t offset) const {
        return (std::uint32_t(frame_[offset]) << 16) | (std::uint32_t(frame_[offset + 1]) << 8) |
               frame_[offset + 2];
    }

    bool ratio(std::uint64_t coefficient, std::uint32_t cycle, std::int32_t& result) const {
        if (!cycle || !coefficient || coefficient * 1000 / cycle > INT32_MAX)
            return false;
        result = static_cast<std::int32_t>(coefficient * 1000 / cycle);
        return true;
    }

    /** @brief Applies a 5.5-kW physical envelope to distinguish reset/corruption from wrap. */
    void counter(std::uint32_t now, std::uint16_t current) {
        if (!counter_seen_) {
            previous_counter_ = current;
            counter_at_ = now;
            counter_seen_ = true;
            return;
        }
        const auto elapsed = static_cast<std::uint32_t>(now - counter_at_);
        const auto delta = static_cast<std::uint16_t>(current - previous_counter_);
        const auto budget = std::uint64_t{5500000} * elapsed + power_coefficient_;
        const bool ambiguous = !power_coefficient_ || budget / power_coefficient_ >= 65536;
        const bool excessive = std::uint64_t(delta) * power_coefficient_ > budget;
        previous_counter_ = current;
        counter_at_ = now;
        if (ambiguous || excessive || delta > UINT32_MAX - pending_pulses_) {
            AddLog(LogLevel::warning, "NRG: CSE counter reset or ambiguous gap; rebased");
            return;
        }
        pending_pulses_ += delta;
    }

    void decode(std::uint32_t now) {
        reference_fields_ = 0;
        const auto header = frame_[0];
        if ((header != 0x55 && header != 0xaa && (header & 0xf0) != 0xf0) ||
            (header & 0xfc) == 0xfc) {
            if (primary_controlled_) {
                /** The source abnormal-header path leaves arrays, raw CF and its baseline
                 * unchanged. Keep available retained output distinct from physical validity.
                 */
                retained_.valid = false;
                retained_.source_retained = !retained_.watchdog_reset;
                return;
            }
            retained_.valid = false;
            retained_.available_fields = 0;
            first_power_cycle_ = 0;
            counter_seen_ = false;
            counter_ready_ = false;
            return;
        }
        const auto voltage_default = (header == 0xaa ? 191200U : number(2)) / 100;
        const auto power_default = (header == 0xaa ? 5364000U : number(14)) / 1000;
        const auto current_default = header == 0xaa ? 16140U : number(8);
        const auto voltage_coefficient =
            std::uint64_t(calibration_.voltage ? calibration_.voltage : voltage_default) * 100;
        const auto power_coefficient =
            std::uint64_t(calibration_.power ? calibration_.power : power_default) * 1000;
        const auto current_coefficient =
            std::uint64_t(calibration_.current ? calibration_.current : current_default);
        if (!power_coefficient || !voltage_coefficient || !current_coefficient) {
            retained_.valid = false;
            return;
        }
        // A chip coefficient change cannot silently reprice already accepted pulse energy.
        if (power_coefficient_ && power_coefficient_ != power_coefficient && pending_pulses_) {
            retained_.valid = false;
            AddLog(LogLevel::warning, "NRG: CSE coefficient changed with pending energy");
            return;
        }
        /** Latch each unresolved coefficient from this valid triplet, not every future frame.
         * Header AA uses the source factory fallback. Service retains this effective snapshot
         * through its ordinary identity-keyed checkpoint without reentering calibration.
         */
        calibration_.voltage = voltage_coefficient / 100;
        calibration_.power = power_coefficient / 1000;
        calibration_.current = current_coefficient;
        learned_ = true;
        hardware_power_coefficient_ = std::uint64_t(power_default) * 1000;
        power_coefficient_ = power_coefficient;
        latest_counter_ = static_cast<std::uint16_t>((std::uint16_t(frame_[21]) << 8) | frame_[22]);
        counter_ready_ = true;
        retained_.watchdog_reset = retained_.source_retained = false;
        const auto flags = frame_[20];
        if (primary_controlled_ && !primary_on_) {
            /** OFF resets the first incomplete-power cycle but not CF or its saved baseline.
             * The invalid-power retry counter is untouched, as in CseReceived's OFF branch.
             */
            first_power_cycle_ = 0;
            voltage_cycle_ = number(5);
            current_cycle_ = number(11);
            power_cycle_ = number(17);
            reference_fields_ = ((flags & 0x40) && voltage_cycle_ ? 1 : 0) |
                                ((flags & 0x20) && current_cycle_ ? 2 : 0) |
                                ((flags & 0x10) && power_cycle_ && (header & 0xf2) != 0xf2 ? 4 : 0);
            retained_.voltage = retained_.current = retained_.power = 0;
            retained_.available_fields = 7;
            retained_.valid = true;
            received_at_ = now;
            return;
        }
        std::int32_t value{};
        if ((flags & 0x40) && ratio(voltage_coefficient, number(5), value)) {
            reference_fields_ |= 1;
            voltage_cycle_ = number(5);
            retained_.voltage = value;
            retained_.available_fields |= 1;
        }
        if (flags & 0x10) {
            invalid_power_ = 0;
            const auto cycle = number(17);
            if ((header & 0xf2) == 0xf2) {
                power_cycle_ = 0;
                retained_.power = 0;
                retained_.available_fields |= 4;
            } else if (cycle) {
                if (!first_power_cycle_)
                    first_power_cycle_ = cycle;
                if (first_power_cycle_ == cycle) {
                    power_cycle_ = 0;
                    retained_.power = 0;
                    retained_.available_fields |= 4;
                } else if (ratio(power_coefficient_, cycle, value)) {
                    first_power_cycle_ = UINT32_MAX;
                    reference_fields_ |= 4;
                    power_cycle_ = cycle;
                    retained_.power = value;
                    retained_.available_fields |= 4;
                }
            }
        } else if (invalid_power_ < options_.invalid_power_frames)
            ++invalid_power_;
        else {
            first_power_cycle_ = 0;
            power_cycle_ = 0;
            retained_.power = 0;
            retained_.available_fields |= 4;
        }
        if (flags & 0x20) {
            if ((retained_.available_fields & 4) && !retained_.power) {
                current_cycle_ = 0;
                retained_.current = 0;
                retained_.available_fields |= 2;
            } else if (ratio(current_coefficient, number(11), value)) {
                reference_fields_ |= 2;
                current_cycle_ = number(11);
                retained_.current = value;
                retained_.available_fields |= 2;
            }
        }
        retained_.valid = retained_.available_fields != 0;
        received_at_ = now;
    }

    serial::Handle handle_{};
    serial::Status serial_status_{};
    std::array<std::uint8_t, 24> frame_{};
    std::size_t used_{};
    Sample retained_{};
    Calibration calibration_{};
    Options options_{};
    std::uint64_t power_coefficient_{}, hardware_power_coefficient_{};
    std::uint32_t received_at_{}, first_power_cycle_{}, counter_at_{}, pending_pulses_{};
    std::uint32_t power_cycle_{}, voltage_cycle_{}, current_cycle_{};
    std::uint16_t previous_counter_{}, invalid_power_{}, latest_counter_{};
    std::uint8_t reference_fields_{};
    bool counter_seen_{}, counter_ready_{}, learned_{};
    ObservationAge primary_age_{};
    bool primary_threshold_applied_{};
    bool primary_controlled_{}, primary_on_{}, primary_observed_{};
};
} // namespace

/** @brief Creates a CSE7766 instance or leaves native resources with their existing owner.
 * @param configuration RX-only UART binding and common calibration for the family.
 * @return Meter owner, or nullptr when pin ownership or allocation fails.
 */
std::unique_ptr<Device> create_cse7766(const Configuration& configuration) {
    if (configuration.channels != 1 ||
        configuration.identity.pin(PinFunction::uart_rx).value() == PinId::kInvalid ||
        configuration.identity.pin(PinFunction::uart_tx).value() != PinId::kInvalid ||
        configuration.identity.address)
        return {};
    for (const auto& binding : configuration.identity.pins)
        if (binding.function != PinFunction::none && binding.function != PinFunction::uart_rx)
            return {};
    auto result = std::unique_ptr<Cse7766>(new (std::nothrow) Cse7766);
    if (!result || !result->open(configuration))
        return {};
    return result;
}
} // namespace newmota::energy
