/*
  ade7953.cpp - Portable ADE7953 I2C and SPI energy devices

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol and calibration behavior reference: Tasmota xnrg_07_ade7953.ino
  Copyright (C) 2021 Theo Arends
  Register layout and calibration are based on the Analog Devices ADE7953 data sheet.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file ade7953.cpp
 * @brief Owns ADE7953 I2C/SPI sessions, initialization and read-to-clear energy acquisition.
 */
#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "core/hardware/runtime_pin_claims.h"
#include "core/storage/filesystem.h"
#include "support/integer_object.h"
#include "drivers/i2c/i2c_device_claims.h"
#include "drivers/spi/spi_service.h"
#include "newmota/log.h"
#include "newmota/platform/i2c.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace newmota::energy {
namespace {
/** @brief Keep unselected transport implementations out of dependency closure and linkage. */
device_spi::Result spi_transfer(
    device_spi::Device d, const std::uint8_t* tx, std::uint8_t* rx, std::size_t size) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::transfer(d, tx, rx, size);
#else
    (void)d;
    (void)tx;
    (void)rx;
    (void)size;
    return device_spi::Result::unsupported;
#endif
}

device_spi::Result spi_open(const device_spi::BusConfiguration& c, device_spi::Bus& b) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::open(c, b);
#else
    (void)c;
    (void)b;
    return device_spi::Result::unsupported;
#endif
}

device_spi::Result spi_attach(
    device_spi::Bus b, const device_spi::DeviceConfiguration& c, device_spi::Device& d) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::attach(b, c, d);
#else
    (void)b;
    (void)c;
    (void)d;
    return device_spi::Result::unsupported;
#endif
}

device_spi::Result spi_close(device_spi::Bus b) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::close(b);
#else
    (void)b;
    return device_spi::Result::success;
#endif
}

device_spi::Result spi_close(device_spi::Device d) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::close(d);
#else
    (void)d;
    return device_spi::Result::success;
#endif
}

bool spi_active(device_spi::Device d) {
#if USE_ADE7953_PRO1PM || USE_ADE7953_PRO2PM || USE_ADE7953_PRO4PM
    return device_spi::active(d);
#else
    (void)d;
    return false;
#endif
}

bool i2c_available(std::uint8_t bus) {
#if USE_ADE7953_EM || USE_ADE7953_PLUS2PM
    return platform::i2c::available(bus);
#else
    (void)bus;
    return false;
#endif
}

bool i2c_claim(std::uint8_t bus, i2c_devices::Lease& lease) {
#if USE_ADE7953_EM || USE_ADE7953_PLUS2PM
    return i2c_devices::claim(bus, 0x38, lease);
#else
    (void)bus;
    (void)lease;
    return false;
#endif
}

void i2c_release(i2c_devices::Lease lease) {
#if USE_ADE7953_EM || USE_ADE7953_PLUS2PM
    i2c_devices::release(lease);
#else
    (void)lease;
#endif
}

platform::i2c::Result i2c_transfer(std::uint8_t bus, std::uint8_t address, const std::uint8_t* tx,
    std::size_t tx_size, std::uint8_t* rx, std::size_t rx_size) {
#if USE_ADE7953_EM || USE_ADE7953_PLUS2PM
    return platform::i2c::transfer(bus, address, tx, tx_size, rx, rx_size);
#else
    (void)bus;
    (void)address;
    (void)tx;
    (void)tx_size;
    (void)rx;
    (void)rx_size;
    return platform::i2c::Result::unavailable;
#endif
}

constexpr std::uint32_t gain_default = 4194304;
using RegisterCalibration = std::array<std::array<std::int32_t, 6>, 4>;

/** @brief Parses the bounded integer/object subset used by ADE calib.dat.
 * Unknown integer fields are ignored. Invalid syntax/ranges reject the entire file.
 */
class CalibrationParser {
    std::string_view text_;
    RegisterCalibration& values_;
    std::array<bool, 4> voltage_specific_{};

    bool assign(const std::array<std::string_view, 3>& path_, unsigned depth, std::int32_t value) {
        for (unsigned c = 0; c < values_.size(); ++c) {
            int reg = -1;
            if (depth == 1 && path_[0] == "rms") {
                if (path_[1] == "voltage" && !voltage_specific_[c])
                    reg = 0;
                if (path_[1].size() == 9 && path_[1].substr(0, 8) == "voltage_" &&
                    path_[1][8] == static_cast<char>('a' + c)) {
                    reg = 0;
                    voltage_specific_[c] = true;
                }
                if (path_[1].size() == 9 && path_[1].substr(0, 8) == "current_" &&
                    path_[1][8] == static_cast<char>('a' + c))
                    reg = 1;
            } else if (depth == 1 && path_[0] == "angles" && path_[1].size() == 6 &&
                       path_[1].substr(0, 5) == "angle" &&
                       path_[1][5] == static_cast<char>('0' + c))
                reg = 5;
            else if (depth == 2 && path_[0] == "powers" && path_[2].size() == 1 &&
                     path_[2][0] == static_cast<char>('a' + c)) {
                if (path_[1] == "totactive")
                    reg = 2;
                else if (path_[1] == "apparent")
                    reg = 3;
                else if (path_[1] == "reactive")
                    reg = 4;
            }
            if (reg < 0)
                continue;
            if (reg == 5 ? (value < -383 || value > 383) : value < 0)
                return false;
            values_[c][reg] = value;
        }
        return true;
    }

  public:
    CalibrationParser(std::string_view text, RegisterCalibration& values)
        : text_(text), values_(values) {}

    bool parse() {
        auto visitor = [this](const auto& path, unsigned depth, std::int32_t value) {
            return assign(path, depth, value);
        };
        return support::IntegerObjectParser<decltype(visitor)>(text_, visitor).parse();
    }
};

/** @brief Loads once per device creation; malformed files never partially replace defaults. */
void load_register_calibration(RegisterCalibration& values) {
    auto text = std::unique_ptr<char[]>(new (std::nothrow) char[2048]);
    std::size_t size{};
    if (!text) {
        AddLog(LogLevel::info, "ADE: no memory for calib.dat; using register defaults");
        return;
    }
    const auto result = filesystem::read_file("/calib.dat", text.get(), 2048, size);
    if (result == filesystem::ReadResult::not_found)
        return;
    auto updated = values;
    if (result != filesystem::ReadResult::succeeded ||
        !CalibrationParser({text.get(), size}, updated).parse()) {
        AddLog(LogLevel::info, "ADE: calib.dat unavailable or invalid; using register defaults");
        return;
    }
    values = updated;
}

constexpr std::uint16_t calibration_registers[2][6] = {
    {0x381, 0x380, 0x382, 0x384, 0x383, 0x108}, {0x38d, 0x38c, 0x38e, 0x390, 0x38f, 0x109}};
constexpr std::uint16_t data_registers[2][6] = {
    {0x31a, 0x31e, 0x322, 0x320, 0x31c, 0x10e}, {0x31b, 0x31f, 0x323, 0x321, 0x31c, 0x10e}};

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

unsigned register_size(std::uint16_t reg) {
    const auto page = (reg >> 8) & 15;
    return page <= 3 ? page + 1 : (page == 7 || page == 8) ? 1 : 0;
}

std::uint64_t magnitude(std::int32_t value) {
    return value < 0 ? std::uint64_t(-std::int64_t(value)) : std::uint64_t(value);
}
enum class Stage : std::uint8_t { reset, startup, warmup, running };

/** @brief One or two physical chips; GPIO callbacks never run protocol work. */
class Ade7953 final : public Device {
    struct Channel {
        Sample sample{};
        Calibration calibration{};
        ObservationAge age{};
        bool observed{}, reference_valid{};
        std::array<std::uint32_t, 3> reference{};

        std::uint32_t received_at{}, interval_ms{1000};
    };

    std::unique_ptr<Channel[]> channels_;
    PhysicalIdentity identity_{};
    RegisterCalibration register_calibration_{};
    device_spi::Bus spi_bus_{};
    i2c_devices::Lease i2c_lease_{};
    std::array<device_spi::Device, 2> spi_devices_{};
    runtime_pins::Owner gpio_owner_{};
    std::uint32_t due_{}, last_read_{};
    std::uint8_t model_{}, count_{}, i2c_bus_{}, chips_{};
    Stage stage_{Stage::startup};
    bool spi_{}, i2c_claimed_{}, gpio_claimed_{}, gpio_faulted_{};
    bool primary_controlled_{}, primary_on_{};

    /** @brief Keeps each SPI register command and read payload under one CS assertion. */
    bool read(std::uint8_t chip, std::uint16_t reg, std::uint32_t& value) {
        const auto size = register_size(reg);
        if (!size)
            return false;
        std::array<std::uint8_t, 7> tx{}, rx{};
        tx[0] = reg >> 8;
        tx[1] = reg;
        const std::uint8_t* result{};
        if (spi_) {
            tx[2] = 0x80;
            for (unsigned i = 3; i < 3 + size; ++i)
                tx[i] = 0xff;
            if (spi_transfer(spi_devices_[chip], tx.data(), rx.data(), 3 + size) !=
                device_spi::Result::success)
                return false;
            result = rx.data() + 3;
        } else {
            if (i2c_transfer(i2c_bus_, 0x38, tx.data(), 2, rx.data(), size) !=
                platform::i2c::Result::success)
                return false;
            result = rx.data();
        }
        value = 0;
        for (unsigned i = 0; i < size; ++i)
            value = (value << 8) | result[i];
        return true;
    }

    bool write(std::uint8_t chip, std::uint16_t reg, std::uint32_t value) {
        const auto size = register_size(reg);
        if (!size)
            return false;
        std::array<std::uint8_t, 7> bytes{};
        bytes[0] = reg >> 8;
        bytes[1] = reg;
        const auto prefix = spi_ ? 3U : 2U;
        for (unsigned i = 0; i < size; ++i)
            bytes[prefix + i] = value >> (8 * (size - 1 - i));
        if (spi_)
            return spi_transfer(spi_devices_[chip], bytes.data(), nullptr, prefix + size) ==
                   device_spi::Result::success;
        const bool okay = i2c_transfer(i2c_bus_, 0x38, bytes.data(), prefix + size, nullptr, 0) ==
                          platform::i2c::Result::success;
        platform::delay_microseconds(5);
        return okay;
    }

    bool initialize_chip(std::uint8_t chip) {
        const std::pair<std::uint16_t, std::uint32_t> settings[] = {{0x102, 4}, {0xfe, 0xad},
            {0x120, 0x30}, {1, 7}, {0x303, 29196}, {0x304, 29196}, {1, 0}};
        for (const auto& setting : settings)
            if (!write(chip, setting.first, setting.second))
                return false;
        const auto inputs = spi_ ? (model_ == 5 ? 2U : 1U) : 2U;
        for (unsigned input = 0; input < inputs; ++input) {
            /** @brief calib.dat keeps the physical A/B register-set order of the retained variants.
             */
            const auto channel = spi_ ? (model_ == 5 ? chip * 2 + input : chip) : input;
            for (unsigned reg = 0; reg < 6; ++reg) {
                auto value = register_calibration_[channel][reg];
                if (reg == 5 && value < 0)
                    value = -value + 0x200; // PHCAL is sign magnitude, not two's complement.
                if (!write(chip, calibration_registers[input][reg], value))
                    return false;
            }
        }
        return true;
    }

    /** @brief Converts confirmed accumulated active energy directly, without power integration. */
    void energy(Channel& channel, std::size_t index, std::uint64_t raw, bool exporting) {
        const auto power = channel.calibration.power ? channel.calibration.power : 1540U;
        auto& value = exporting ? channel.sample.export_increment : channel.sample.import_increment;
        if (register_calibration_[index][2] != gain_default) {
            value = raw * 1000; // Custom gain: 2.5 accumulated LSB per watt-second.
            channel.sample.increment_denominator = 9;
        } else {
            value = raw * 130083; // Exact pinned 23.41494 correction, in rational micro-Wh.
            channel.sample.increment_denominator = 2 * power;
        }
        channel.sample.increment_fields |= exporting ? 2 : 1;
    }

    void acquire(std::uint32_t now, bool discard) {
        const auto elapsed = last_read_ ? now - last_read_ : 1000U;
        last_read_ = now;
        for (std::size_t channel = 0; channel < count_; ++channel) {
            auto& state = channels_[channel];
            const std::uint8_t chip = spi_ ? (model_ == 5 ? channel / 2 : channel) : 0;
            const auto input = spi_ ? (model_ == 5 ? channel % 2 : 0) : channel;
            std::array<std::uint32_t, 6> raw{};
            bool okay = true;
            // Read instantaneous values and signs before consuming any energy register.
            const unsigned order[]{0, 4, 5, 1, 2, 3};
            std::uint32_t mode{};
            bool active_read{};
            for (const auto index : order) {
                const auto result = read(chip, data_registers[input][index], raw[index]);
                okay &= result;
                if (index == 1)
                    active_read = result;
            }
            /** Read-to-clear registers must be consumed even when ACCMODE fails, especially
             * OFF: leaving them unread could incorrectly charge that energy after ON resumes.
             * A missing EM direction makes its confirmed energy uninterpretable, not import.
             */
            const bool mode_valid = spi_ || read(chip, 0x301, mode);
            okay &= mode_valid;
            if (discard)
                continue;
            const bool exporting = model_ == 1 && ((mode & (1U << (10 + channel))) ||
                                                      static_cast<std::int32_t>(raw[1]) < 0);
            /** OFF reads consume hardware accumulators but never bank their energy for ON. */
            if (active_read && (model_ != 1 || mode_valid) && (!primary_controlled_ || primary_on_))
                energy(state, channel, magnitude(static_cast<std::int32_t>(raw[1])), exporting);
            if (!okay || !elapsed || elapsed > 5000) {
                state.sample.valid = false;
                state.reference = {};
                state.reference_valid = false;
                continue;
            }
            /** Calibration references follow every successful raw acquisition, even OFF.
             * Their presence/freshness is independent of the retained published arrays.
             */
            state.reference = {
                std::uint32_t(magnitude(static_cast<std::int32_t>(raw[1]))), raw[4], raw[0]};
            state.received_at = now;
            state.interval_ms = elapsed;
            state.reference_valid = true;
            if (primary_controlled_ && !primary_on_) {
                state.sample.valid = false;
                state.sample.source_retained = !state.sample.watchdog_reset;
                continue;
            }
            state.observed = primary_controlled_;
            auto& s = state.sample;
            s.watchdog_reset = s.source_retained = false;
            const auto pc = state.calibration.power ? state.calibration.power : 1540U;
            const auto& registers = register_calibration_[channel];
            const auto vc = registers[0] != gain_default ? 10000U
                            : state.calibration.voltage  ? state.calibration.voltage
                                                         : 26000U;
            const auto ic = registers[1] != gain_default ? 10000U
                            : state.calibration.current  ? state.calibration.current
                                                         : 10000U;
            // Read-to-clear samples cover the measured interval on both transports.
            const long double multiplier = 234149.4L / pc * 1000 / elapsed;
            const long double custom_multiplier = 400000.0L / elapsed;
            s.voltage = saturate_i32(std::uint64_t(raw[4]) * 1000 / vc);
            s.current = raw[1] ? saturate_i32(std::uint64_t(raw[0]) * 100 / ic) : 0;
            s.power = static_cast<std::int64_t>(
                magnitude(static_cast<std::int32_t>(raw[1])) *
                (registers[2] != gain_default ? custom_multiplier : multiplier));
            s.apparent_mva = static_cast<std::int64_t>(
                magnitude(static_cast<std::int32_t>(raw[2])) *
                (registers[3] != gain_default ? custom_multiplier : multiplier));
            s.reactive_mvar = static_cast<std::int64_t>(
                magnitude(static_cast<std::int32_t>(raw[3])) *
                (registers[4] != gain_default ? custom_multiplier : multiplier));
            if (exporting)
                s.power = -s.power;
            if (model_ == 1) {
                if (mode & (1U << (18 + channel * 3)))
                    s.reactive_mvar = 0;
                else if (mode & (1U << (12 + channel)))
                    s.reactive_mvar = -s.reactive_mvar;
            }
            s.frequency_millihz = std::uint32_t(223750000ULL / (std::uint64_t(raw[5]) + 1));
            s.power_factor_milli =
                s.apparent_mva ? static_cast<std::int32_t>(std::clamp(
                                     static_cast<long double>(s.power) * 1000 / s.apparent_mva,
                                     -1000.0L, 1000.0L))
                               : 0;
            s.available_fields = 7;
            s.auxiliary_fields = 1 | 2 | 16 | 32;
            s.valid = true;
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

    /** @brief Pro 1PM/2PM measure separate inputs; the other models share V/F. */
    bool common_voltage() const override { return model_ != 3 && model_ != 4; }

    /** @brief Shares frequency according to the source model topology. */
    bool common_frequency() const override { return common_voltage(); }

    /** @brief Restores the pinned model's runtime local-export reporting default. */
    bool export_reporting_default() const override { return model_ == 1; }

    Ade7953(const Configuration& c, std::uint8_t model)
        : channels_(new (std::nothrow) Channel[c.channels]), identity_(c.identity),
          due_(platform::monotonic_milliseconds() + 2000), model_(model), count_(c.channels),
          i2c_bus_(c.identity.bus - 1), chips_(model >= 4 ? 2 : 1), spi_(model >= 3) {
        for (auto& registers : register_calibration_) {
            registers.fill(gain_default);
            registers[5] = model_ == 1 ? 200 : 0;
        }
        load_register_calibration(register_calibration_);
        if (channels_)
            for (unsigned channel = 0; channel < count_; ++channel) {
                channels_[channel].calibration = c.calibration;
                channels_[channel].sample.use_absolute = true;
                channels_[channel].sample.available_fields = 0;
            }
    }

    ~Ade7953() override {
        for (const auto target : spi_devices_)
            (void)spi_close(target);
        (void)spi_close(spi_bus_);
        if (i2c_claimed_)
            i2c_release(i2c_lease_);
        if (gpio_claimed_) {
            bool cleaned = true;
            for (const auto role : {PinFunction::reset, PinFunction::interrupt}) {
                const auto pin = identity_.pin(role);
                if (pin.valid() && platform::gpio_configure_input(pin, platform::GpioPull::none) !=
                                       platform::GpioResult::success)
                    cleaned = false;
            }
            if (cleaned)
                runtime_pins::release(gpio_owner_);
        }
    }

    bool open(const Configuration& c) {
        const auto expected = model_ == 3 ? 1 : model_ == 5 ? 4 : 2;
        if (!channels_ || c.channels != expected || c.options.flags ||
            c.options.invalid_power_frames != 128 || !calibrate(c.calibration))
            return false;
        for (const auto& binding : identity_.pins) {
            const auto role = binding.function;
            if (role == PinFunction::none || role == PinFunction::reset ||
                role == PinFunction::interrupt)
                continue;
            if (!spi_ || (role != PinFunction::spi_clock && role != PinFunction::spi_miso &&
                             role != PinFunction::spi_mosi && role != PinFunction::spi_cs &&
                             role != PinFunction::spi_cs2))
                return false;
        }
        if (spi_) {
            if (identity_.bus || identity_.address ||
                !identity_.pin(PinFunction::spi_miso).valid() ||
                !identity_.pin(PinFunction::spi_mosi).valid())
                return false;
            if (spi_open(
                    {identity_.pin(PinFunction::spi_clock), identity_.pin(PinFunction::spi_mosi),
                        identity_.pin(PinFunction::spi_miso)},
                    spi_bus_) != device_spi::Result::success)
                return false;
            for (unsigned chip = 0; chip < chips_; ++chip) {
                device_spi::DeviceConfiguration config{};
                config.select = identity_.pin(chip ? PinFunction::spi_cs2 : PinFunction::spi_cs);
                config.frequency_hz = 250000;
                if (spi_attach(spi_bus_, config, spi_devices_[chip]) != device_spi::Result::success)
                    return false;
            }
            if (chips_ == 1 && identity_.pin(PinFunction::spi_cs2).valid())
                return false;
        } else {
            if (!identity_.bus || identity_.bus > platform::i2c::kBusCount ||
                identity_.address != 0x38 || !i2c_available(i2c_bus_) ||
                !i2c_claim(i2c_bus_, i2c_lease_))
                return false;
            i2c_claimed_ = true;
        }
        std::array<PinId, 2> pins{};
        std::size_t used{};
        for (const auto role : {PinFunction::reset, PinFunction::interrupt})
            if (identity_.pin(role).valid())
                pins[used++] = identity_.pin(role);
        if (used) {
            gpio_owner_ = runtime_pins::allocate_energy_owner();
            if (!runtime_pins::claim(gpio_owner_, pins.data(), used))
                return false;
            gpio_claimed_ = true;
            const auto irq = identity_.pin(PinFunction::interrupt),
                       reset = identity_.pin(PinFunction::reset);
            if (irq.valid() && platform::gpio_configure_input(irq, platform::GpioPull::none) !=
                                   platform::GpioResult::success) {
                gpio_faulted_ = true;
                return false;
            }
            if (reset.valid()) {
                if (platform::gpio_configure_output(reset, false) !=
                    platform::GpioResult::success) {
                    gpio_faulted_ = true;
                    return false;
                }
                stage_ = Stage::reset;
                due_ = platform::monotonic_milliseconds() + 1;
            }
        }
        return true;
    }

    bool online() const override {
        if (gpio_faulted_)
            return false;
        if (!spi_)
            return i2c_claimed_ && i2c_available(i2c_bus_);
        for (unsigned chip = 0; chip < chips_; ++chip)
            if (!spi_active(spi_devices_[chip]))
                return false;
        return true;
    }

    bool independent_calibration() const override { return true; }

    bool coefficient_reset_alias() const override { return true; }

    std::uint32_t current_reference_scale() const override { return 100; }

    std::uint32_t default_coefficient(CalibrationField field) const override {
        switch (field) {
        case CalibrationField::power:
            return 1540;
        case CalibrationField::voltage:
            return 26000;
        case CalibrationField::current:
            return 10000;
        default:
            return 0;
        }
    }

    bool calibrate_channel(std::size_t channel, const Calibration& c) override {
        if (channel >= count_ || c.power > INT32_MAX || c.voltage > INT32_MAX ||
            c.current > INT32_MAX)
            return false;
        channels_[channel].calibration = c;
        return true;
    }

    bool calibrate(const Calibration& c) override {
        if (c.power > INT32_MAX || c.voltage > INT32_MAX || c.current > INT32_MAX)
            return false;
        for (unsigned channel = 0; channel < count_; ++channel)
            channels_[channel].calibration = c;
        return true;
    }

    bool calibration_for(
        CalibrationField field, std::uint32_t reference, Calibration& output) const override {
        return calibration_for_channel(0, field, reference, output);
    }

    bool calibration_for_channel(std::size_t channel, CalibrationField field,
        std::uint32_t reference, Calibration& output) const override {
        if (channel >= count_)
            return false;
        if (stage_ != Stage::running || !channels_[channel].reference_valid ||
            (!primary_controlled_ && !channels_[channel].sample.valid) ||
            platform::monotonic_milliseconds() - channels_[channel].received_at > 3000)
            return false;
        std::uint64_t coefficient{};
        if (field == CalibrationField::power && reference > 1000 && reference < 20000000)
            coefficient = std::uint64_t(channels_[channel].reference[0]) * 234149400 /
                          (std::uint64_t(reference) * channels_[channel].interval_ms);
        else if (field == CalibrationField::voltage && reference > 100000 && reference < 400000)
            coefficient = std::uint64_t(channels_[channel].reference[1]) * 1000 / reference;
        else if (field == CalibrationField::current && reference > 2000 && reference < 10000000)
            coefficient = (std::uint64_t(channels_[channel].reference[2]) * 100 / reference) * 100;
        else
            return false;
        if (!coefficient || coefficient > INT32_MAX)
            return false;
        if (field == CalibrationField::power)
            output.power = coefficient;
        else if (field == CalibrationField::voltage)
            output.voltage = coefficient;
        else
            output.current = coefficient;
        return true;
    }

#if USE_ENERGY_POWER_POLICY
    /** @brief Gates only primary publication/accounting; raw accumulator reads continue OFF. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
        for (unsigned c = 0; c < count_; ++c) {
            auto& s = channels_[c].sample;
            channels_[c].observed = false;
            s.increment_fields = 0;
            s.import_increment = {};
            s.export_increment = {};
            if (now - channels_[c].received_at > 3000) {
                if (!primary_controlled_) {
                    s.valid = false;
                    s.available_fields = 0;
                }
                channels_[c].reference = {};
                channels_[c].reference_valid = false;
            }
        }
        if (online() && reached(now, due_)) {
            if (stage_ == Stage::reset) {
                const auto reset = identity_.pin(PinFunction::reset);
                if (platform::gpio_write(reset, true) != platform::GpioResult::success)
                    gpio_faulted_ = true;
                stage_ = Stage::startup;
                due_ = now + 2000;
            } else if (stage_ == Stage::startup) {
                bool initialized = true;
                for (unsigned chip = 0; chip < chips_; ++chip)
                    initialized &= initialize_chip(chip);
                if (initialized) {
                    stage_ = Stage::warmup;
                    due_ = now + 1000;
                } else
                    due_ = now + 5000;
            } else {
                acquire(now, stage_ == Stage::warmup);
                if (stage_ == Stage::warmup)
                    stage_ = Stage::running;
                due_ = now + 1000;
            }
        }
        for (std::size_t c = 0; c < count_; ++c) {
            auto& state = channels_[c];
            auto& s = state.sample;
            if (primary_controlled_) {
                /** Read failures/OFF do not renew the source per-phase watchdog. Retained
                 * arrays remain visible until fifth-tick reset; physical validity stays separate.
                 */
                (void)state.age.poll(now, state.observed);
                if (state.age.expired_now()) {
                    s.voltage = s.current = s.power = 0;
                    s.frequency_millihz = 0;
                    s.apparent_mva = s.reactive_mvar = s.power_factor_milli = 0;
                    s.valid = false;
                    s.watchdog_reset = true;
                    s.source_retained = false;
                }
                s.available_fields = 7;
                s.auxiliary_fields = 1 | 2 | 16 | 32;
                if (!s.valid && !s.watchdog_reset)
                    s.source_retained = true;
            }
            if (c < count)
                samples[c] = s;
        }
    }
};

std::unique_ptr<Device> create(const Configuration& c, std::uint8_t model) {
    auto meter = std::unique_ptr<Ade7953>(new (std::nothrow) Ade7953(c, model));
    if (!meter || !meter->open(c))
        return {};
    return meter;
}
} // namespace
#if USE_ADE7953_EM
/** @brief Creates the bidirectional two-channel Shelly EM I2C mapping. */
std::unique_ptr<Device> create_ade7953_em(const Configuration& c) {
    return create(c, 1);
}
#endif
#if USE_ADE7953_PLUS2PM
/** @brief Creates the Shelly Plus 2PM/Gen3 I2C mapping. */
std::unique_ptr<Device> create_ade7953_plus2pm(const Configuration& c) {
    return create(c, 2);
}
#endif
#if USE_ADE7953_PRO1PM
/** @brief Creates the single-chip Pro 1PM SPI mapping. */
std::unique_ptr<Device> create_ade7953_pro1pm(const Configuration& c) {
    return create(c, 3);
}
#endif
#if USE_ADE7953_PRO2PM
/** @brief Creates the two-chip Pro 2PM SPI mapping. */
std::unique_ptr<Device> create_ade7953_pro2pm(const Configuration& c) {
    return create(c, 4);
}
#endif
#if USE_ADE7953_PRO4PM
/** @brief Creates the two-chip, four-channel Pro 4PM SPI mapping. */
std::unique_ptr<Device> create_ade7953_pro4pm(const Configuration& c) {
    return create(c, 5);
}
#endif
} // namespace newmota::energy
