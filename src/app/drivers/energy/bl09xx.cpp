/*
  bl09xx.cpp - Portable BL0939, BL0940 and BL0942 energy metering

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol and conversion adapted from Tasmota xnrg_14_bl09xx.ino.
  Copyright (C) 2021 Theo Arends
  Upstream distributed under GPL version 3 or later, without any warranty.
*/

/** @file bl09xx.cpp
 * @brief Shared bounded UART protocol with separately selectable device factories.
 * Hardware CF counters are not used by the pinned source; accounting uses signed
 * published power at the source one-second boundary.
 */
#include "services/energy/energy_service.h"
#if USE_GLOBAL_TEMPERATURE
#include "services/temperature/global_temperature.h"
#endif
#include "services/energy/energy_observation_age.h"
#include "drivers/serial/serial_service.h"
#include "drivers/sensors/sensor_service.h"
#include "core/command/command.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <array>
#include <cstring>
#include <new>

namespace newmota::energy {
namespace {
enum class Model : std::uint8_t { bl0939, bl0940, bl0942 };

struct Spec {
    std::uint32_t power, voltage, current;
    std::uint8_t address, channels, packet;
};

/** @brief Factory-local descriptors keep deselected model constants out of retained code. */
struct Raw {
    std::uint32_t current{};
    std::int32_t power{};
    Calibration calibration{};
    Sample published{};
};

std::uint32_t little24(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
}

std::int32_t signed24(const std::uint8_t* p) {
    const auto value = little24(p);
    return value & 0x800000 ? static_cast<std::int32_t>(value) - 0x1000000
                            : static_cast<std::int32_t>(value);
}

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

/** @brief Owns one UART, configured channel state and an asynchronous startup/read scheduler. */
class Bl09xx final : public Device {
    Model model_;
    Spec spec_;
    serial::Handle port_{};
    serial::Config uart_{};
    serial::Status status_{};
    std::unique_ptr<Raw[]> channels_;
    std::array<std::uint8_t, 35> rx_{};
    std::array<std::uint8_t, 6> tx_{};
    std::size_t rx_size_{}, tx_size_{}, written_{};
    std::uint32_t now_{}, due_{}, tx_deadline_{}, valid_at_{}, decoded_at_{}, byte_at_{};
    std::uint32_t voltage_{}, frequency_{}, good_{}, bad_{}, filtered_{}, errors_{}, requests_{};
    std::int64_t temperature_mc_{};
#if USE_BL0939 || USE_BL0940
    std::uint16_t temperature_raw_{};
#endif
    std::uint8_t options_{}, init_{};
    bool seen_{}, decoded_{}, draining_{}, init_frame_{};
    ObservationAge primary_age_{};
#if USE_BL0939
    ObservationAge primary_second_age_{};
#endif
    bool primary_controlled_{}, primary_on_{}, primary_observed_{};

    /** @brief A valid checksum refreshes source validity even when its temperature filter rejects
     * conversion. */
    void decode() {
        seen_ = true;
        valid_at_ = now_;
        primary_observed_ = primary_controlled_;
#if USE_BL0939 || USE_BL0940
        if (model_ != Model::bl0942) {
            const auto temperature = std::uint16_t(rx_[28]) | (std::uint16_t(rx_[29]) << 8);
#if USE_BL0940
            if (model_ == Model::bl0940 && temperature_raw_ &&
                (int(temperature) < int(temperature_raw_) - 10 ||
                    int(temperature) > int(temperature_raw_) + 10)) {
                temperature_raw_ = temperature;
                ++filtered_;
                return;
            }
#endif
            temperature_raw_ = temperature;
            // Exact milli-Celsius form of (170/448)*(TPS1/2-32)-45.
            temperature_mc_ = (std::int64_t(temperature) - 64) * 85000 / 448 - 45000;
#if USE_GLOBAL_TEMPERATURE
            global_temperature().observe(static_cast<std::int32_t>(temperature_mc_), now_);
#endif
            voltage_ = little24(rx_.data() + 10);
            for (unsigned c = 0; c < spec_.channels; ++c) {
                channels_[c].current = little24(rx_.data() + 4 + c * 3);
                channels_[c].power = signed24(rx_.data() + 16 + c * 3);
            }
        }
#if USE_BL0942
        else
#endif
#endif
#if USE_BL0942
        {
            voltage_ = little24(rx_.data() + 4);
            channels_[0].current = little24(rx_.data() + 1);
            auto power = signed24(rx_.data() + 10);
            if (!(options_ & 4) && power < 0)
                power = -power;
            channels_[0].power = power;
            frequency_ = std::uint16_t(rx_[16]) | (std::uint16_t(rx_[17]) << 8);
        }
#endif
        decoded_ = true;
        decoded_at_ = now_;
        if (primary_controlled_)
            publish_primary();
    }

    /** @brief Mirrors UpdateEnergy only after a successfully decoded frame; OFF zeros V/I/P
     * but retains frequency. A checksum-valid temperature-filter rejection never enters here.
     */
    void publish_primary() {
        for (unsigned c = 0; c < spec_.channels; ++c) {
            auto& sample = channels_[c].published;
            const auto cal = channels_[c].calibration;
            sample.valid = true;
            sample.watchdog_reset = sample.source_retained = false;
            sample.available_fields = 7;
            sample.voltage = primary_on_ ? saturate_i32(std::uint64_t(voltage_) * 1000 /
                                                        channels_[0].calibration.voltage)
                                         : 0;
            sample.power = sample.current = 0;
            if (primary_on_ && static_cast<std::uint32_t>(channels_[c].power) > cal.power) {
                sample.power = std::int64_t(channels_[c].power) * 1000 / cal.power;
                sample.current =
                    saturate_i32(std::uint64_t(channels_[c].current) * 1000 / cal.current);
            }
            if (model_ == Model::bl0942) {
                if (primary_on_ && frequency_)
                    sample.frequency_millihz = 1000000000U / frequency_;
                sample.auxiliary_fields |= 1;
            }
        }
    }

    /** @brief Retains a possible embedded header after a checksum failure, as upstream does. */
    void receive() {
        std::array<std::uint8_t, 128> bytes{};
        const auto transfer = serial::read(port_, bytes.data(), bytes.size());
        if (transfer.result != serial::Result::success) {
            ++errors_;
            rx_size_ = 0;
            return;
        }
        for (std::size_t i = 0; i < transfer.count; ++i) {
            if (rx_size_ && now_ - byte_at_ > 100) {
                rx_size_ = 0;
                ++bad_;
            }
            byte_at_ = now_;
            if (!rx_size_ && bytes[i] != 0x55)
                continue;
            rx_[rx_size_++] = bytes[i];
            if (rx_size_ != spec_.packet)
                continue;
            std::uint8_t checksum = 0x50 | spec_.address;
            for (std::size_t j = 0; j + 1 < spec_.packet; ++j)
                checksum += rx_[j];
            checksum ^= 0xff;
            if (checksum == rx_[spec_.packet - 1]) {
                ++good_;
                decode();
                rx_size_ = 0;
            } else {
                ++bad_;
                std::size_t next = 1;
                while (next < rx_size_ && rx_[next] != 0x55)
                    ++next;
                rx_size_ -= next;
                std::memmove(rx_.data(), rx_.data() + next, rx_size_);
            }
        }
    }

    void prepare() {
        if (!reached(now_, due_) || tx_size_)
            return;
        const bool initialization = init_ < 5;
        if (!initialization) {
            rx_size_ = 0;
            if (serial::discard_rx(port_) != serial::Result::success) {
                ++errors_;
                due_ = now_ + 1000;
                return; // No burst reservation has been acquired.
            }
        }
        if (serial::begin_tx(port_) != serial::Result::success)
            return;
        init_frame_ = initialization;
        if (init_frame_) {
#if USE_BL0939 || USE_BL0940
            constexpr std::uint8_t init[5][4] = {{0x19, 0x5a, 0x5a, 0x5a}, {0x1a, 0x55, 0, 0},
                {0x18, 0, 0x10, 0}, {0x1b, 0xff, 0x47, 0}, {0x10, 0x1c, 0x18, 0}};
            tx_[0] = 0xa0 | spec_.address;
            std::uint8_t sum = tx_[0];
            for (unsigned i = 0; i < 4; ++i) {
                tx_[i + 1] = init[init_][i];
                sum += tx_[i + 1];
            }
            tx_[5] = sum ^ 0xff;
            tx_size_ = 6;
#endif
        } else {
            tx_[0] = 0x50 | spec_.address;
            tx_[1] = 0xaa;
            tx_size_ = 2;
        }
        written_ = 0;
        draining_ = false;
        tx_deadline_ = now_ + 1000;
    }

    void transmit() {
        if (!tx_size_)
            return;
        if (!draining_) {
            const auto transfer =
                serial::write_some(port_, tx_.data() + written_, tx_size_ - written_);
            written_ += transfer.count;
            if (written_ == tx_size_ ||
                (transfer.result != serial::Result::success &&
                    transfer.result != serial::Result::busy) ||
                reached(now_, tx_deadline_)) {
                if (written_ != tx_size_)
                    ++errors_;
                (void)serial::end_tx(port_);
                draining_ = true;
            }
        }
        if (draining_ && serial::tx_idle(port_)) {
            const bool sent = written_ == tx_size_;
            if (sent && init_frame_)
                ++init_;
            else if (sent)
                ++requests_;
            tx_size_ = 0;
            due_ = now_ + (init_ < 5 ? 1 : 1000);
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies the existing source age without changing watchdog or accounting state. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!primary_controlled_ || channel >= spec_.channels)
            return false;
#if USE_BL0939
        /** @brief The source renews only phase zero; BL0939 phase two independently ages to five.
         */
        if (channel) {
            output = primary_second_age_.value();
            return true;
        }
#endif
        output = primary_age_.value();
        return true;
    }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Preserves this source model's global-temperature protection request. */
    bool use_overtemperature() const override { return model_ != Model::bl0942; }
#endif

    /** @brief Matches the source model's shared voltage/frequency topology. */
    bool common_voltage() const override { return true; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return true; }

    Bl09xx(Model model, Spec spec)
        : model_(model), spec_(spec), channels_(new (std::nothrow) Raw[spec.channels]) {}

    ~Bl09xx() override { (void)serial::close(port_); }

    bool open(const Configuration& config) {
        if (!channels_ || config.channels != spec_.channels || config.identity.bus ||
            config.identity.address != spec_.address)
            return false;
        for (const auto& binding : config.identity.pins)
            if (binding.pin.valid() && binding.function != PinFunction::uart_rx &&
                binding.function != PinFunction::uart_tx)
                return false;
        uart_.rx = config.identity.pin(PinFunction::uart_rx);
        uart_.tx = config.identity.pin(PinFunction::uart_tx);
        if (!uart_.rx.valid() || !uart_.tx.valid() || !valid_options(config.options))
            return false;
        options_ = config.options.flags;
        uart_.baud = 4800U << (model_ == Model::bl0942 ? options_ & 3 : 0);
        if (serial::open(uart_, port_) != serial::Result::success)
            return false;
        init_ = model_ == Model::bl0942 ? 5 : 0;
        now_ = due_ = platform::monotonic_milliseconds();
        status_ = serial::status(port_);
        return calibrate(config.calibration);
    }

#if USE_BL0939 || USE_BL0940
    /** @brief Appends the BL0939/BL0940 chip temperature as the source does, in the shared
     * temperature unit, offset and resolution: "BL09XX":{"Temperature":t}.
     */
    bool write_json_fields(CommandOutput& output) const override {
        std::int32_t temperature{};
        if (!chip_temperature(temperature))
            return true;
        return output.write(",\"BL09XX\":{\"Temperature\":") &&
               sensor_service().write_value(output, SensorQuantity::temperature, temperature) &&
               output.write("}");
    }

    /** @brief BL0939 and BL0940 measure their die temperature; BL0942 has none. */
    bool chip_temperature(std::int32_t& millicelsius) const override {
        if (model_ == Model::bl0942 || !decoded_)
            return false;
        millicelsius = saturate_i32(temperature_mc_);
        return true;
    }
#endif

    bool online() const override { return init_ == 5 && serial::active(port_); }

    bool independent_calibration() const override { return true; }

    std::uint32_t current_reference_scale() const override { return 100; }

    bool shared_voltage_calibration() const override { return true; }

    Calibration canonical_calibration(Calibration value) const override {
        if (!value.power)
            value.power = spec_.power;
        if (!value.voltage)
            value.voltage = spec_.voltage;
        if (!value.current)
            value.current = spec_.current;
        return value;
    }

    bool initial_calibration(Calibration& value) const override {
        value = canonical_calibration(value);
        return true;
    }

    std::uint32_t default_coefficient(CalibrationField field) const override {
        return field == CalibrationField::power     ? spec_.power
               : field == CalibrationField::voltage ? spec_.voltage
               : field == CalibrationField::current ? spec_.current
                                                    : 0;
    }

    bool calibrate(const Calibration& value) override {
        if (!channels_)
            return false;
        for (unsigned c = 0; c < spec_.channels; ++c)
            channels_[c].calibration = canonical_calibration(value);
        return true;
    }

    bool calibrate_channel(std::size_t channel, const Calibration& value) override {
        if (channel >= spec_.channels)
            return false;
        channels_[channel].calibration = canonical_calibration(value);
        return true;
    }

    bool calibration_for_channel(std::size_t channel, CalibrationField field,
        std::uint32_t reference, Calibration& output) const override {
        if (channel >= spec_.channels || !reference || !decoded_ || now_ - decoded_at_ > 4000)
            return false;
        std::uint64_t raw{};
        if (field == CalibrationField::power) {
            if (channels_[channel].power <= 0)
                return false;
            raw = channels_[channel].power;
        } else if (field == CalibrationField::voltage)
            raw = voltage_;
        else if (field == CalibrationField::current)
            raw = channels_[channel].current;
        else
            return false;
        // Source inputs are truncated to hundredths, including centi-mA current references.
        std::uint64_t value{};
        if (field == CalibrationField::current)
            value = (raw * 100 / reference) * 1000;
        else {
            const auto hundredths = reference / 10;
            if (!hundredths)
                return false;
            value = raw * 100 / hundredths;
        }
        if (!value || value > UINT32_MAX)
            return false;
        if (field == CalibrationField::power)
            output.power = value;
        else if (field == CalibrationField::voltage)
            output.voltage = value;
        else
            output.current = value;
        return true;
    }

    bool valid_options(const Options& value) const {
        return !(value.flags & ~(model_ == Model::bl0942 ? 7U : 0U)) &&
               value.invalid_power_frames == 128;
    }

    bool set_options(const Options& value) override {
        if (!valid_options(value))
            return false;
        const auto baud = 4800U << (model_ == Model::bl0942 ? value.flags & 3 : 0);
        auto updated = uart_;
        updated.baud = baud;
        const bool baud_changed = baud != uart_.baud;
        if (baud_changed && serial::reconfigure(port_, updated) != serial::Result::success)
            return false;
        if (baud_changed || options_ != value.flags) {
            if (serial::discard_rx(port_) != serial::Result::success) {
                ++errors_;
                if (baud_changed)
                    (void)serial::reconfigure(port_, uart_);
                return false;
            }
            rx_size_ = 0;
            seen_ = decoded_ = false;
            for (unsigned c = 0; c < spec_.channels; ++c)
                channels_[c].published = {};
        }
        uart_ = updated;
        options_ = value.flags;
        return true;
    }

#if USE_ENERGY_POWER_POLICY
    /** @brief Selects the source global gate for primary publication, not raw UART reception. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* output, std::size_t count) override {
        now_ = now;
        primary_observed_ = false;
        if (serial::active(port_)) {
            const auto status = serial::status(port_);
            if (status.errors != status_.errors || status.dropped != status_.dropped) {
                ++errors_;
                rx_size_ = 0;
            }
            status_ = status;
            transmit();
            receive();
            prepare();
            transmit();
        }
        const bool second = primary_controlled_ && primary_age_.poll(now_, primary_observed_);
        if (second && primary_age_.stale()) {
            voltage_ = 0;
            for (unsigned c = 0; c < spec_.channels; ++c)
                channels_[c].current = channels_[c].power = 0;
        }
        for (std::size_t c = 0; c < count && c < spec_.channels; ++c) {
            auto& sample = output[c];
            sample = {};
            if (primary_controlled_) {
                auto& published = channels_[c].published;
                bool reset = primary_age_.expired_now();
#if USE_BL0939
                if (c) {
                    /** Pinned BL0939 renews only data_valid[0]. Phase two gets the core's
                     * one-time aging reset, but later decode can republish without rearming it.
                     * The one-second accounting hook still uses phase one's age for all phases.
                     */
                    (void)primary_second_age_.poll(now_, false);
                    reset = primary_second_age_.expired_now();
                }
#endif
                if (reset) {
                    published.voltage = published.current = published.power = 0;
                    published.frequency_millihz = 0;
                    published.valid = false;
                    published.watchdog_reset = true;
                    published.source_retained = false;
                }
                if (primary_age_.stale())
                    published.valid = false;
                sample = published;
                sample.available_fields = 7;
                sample.use_absolute = true;
                if (model_ == Model::bl0942)
                    sample.auxiliary_fields |= 1;
                if (!sample.valid && !sample.watchdog_reset)
                    sample.source_retained = true;
                /** Source one-second hook accounts retained published power, including any
                 * interval before an OFF frame actually zeros it. The source int32 delta is
                 * trunc(power_mW/36) in 10-micro-Wh units; preserve truncation per hook, not
                 * rational carry across hooks. Unsigned magnitude also handles INT64_MIN safely.
                 * No elapsed-gap extrapolation or ordinary measured-power integration.
                 */
                if (second && !primary_age_.stale() && sample.power) {
                    const bool exporting = sample.power < 0;
                    const auto power = exporting ? std::uint64_t(-(sample.power + 1)) + 1
                                                 : std::uint64_t(sample.power);
                    const auto increment = power / 36 * 10;
                    if (exporting)
                        sample.export_increment = increment;
                    else
                        sample.import_increment = increment;
                    sample.increment_fields = exporting ? 2 : 1;
                }
                if (model_ != Model::bl0942 && decoded_) {
                    sample.temperature_millicelsius = saturate_i32(temperature_mc_);
                    sample.auxiliary_fields |= 1U << 16;
                }
                continue;
            }
            const auto cal = channels_[c].calibration;
            sample.valid = online() && seen_ && now_ - valid_at_ <= 4000;
            sample.voltage =
                saturate_i32(std::uint64_t(voltage_) * 1000 / channels_[0].calibration.voltage);
            // Preserve C++ usual arithmetic conversions in the pinned mixed signed/unsigned cutoff.
            if (static_cast<std::uint32_t>(channels_[c].power) > cal.power) {
                sample.power = std::int64_t(channels_[c].power) * 1000 / cal.power;
                sample.current =
                    saturate_i32(std::uint64_t(channels_[c].current) * 1000 / cal.current);
            }
            if (model_ == Model::bl0942 && frequency_) {
                sample.frequency_millihz = 1000000000U / frequency_;
                sample.auxiliary_fields |= 1;
            }
            if (model_ != Model::bl0942 && decoded_) {
                sample.temperature_millicelsius = saturate_i32(temperature_mc_);
                sample.auxiliary_fields |= 1U << 16;
            }
        }
    }
};

std::unique_ptr<Device> create(const Configuration& config, Model model, Spec spec) {
    auto device = std::unique_ptr<Bl09xx>(new (std::nothrow) Bl09xx(model, spec));
    if (!device || !device->open(config))
        return {};
    return device;
}
} // namespace

#if USE_BL0939
/** @brief Creates the selected dual-channel BL0939 at address5/4800 baud.
 * @param config Two-channel UART identity and retained coefficients/options.
 * @return Device, or empty on unavailable resources/invalid configuration.
 */
std::unique_ptr<Device> create_bl0939(const Configuration& config) {
    return create(config, Model::bl0939, {713, 17159, 266013, 5, 2, 35});
}
#endif
#if USE_BL0940
/** @brief Creates the selected single-channel BL0940 with its temperature/init behavior.
 * @param config Single-channel UART identity, address0 and coefficients/options.
 * @return Device, or empty on unavailable resources/invalid configuration.
 */
std::unique_ptr<Device> create_bl0940(const Configuration& config) {
    return create(config, Model::bl0940, {1430, 33000, 275000, 0, 1, 35});
}
#endif
#if USE_BL0942
/** @brief Creates the selected BL0942 with explicit three-bit baud/sign options.
 * @param config Single-channel UART identity, address8 and coefficients/options.
 * @return Device, or empty on unavailable resources/invalid configuration.
 */
std::unique_ptr<Device> create_bl0942(const Configuration& config) {
    return create(config, Model::bl0942, {596, 15187, 251213, 8, 1, 23});
}
#endif
} // namespace newmota::energy
