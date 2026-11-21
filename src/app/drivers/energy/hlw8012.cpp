/*
  hlw8012.cpp - Shared HLW8012 and BL0937 pulse energy devices

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Pulse conversion reference: Tasmota xnrg_01_hlw8012.ino
  Copyright (C) 2021 Theo Arends

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file hlw8012.cpp
 * @brief Shared HLW8012 and BL0937 pulse energy devices.
 */

#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "drivers/gpio/edge_capture_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/log.h"
#include <limits>
#include <algorithm>
#include <new>

namespace newmota::energy {
namespace {
/** @brief Converts pulse periods on the application task; native IRQs only retain integers. */
class Hlw8012 final : public Device {
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

    explicit Hlw8012(bool bl0937)
        : power_reference_(bl0937 ? 1362 : 10000), voltage_reference_(bl0937 ? 822 : 2200),
          current_reference_(bl0937 ? 3300 : 4545) {}

    bool open(const Configuration& configuration) {
        if (configuration.channels != 1 ||
            configuration.identity.pin(PinFunction::cf).value() == PinId::kInvalid ||
            (configuration.options.flags & ~1U) || !calibrate(configuration.calibration))
            return false;
        inverted_ = (configuration.options.flags & 1) != 0;
        const bool has_cf1 =
            configuration.identity.pin(PinFunction::cf1).value() != PinId::kInvalid;
        if (configuration.identity.bus || configuration.identity.address)
            return false;
        cf_pin_ = PinId{configuration.identity.pin(PinFunction::cf).value()};
        cf1_pin_ = PinId{configuration.identity.pin(PinFunction::cf1).value()};
        selector_ = configuration.identity.pin(PinFunction::selector);
        if (!has_cf1 && selector_.valid())
            return false;
        if (cf_pin_.value() == cf1_pin_.value() || cf_pin_.value() == selector_.value() ||
            (has_cf1 && cf1_pin_.value() == selector_.value()))
            return false;
        if (selector_.valid()) {
            selector_owner_ = runtime_pins::allocate_energy_owner();
            if (!runtime_pins::claim(selector_owner_, &selector_, 1))
                return false;
            selector_claimed_ = true;
            if (platform::gpio_configure_output(selector_, !inverted_) !=
                platform::GpioResult::success) {
                selector_faulted_ = true;
                return false;
            }
        }
        platform::edge_capture::Configuration capture{};
        capture.pin = cf_pin_;
        capture.edge = platform::edge_capture::Edge::falling;
        capture.maximum_interval_us = 10000000;
        capture.pull = platform::GpioPull::up;
        if (edges::open(capture, cf_) != edges::Result::success)
            return false;
        if (has_cf1) {
            capture.pin = cf1_pin_;
            capture.maximum_interval_us = 0;
            if (edges::open(capture, cf1_) != edges::Result::success)
                return false;
        }
        started_ = switched_at_ = power_window_at_ =
            platform::edge_capture::monotonic_microseconds();
        if (has_cf1 && !arm_ui(started_))
            return false;
        return true;
    }

    ~Hlw8012() override {
        (void)edges::close(cf_);
        (void)edges::close(cf1_);
        if (selector_claimed_ && !selector_faulted_) {
            if (platform::gpio_configure_input(selector_, platform::GpioPull::none) ==
                platform::GpioResult::success)
                runtime_pins::release(selector_owner_);
        }
    }

    std::uint32_t default_coefficient(CalibrationField field) const override {
        switch (field) {
        case CalibrationField::power:
            return 12530;
        case CalibrationField::voltage:
            return 1950;
        case CalibrationField::current:
            return 3500;
        default:
            return 0;
        }
    }

    bool calibrate(const Calibration& config) override {
        if (config.power > INT32_MAX || config.voltage > INT32_MAX || config.current > INT32_MAX)
            return false;
        power_calibration_ = config.power ? config.power : 12530;
        voltage_calibration_ = config.voltage ? config.voltage : 1950;
        current_calibration_ = config.current ? config.current : 3500;
        return true;
    }

    bool set_options(const Options& options) override {
        if (options.flags & ~1U)
            return false;
        const bool inverted = (options.flags & 1) != 0;
        if (selector_.valid() && platform::gpio_write(selector_, voltage_mode_ != inverted) !=
                                     platform::GpioResult::success) {
            selector_faulted_ = true;
            return false;
        }
        inverted_ = inverted;
        switched_at_ = platform::edge_capture::monotonic_microseconds();
        voltage_at_ = current_at_ = 0;
        return !cf1_pin_.valid() || arm_ui(switched_at_);
    }

    bool calibration_for(
        CalibrationField field, std::uint32_t reference, Calibration& calibration) const override {
        if (primary_controlled_ && !primary_on_)
            return false;
        std::uint64_t value{};
        if (field == CalibrationField::power && power_period_)
            value = std::uint64_t(reference / 100) * power_period_ / power_reference_;
        else if (field == CalibrationField::voltage && voltage_period_)
            value = std::uint64_t(reference / 100) * voltage_period_ / voltage_reference_;
        else if (field == CalibrationField::current && current_period_)
            value = std::uint64_t(reference) * current_period_ / current_reference_;
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
    /** @brief Uses the source global gate for primary measurements, never raw CF accounting. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now_ms, Sample* samples, std::size_t count) override {
        if (!count)
            return;
        Sample sample{};
        sample.use_pulses = true;
        sample.available_fields = 0; // Each field becomes available independently.
        sample.pulse_scale = std::uint64_t(power_reference_) * power_calibration_;
        const auto now = platform::edge_capture::monotonic_microseconds();
        edges::Snapshot cf{};
        if (selector_faulted_ || edges::snapshot(cf_, cf) != edges::Result::success ||
            cf.overflow) {
            power_ready_ = false;
            power_period_ = voltage_period_ = current_period_ = 0;
            if (primary_controlled_) {
                /** A failed capture cannot confirm an increment or advance either CF baseline.
                 * It still ages retained output to the source watchdog zero boundary; native
                 * owner claims and previously captured pending periods remain untouched.
                 */
                (void)primary_age_.poll(now_ms, false);
                if (primary_age_.expired_now())
                    power_ = voltage_ = current_ = 0;
                sample.use_absolute = true;
                sample.use_pulses = false;
                sample.available_fields = 4 | (cf1_pin_.valid() ? 1 : 0) |
                                          (selector_.valid() && cf1_pin_.valid() ? 2 : 0);
                sample.power = power_;
                sample.voltage = voltage_;
                sample.current = current_;
                sample.watchdog_reset =
                    primary_age_.stale() && !sample.power && !sample.voltage && !sample.current;
                sample.source_retained = !sample.watchdog_reset;
            }
            samples[0] = sample;
            return;
        }
        sample.pulse_count = cf.period_count - previous_cf_.period_count;
        const bool on = !primary_controlled_ || primary_on_;
        const bool fresh_cf = cf.count != previous_cf_.count;
        const auto previous_ui_edge = last_ui_edge_;
        if (primary_controlled_)
            primary_energy_periods_ += sample.pulse_count;
        const auto age = now - (cf.seen ? cf.last_edge_us : started_);
        if (now - power_window_at_ >= 200000) {
            auto period = cf.last_period_us;
            const auto periods = cf.period_count - power_baseline_.period_count;
            if (periods)
                period = (cf.summed_period_us - power_baseline_.summed_period_us) / periods;
            power_period_ = period;
            if (primary_controlled_) {
                if (age > 10000000)
                    power_period_ = 0;
                /** Source gates the 200-ms conversion, retaining power for one retry window.
                 * Period counters continue to advance regardless of this output gate.
                 */
                if (age <= 10000000 && period && on) {
                    power_ready_ = convert((sample.pulse_scale / period) * 100, 1, power_);
                    power_retry_ = power_ready_;
                } else if (power_retry_)
                    power_retry_ = false;
                else {
                    power_ = 0;
                    power_ready_ = true;
                }
            } else if (age >= 10000000) {
                power_period_ = 0;
                if (power_retry_)
                    power_retry_ = false;
                else
                    power_ = 0;
                power_ready_ = true;
            } else if (period) {
                power_ready_ = convert(sample.pulse_scale * 100, period, power_);
                power_retry_ = power_ready_;
            } else if (cf.seen) {
                power_ = 0;
                power_ready_ = true;
            }
            power_baseline_ = cf;
            power_window_at_ = now;
        }
        sample.power = power_;
        if (power_ready_)
            sample.available_fields |= 4;
        previous_cf_ = cf;
        if (cf1_pin_.valid())
            sample_ui(now, power_ == 0 && power_ready_, on);
        if (voltage_at_ && now - voltage_at_ <= 10000000) {
            sample.voltage = voltage_;
            sample.available_fields |= 1;
        }
        if (current_at_ && now - current_at_ <= 10000000) {
            sample.current = current_;
            sample.available_fields |= 2;
        }
        if (!voltage_at_ || now - voltage_at_ > 10000000)
            voltage_period_ = 0;
        if (!current_at_ || now - current_at_ > 10000000)
            current_period_ = 0;
        const auto last_edge = std::max(cf.seen ? cf.last_edge_us : 0, last_ui_edge_);
        sample.valid = sample.available_fields != 0 && last_edge && now - last_edge <= 4000000;
        if (primary_controlled_) {
            /** Source CF/CF1 edges renew data_valid even while OFF. The source one-second
             * energy hook consumes the raw period counter without checking power_on.
             */
            const bool second =
                primary_age_.poll(now_ms, fresh_cf || previous_ui_edge != last_ui_edge_);
            sample.use_pulses = false;
            sample.use_absolute = true;
            sample.pulse_count = 0;
            sample.available_fields =
                4 | (cf1_pin_.valid() ? 1 : 0) | (selector_.valid() && cf1_pin_.valid() ? 2 : 0);
            if (primary_age_.expired_now())
                power_ = voltage_ = current_ = 0;
            if (second && primary_age_.stale())
                power_period_ = voltage_period_ = current_period_ = 0;
            if (second && !primary_age_.stale() && primary_energy_periods_) {
                const auto periods = primary_energy_periods_;
                primary_energy_periods_ = 0;
                const auto length = 1000000U / periods;
                if (length) {
                    sample.import_increment = ((sample.pulse_scale / 36) * 100 / length) * 10;
                    sample.increment_fields = 1;
                }
            }
            sample.power = power_;
            sample.voltage = voltage_;
            sample.current = current_;
            sample.valid = !primary_age_.stale() && last_edge != 0;
            sample.watchdog_reset =
                primary_age_.stale() && !sample.power && !sample.voltage && !sample.current;
            sample.source_retained = !sample.valid && !sample.watchdog_reset;
        }
        samples[0] = sample;
    }

  private:
    bool convert(std::uint64_t coefficient, std::uint64_t period, std::int32_t& value) {
        if (!period || coefficient / period > INT32_MAX)
            return false;
        value = static_cast<std::int32_t>(coefficient / period);
        return true;
    }

    /** @brief Matches the source's timer>2 settle interval and first ten CF1 periods. */
    bool arm_ui(std::uint64_t now) {
        ui_tick_at_ = now;
        return edges::window(cf1_, {now + 600000, now + 1600000, 10}) == edges::Result::success;
    }

    void sample_ui(std::uint64_t now, bool no_load, bool voltage_powered) {
        edges::Snapshot snapshot{};
        if (edges::snapshot(cf1_, snapshot) != edges::Result::success || snapshot.overflow) {
            voltage_at_ = current_at_ = 0;
            return;
        }
        last_ui_edge_ = snapshot.seen ? snapshot.last_edge_us : 0;
        if (now - ui_tick_at_ < 200000)
            return;
        ui_tick_at_ = now;
        if (snapshot.window_count < 10 && now - switched_at_ < 1600000)
            return;
        const auto period =
            snapshot.window_count ? snapshot.window_sum_us / snapshot.window_count : 0;
        std::int32_t value{};
        if (voltage_mode_) {
            voltage_period_ = 0;
            voltage_ = 0;
            if (convert(std::uint64_t(voltage_reference_) * voltage_calibration_ * 100, period,
                    value)) {
                voltage_period_ = period;
                if (primary_controlled_)
                    value = (value / 100) * 100;
                voltage_ = voltage_powered ? value : 0;
            }
            voltage_at_ = now;
        } else {
            current_period_ = 0;
            current_ = 0;
            if (!no_load &&
                convert(std::uint64_t(current_reference_) * current_calibration_, period, value)) {
                current_period_ = period;
                current_ = value;
            }
            current_at_ = now;
        }
        if (selector_.valid()) {
            voltage_mode_ = !voltage_mode_;
            if (platform::gpio_write(selector_, voltage_mode_ != inverted_) !=
                platform::GpioResult::success) {
                selector_faulted_ = true;
                voltage_at_ = current_at_ = 0;
                return;
            }
        }
        switched_at_ = now;
        if (!arm_ui(now)) {
            selector_faulted_ = true;
            voltage_at_ = current_at_ = 0;
        }
    }

    edges::Handle cf_{}, cf1_{};
    edges::Snapshot previous_cf_{}, power_baseline_{};
    PinId cf_pin_{}, cf1_pin_{}, selector_{};
    runtime_pins::Owner selector_owner_{};
    std::uint32_t power_reference_, voltage_reference_, current_reference_;
    std::uint32_t power_calibration_{}, voltage_calibration_{}, current_calibration_{};
    std::uint64_t started_{}, switched_at_{}, voltage_at_{}, current_at_{}, power_window_at_{},
        ui_tick_at_{}, last_ui_edge_{};
    std::int32_t voltage_{}, current_{}, power_{};
    std::uint32_t power_period_{}, voltage_period_{}, current_period_{};
    bool voltage_mode_{true}, inverted_{}, selector_claimed_{}, power_ready_{}, selector_faulted_{},
        power_retry_{};
    ObservationAge primary_age_{};
    std::uint32_t primary_energy_periods_{};
    bool primary_controlled_{}, primary_on_{};
};

std::unique_ptr<Device> create(const Configuration& configuration, bool bl0937) {
    for (const auto& binding : configuration.identity.pins)
        if (binding.function != PinFunction::none && binding.function != PinFunction::cf &&
            binding.function != PinFunction::cf1 && binding.function != PinFunction::selector)
            return {};
    auto result = std::unique_ptr<Hlw8012>(new (std::nothrow) Hlw8012(bl0937));
    if (!result || !result->open(configuration))
        return {};
    return result;
}
} // namespace

/** @brief Creates an HLW8012 instance over shared edge capture and exclusive SEL ownership.
 * @param configuration Bound CF, CF1, and optional selector pins with calibration.
 * @return Pulse device owner, or nullptr when resources cannot be claimed.
 */
std::unique_ptr<Device> create_hlw8012(const Configuration& configuration) {
    return create(configuration, false);
}

/** @brief Creates a BL0937/HJL instance sharing the pulse implementation with its own ratios.
 * @param configuration Bound CF, CF1, and optional selector pins with calibration.
 * @return Pulse device owner, or nullptr when resources cannot be claimed.
 */
std::unique_ptr<Device> create_bl0937(const Configuration& configuration) {
    return create(configuration, true);
}
} // namespace newmota::energy
