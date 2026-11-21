/*
  mcp39f501.cpp - MCP39F501 single-phase energy meter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol and calibration behavior reference: Tasmota xnrg_04_mcp39f501.ino
  Copyright (C) 2021 Theo Arends
  The calibration register layout is based on the Microchip MCP39F501 data sheet.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file mcp39f501.cpp
 * @brief Polls and calibrates an MCP39F501 through an exclusively owned UART.
 */

#include "services/energy/energy_service.h"
#include "services/energy/energy_observation_age.h"
#include "core/hardware/runtime_pin_claims.h"
#include "drivers/serial/serial_service.h"
#include "newmota/log.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <array>
#include <initializer_list>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::uint32_t kBaud = 4800;
constexpr std::uint32_t kInitialDelayMs = 3000;
constexpr std::uint32_t kReplyTimeoutMs = 1000;
constexpr std::uint32_t kFrameGapMs = (24000 / kBaud) + 1;
constexpr std::uint32_t kPollIntervalMs = 5000;
constexpr std::uint32_t kCalibrationDelayMs = 7000;
constexpr std::uint32_t kFreshnessMs = 6000;
constexpr std::uint16_t kCalibrationBase = 0x0028;
constexpr std::uint8_t kCalibrationLength = 52;
constexpr std::uint16_t kFrequencyReference = 0x0094;
constexpr std::uint16_t kFrequencyGain = 0x00ae;
constexpr std::uint8_t kStartFrame = 0xa5;
constexpr std::uint8_t kAckFrame = 0x06;
constexpr std::uint8_t kNakFrame = 0x15;
constexpr std::uint8_t kCrcError = 0x51;
constexpr std::uint8_t kSingleWireFrame = 0xab;
constexpr std::uint32_t kSingleWireFlag = 0x00000100;
constexpr std::size_t kMaximumResponse = 60;
constexpr std::size_t kMaximumCommand = 64;

enum class Stage : std::uint8_t { calibration, frequency, address, running };
enum class Request : std::uint8_t {
    none,
    address,
    data,
    startup_calibration,
    calibration,
    startup_frequency,
    frequency,
    system_pointer,
    system_value,
    interval_pointer,
    interval_value,
    calibration_pointer,
    calibration_value,
    calibration_save,
    frequency_reference_pointer,
    frequency_reference_value,
    frequency_gain_pointer,
    frequency_gain_value,
    frequency_save
};

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

std::uint16_t little16(const std::uint8_t* data) {
    return std::uint16_t(data[0]) | (std::uint16_t(data[1]) << 8);
}

std::uint32_t little32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
           (std::uint32_t(data[3]) << 24);
}

void put_little16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8);
}

void put_little32(std::uint8_t* data, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        data[i] = static_cast<std::uint8_t>(value >> (i * 8));
}

std::uint8_t checksum(const std::uint8_t* data, std::size_t size) {
    std::uint8_t sum{};
    for (std::size_t i = 0; i + 1 < size; ++i)
        sum = static_cast<std::uint8_t>(sum + data[i]);
    return sum;
}

/** @brief Owns one MCP39F501 protocol session and its optional active-low reset pad. */
class Mcp39f501 final : public Device {
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

    explicit Mcp39f501(const Configuration& configuration) : identity_(configuration.identity) {
        now_ = platform::monotonic_milliseconds();
        due_ = now_ + kInitialDelayMs;
        reset_ = identity_.pin(PinFunction::reset);
    }

    ~Mcp39f501() override {
        (void)serial::request_close(handle_);
        if (reset_claimed_ && !reset_faulted_) {
            if (platform::gpio_configure_input(reset_, platform::GpioPull::none) ==
                platform::GpioResult::success)
                runtime_pins::release(reset_owner_);
            else
                reset_faulted_ = true;
        }
    }

    bool open(const Configuration& configuration) {
        if (configuration.channels != 1 || configuration.identity.bus ||
            configuration.identity.address || !calibrate(configuration.calibration) ||
            !set_options(configuration.options))
            return false;
        for (const auto& binding : configuration.identity.pins)
            if (binding.function != PinFunction::none && binding.function != PinFunction::uart_rx &&
                binding.function != PinFunction::uart_tx && binding.function != PinFunction::reset)
                return false;
        const auto rx = identity_.pin(PinFunction::uart_rx);
        const auto tx = identity_.pin(PinFunction::uart_tx);
        if (!rx.valid() || !tx.valid())
            return false;
        serial::Config config{};
        config.rx = rx;
        config.tx = tx;
        config.baud = kBaud;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.parity = platform::serial::Parity::none;
        config.rx_capacity = 128;
        if (serial::open(config, handle_) != serial::Result::success)
            return false;
        if (reset_.valid()) {
            reset_owner_ = runtime_pins::allocate_energy_owner();
            if (!runtime_pins::claim(reset_owner_, &reset_, 1))
                return false;
            reset_claimed_ = true;
            if (platform::gpio_configure_output(reset_, false) != platform::GpioResult::success) {
                reset_faulted_ = true;
                return false;
            }
            reset_pending_ = true;
            reset_release_at_ = now_ + 100;
        }
        return true;
    }

    bool online() const override { return serial::active(handle_) && !reset_faulted_; }

    bool calibrate(const Calibration& calibration) override {
        // The MCP39F501 stores its calibration in the device's own nonvolatile registers.
        return !calibration.power && !calibration.voltage && !calibration.current;
    }

    bool set_options(const Options& options) override {
        return !options.flags && options.invalid_power_frames == 128;
    }

    bool configuration_locked() const override {
        return control_.phase == ControlPhase::queued || control_.phase == ControlPhase::committing;
    }

    /** @brief Gates calibration on raw meter registers, including measurements received OFF.
     * Published source arrays can be retained/zeroed independently and are not references.
     */
    CalibrationRequest request_calibration(
        CalibrationField field, std::uint32_t reference) override {
        if (field == CalibrationField::frequency) {
            if (reference <= 45000 || reference >= 65000 || !frequency_raw_)
                return CalibrationRequest::rejected;
        } else {
            if (field != CalibrationField::power && field != CalibrationField::voltage &&
                field != CalibrationField::current)
                return CalibrationRequest::rejected;
            const auto measured = field == CalibrationField::power     ? power_raw_
                                  : field == CalibrationField::voltage ? voltage_raw_
                                                                       : current_raw_;
            if (!measured)
                return CalibrationRequest::rejected;
            if ((field == CalibrationField::power &&
                    (reference <= 1000 || reference >= 20000000)) ||
                (field == CalibrationField::voltage &&
                    (reference <= 100000 || reference >= 400000)) ||
                (field == CalibrationField::current &&
                    (reference <= 10000 || reference >= 80000000)))
                return CalibrationRequest::rejected;
        }
        if (!online() || calibration_requested_ || configuration_locked())
            return CalibrationRequest::rejected;
        requested_field_ = field;
        requested_reference_ = reference;
        calibration_requested_ = true;
        control_ = {ControlPhase::queued, Control::calibration, reference, 0, 0, false, field};
        due_ = now_;
        return CalibrationRequest::queued;
    }

    ControlStatus control_status() const override { return control_; }

#if USE_ENERGY_POWER_POLICY
    /** @brief Primary source power gates publication, never reception of raw MCP registers. */
    bool set_primary_power(bool on) override {
        primary_controlled_ = true;
        primary_on_ = on;
        return true;
    }
#endif

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
        now_ = now;
        primary_observed_ = false;
        update_reset();
        check_serial_status();
        receive();
        if (!primary_controlled_)
            expire_sample();
        if (response_pending_ && reached(now_, response_deadline_))
            response_timeout();
        if (tx_pending_)
            transmit();
        if (!tx_pending_ && !response_pending_ && !reset_pending_ && !reset_faulted_ &&
            reached(now_, due_))
            schedule();
        if (primary_controlled_) {
            /** OFF frames refresh raw registers but not source observation age. Each actual
             * second hook first clears stale raw inputs, then accounts the retained centi-W
             * register in source integer order. No gap duration or ordinary power integration.
             */
            const bool second = primary_age_.poll(now_, primary_observed_);
            if (primary_age_.expired_now()) {
                sample_.voltage = sample_.current = sample_.power = 0;
                sample_.frequency_millihz = 0;
                sample_.valid = false;
                sample_.watchdog_reset = true;
                sample_.source_retained = false;
            }
            if (second && primary_age_.stale())
                current_raw_ = voltage_raw_ = power_raw_ = frequency_raw_ = 0;
            auto sample = sample_;
            sample.available_fields = 7;
            sample.auxiliary_fields |= 1;
            sample.use_absolute = true;
            if (!sample.valid && !sample.watchdog_reset)
                sample.source_retained = true;
            if (second && power_raw_) {
                sample.import_increment = (std::uint64_t(power_raw_) * 10 / 36) * 10;
                sample.increment_fields = 1;
            }
            if (count)
                samples[0] = sample;
        } else if (count)
            samples[0] = sample_;
    }

  private:
    bool queue_packet(Request request, std::initializer_list<std::uint8_t> commands) {
        if (commands.size() + 3 > transmit_.size())
            return false;
        std::size_t offset = 2;
        for (const auto byte : commands)
            transmit_[offset++] = byte;
        return finish_packet(request, offset + 1);
    }

    bool finish_packet(Request request, std::size_t size) {
        if (size < 4 || size > transmit_.size())
            return false;
        transmit_[0] = kStartFrame;
        transmit_[1] = static_cast<std::uint8_t>(size);
        transmit_[size - 1] = checksum(transmit_.data(), size);
        transmit_size_ = size;
        transmit_offset_ = 0;
        transmit_kind_ = request;
        tx_pending_ = true;
        response_pending_ = false;
        (void)serial::discard_rx(handle_);
        return true;
    }

    void schedule() {
        if (calibration_requested_) {
            if (requested_field_ == CalibrationField::frequency)
                (void)queue_packet(
                    Request::frequency, {0x41, 0x00, 0x94, 0x52, 0x41, 0x00, 0xae, 0x52});
            else
                (void)queue_packet(
                    Request::calibration, {0x41, 0x00, 0x28, 0x4e, kCalibrationLength});
            return;
        }
        switch (stage_) {
        case Stage::calibration:
            (void)queue_packet(
                Request::startup_calibration, {0x41, 0x00, 0x28, 0x4e, kCalibrationLength});
            break;
        case Stage::frequency:
            (void)queue_packet(
                Request::startup_frequency, {0x41, 0x00, 0x94, 0x52, 0x41, 0x00, 0xae, 0x52});
            break;
        case Stage::address:
            (void)queue_packet(Request::address, {0x41, 0x00, 0x26, 0x52});
            break;
        case Stage::running:
            (void)queue_packet(Request::data, {0x41, 0x00, 0x04, 0x4e, 22});
            break;
        }
    }

    void queue_system_configuration() {
        (void)queue_packet(Request::system_pointer, {0x41, 0x00, 0x42});
    }

    void queue_system_value() {
        (void)queue_packet(
            Request::system_value, {0x45, static_cast<std::uint8_t>(system_configuration_ >> 24),
                                       static_cast<std::uint8_t>(system_configuration_ >> 16),
                                       static_cast<std::uint8_t>(system_configuration_ >> 8),
                                       static_cast<std::uint8_t>(system_configuration_)});
    }

    void queue_interval_pointer() {
        (void)queue_packet(Request::interval_pointer, {0x41, 0x00, 0x5a});
    }

    void queue_interval_value() { (void)queue_packet(Request::interval_value, {0x57, 0x00, 0x02}); }

    void queue_calibration_write() {
        control_.phase = ControlPhase::committing;
        (void)queue_packet(
            Request::calibration_pointer, {0x41, static_cast<std::uint8_t>(kCalibrationBase >> 8),
                                              static_cast<std::uint8_t>(kCalibrationBase)});
    }

    void queue_calibration_value() {
        std::size_t offset = 2;
        transmit_[offset++] = 0x4d;
        transmit_[offset++] = kCalibrationLength;
        for (const auto byte : calibration_registers_)
            transmit_[offset++] = byte;
        (void)finish_packet(Request::calibration_value, offset + 1);
    }

    void queue_save(Request request) { (void)queue_packet(request, {0x53, address_}); }

    void queue_frequency_write() {
        control_.phase = ControlPhase::committing;
        (void)queue_packet(Request::frequency_reference_pointer,
            {0x41, static_cast<std::uint8_t>(kFrequencyReference >> 8),
                static_cast<std::uint8_t>(kFrequencyReference)});
    }

    void queue_frequency_reference_value() {
        (void)queue_packet(Request::frequency_reference_value,
            {0x57, static_cast<std::uint8_t>(frequency_reference_ >> 8),
                static_cast<std::uint8_t>(frequency_reference_)});
    }

    void queue_frequency_gain_pointer() {
        (void)queue_packet(
            Request::frequency_gain_pointer, {0x41, static_cast<std::uint8_t>(kFrequencyGain >> 8),
                                                 static_cast<std::uint8_t>(kFrequencyGain)});
    }

    void queue_frequency_gain_value() {
        (void)queue_packet(
            Request::frequency_gain_value, {0x57, static_cast<std::uint8_t>(frequency_gain_ >> 8),
                                               static_cast<std::uint8_t>(frequency_gain_)});
    }

    void transmit() {
        if (!tx_pending_ || transmit_offset_ >= transmit_size_)
            return;
        const auto result = serial::write_some(
            handle_, transmit_.data() + transmit_offset_, transmit_size_ - transmit_offset_);
        transmit_offset_ += result.count;
        if (result.result != serial::Result::success) {
            tx_pending_ = false;
            fail_transaction(unsigned(result.result));
            due_ = now_ + kPollIntervalMs;
            return;
        }
        if (transmit_offset_ == transmit_size_) {
            tx_pending_ = false;
            response_pending_ = true;
            waiting_kind_ = transmit_kind_;
            response_deadline_ = now_ + kReplyTimeoutMs;
        }
    }

    void receive() {
        std::array<std::uint8_t, 128> input{};
        const auto transfer = serial::read(handle_, input.data(), input.size());
        if (transfer.result != serial::Result::success) {
            frame_size_ = 0;
            return;
        }
        for (std::size_t i = 0; i < transfer.count; ++i) {
            const auto byte = input[i];
            if (!frame_size_ && byte != kAckFrame && byte != kNakFrame && byte != kCrcError &&
                byte != kSingleWireFrame)
                continue;
            if (frame_size_ == frame_.size()) {
                frame_size_ = 0;
                fail_transaction(unsigned(serial::Result::failed));
                continue;
            }
            frame_[frame_size_++] = byte;
            last_byte_at_ = now_;
            if (frame_size_ >= 2 && frame_[0] == kAckFrame && frame_[1] == frame_size_)
                process_frame();
        }
        if (frame_size_ && now_ - last_byte_at_ >= kFrameGapMs)
            process_frame();
    }

    void process_frame() {
        const auto size = frame_size_;
        frame_size_ = 0;
        if (size == 1) {
            if (frame_[0] == kAckFrame)
                accept_ack();
            else if (frame_[0] == kNakFrame || frame_[0] == kCrcError)
                fail_transaction(frame_[0]);
            else if (frame_[0] == kSingleWireFrame)
                response_pending_ = false;
            return;
        }
        if (size < 3 || frame_[0] != kAckFrame || frame_[1] != size ||
            checksum(frame_.data(), size) != frame_[size - 1]) {
            fail_transaction(unsigned(serial::Result::failed));
            return;
        }
        if (!response_pending_) // Ignore late or unsolicited ACK frames.
            return;
        const auto request = waiting_kind_;
        response_pending_ = false;
        switch (request) {
        case Request::address:
            if (size != 5) {
                retry_startup();
                break;
            }
            address_ = frame_[3];
            if (address_) {
                stage_ = Stage::running;
                due_ = now_ + 1000;
            } else
                due_ = now_ + kPollIntervalMs;
            break;
        case Request::data:
            if (size == 25)
                decode_data();
            else
                due_ = now_ + kPollIntervalMs;
            break;
        case Request::startup_calibration:
            if (size != kCalibrationLength + 3) {
                retry_startup();
                break;
            }
            copy_calibration();
            system_configuration_ = little32(calibration_registers_.data() + 28);
            if (system_configuration_ & kSingleWireFlag) {
                system_configuration_ &= ~kSingleWireFlag;
                queue_system_configuration();
            } else {
                stage_ = Stage::frequency;
                due_ = now_ + 3000;
            }
            break;
        case Request::calibration:
            if (size != kCalibrationLength + 3) {
                fail_calibration(unsigned(serial::Result::failed));
                break;
            }
            copy_calibration();
            if (apply_reference())
                queue_calibration_write();
            else
                fail_calibration(unsigned(serial::Result::invalid_argument));
            break;
        case Request::startup_frequency:
            if (size != 7) {
                retry_startup();
                break;
            }
            frequency_reference_ = big16(frame_.data() + 2);
            frequency_gain_ = big16(frame_.data() + 4);
            stage_ = Stage::address;
            due_ = now_ + kCalibrationDelayMs;
            break;
        case Request::frequency:
            if (size != 7) {
                fail_calibration(unsigned(serial::Result::failed));
                break;
            }
            frequency_reference_ = big16(frame_.data() + 2);
            frequency_gain_ = big16(frame_.data() + 4);
            if (apply_frequency_reference())
                queue_frequency_write();
            else
                fail_calibration(unsigned(serial::Result::invalid_argument));
            break;
        default:
            retry_startup();
            break;
        }
    }

    static std::uint16_t big16(const std::uint8_t* data) {
        return (std::uint16_t(data[0]) << 8) | data[1];
    }

    void copy_calibration() {
        for (std::size_t i = 0; i < calibration_registers_.size(); ++i)
            calibration_registers_[i] = frame_[i + 2];
    }

    /** @brief Always retains raw data; only ON primary frames publish and renew data_valid. */
    void decode_data() {
        current_raw_ = little32(frame_.data() + 2);
        voltage_raw_ = little16(frame_.data() + 6);
        power_raw_ = little32(frame_.data() + 8);
        frequency_raw_ = little16(frame_.data() + 22);
        last_data_at_ = now_;
        due_ = now_ + kPollIntervalMs;
        if (primary_controlled_ && !primary_on_) {
            sample_.valid = false;
            sample_.source_retained = !sample_.watchdog_reset;
            return;
        }
        primary_observed_ = primary_controlled_;
        sample_.valid = true;
        sample_.watchdog_reset = sample_.source_retained = false;
        sample_.available_fields = 7;
        sample_.voltage = std::int64_t(voltage_raw_) * 100;
        sample_.power = std::int64_t(power_raw_) * 10;
        sample_.current = power_raw_ ? current_raw_ / 10 : 0;
        sample_.frequency_millihz = frequency_raw_;
        sample_.auxiliary_fields |= 1;
    }

    bool apply_reference() {
        std::uint32_t expected{}, measured{};
        std::size_t gain_offset{}, range_shift{};
        if (requested_field_ == CalibrationField::power) {
            expected = requested_reference_ / 10; // milli-W to MCP centi-W
            measured = power_raw_;
            gain_offset = 4;
            range_shift = 16;
            put_little32(calibration_registers_.data() + 44, expected);
        } else if (requested_field_ == CalibrationField::voltage) {
            expected = requested_reference_ / 100; // milli-V to MCP deci-V
            measured = voltage_raw_;
            gain_offset = 2;
            range_shift = 0;
            put_little16(calibration_registers_.data() + 42, static_cast<std::uint16_t>(expected));
        } else {
            expected = requested_reference_ / 100; // milli-A to MCP deci-A
            measured = current_raw_;
            gain_offset = 0;
            range_shift = 8;
            put_little32(calibration_registers_.data() + 38, expected);
        }
        return adjust_gain(expected, measured, gain_offset, range_shift);
    }

    bool adjust_gain(std::uint32_t expected, std::uint32_t measured, std::size_t gain_offset,
        std::size_t range_shift) {
        if (!expected || !measured)
            return false;
        auto gain = std::uint32_t(little16(calibration_registers_.data() + gain_offset));
        auto range =
            std::uint32_t((little32(calibration_registers_.data() + 34) >> range_shift) & 0xffU);
        for (unsigned iteration = 0; iteration < 32; ++iteration) {
            const auto new_gain = std::uint64_t(gain) * expected / measured;
            if (new_gain < 25000 && measured > 6) {
                if (range == 255)
                    return false;
                ++range;
                measured /= 2;
                continue;
            }
            if (new_gain > 55000) {
                if (!range || measured > UINT32_MAX / 2)
                    return false;
                --range;
                measured *= 2;
                continue;
            }
            if (new_gain > UINT16_MAX)
                return false;
            put_little16(
                calibration_registers_.data() + gain_offset, static_cast<std::uint16_t>(new_gain));
            auto full_range = little32(calibration_registers_.data() + 34);
            full_range &= ~(0xffU << range_shift);
            full_range |= range << range_shift;
            put_little32(calibration_registers_.data() + 34, full_range);
            return true;
        }
        return false;
    }

    /** @brief Scales device gain from the latest raw frequency, independent of publication. */
    bool apply_frequency_reference() {
        auto measured = frequency_raw_;
        auto gain = frequency_gain_;
        if (measured == 0xffff || !gain) {
            measured = 50000;
            gain = 0x8000;
        }
        if (!measured)
            return false;
        const auto updated = std::uint64_t(gain) * requested_reference_ / measured;
        if (!updated || updated > UINT16_MAX)
            return false;
        frequency_reference_ = static_cast<std::uint16_t>(requested_reference_);
        frequency_gain_ = static_cast<std::uint16_t>(updated);
        return true;
    }

    void accept_ack() {
        if (!response_pending_)
            return;
        response_pending_ = false;
        switch (waiting_kind_) {
        case Request::system_pointer:
            queue_system_value();
            break;
        case Request::system_value:
            queue_interval_pointer();
            break;
        case Request::interval_pointer:
            queue_interval_value();
            break;
        case Request::interval_value:
            stage_ = Stage::frequency;
            due_ = now_ + kPollIntervalMs;
            break;
        case Request::calibration_pointer:
            queue_calibration_value();
            break;
        case Request::calibration_value:
            queue_save(Request::calibration_save);
            break;
        case Request::calibration_save:
            complete_calibration();
            break;
        case Request::frequency_reference_pointer:
            queue_frequency_reference_value();
            break;
        case Request::frequency_reference_value:
            queue_frequency_gain_pointer();
            break;
        case Request::frequency_gain_pointer:
            queue_frequency_gain_value();
            break;
        case Request::frequency_gain_value:
            queue_save(Request::frequency_save);
            break;
        case Request::frequency_save:
            complete_calibration();
            break;
        default:
            retry_startup();
            break;
        }
    }

    void check_serial_status() {
        const auto status = serial::status(handle_);
        if (status.dropped == serial_status_.dropped && status.errors == serial_status_.errors)
            return;
        serial_status_ = status;
        frame_size_ = 0;
        sample_.valid = false;
        if (primary_controlled_)
            sample_.source_retained = !sample_.watchdog_reset;
        else
            sample_.available_fields = 0;
        response_pending_ = false;
        if (calibration_requested_)
            fail_calibration(unsigned(serial::Result::failed));
    }

    void update_reset() {
        if (!reset_pending_ || !reached(now_, reset_release_at_))
            return;
        if (platform::gpio_write(reset_, true) != platform::GpioResult::success) {
            reset_faulted_ = true;
            reset_pending_ = false;
            AddLog(LogLevel::error, "NRG: MCP39F501 reset release failed");
            return;
        }
        reset_pending_ = false;
    }

    void expire_sample() {
        if (!last_data_at_ || now_ - last_data_at_ <= kFreshnessMs)
            return;
        sample_.valid = false;
        sample_.available_fields = 0;
        sample_.voltage = sample_.current = sample_.power = 0;
        sample_.frequency_millihz = 0;
        current_raw_ = voltage_raw_ = power_raw_ = 0;
    }

    void response_timeout() {
        response_pending_ = false;
        frame_size_ = 0;
        if (calibration_requested_ && waiting_kind_ != Request::startup_calibration &&
            waiting_kind_ != Request::startup_frequency && waiting_kind_ != Request::address &&
            waiting_kind_ != Request::data) {
            fail_calibration(unsigned(serial::Result::timeout));
            return;
        }
        if (waiting_kind_ == Request::data) {
            due_ = now_ + kPollIntervalMs;
            return;
        }
        retry_startup();
    }

    void fail_transaction(std::uint8_t result) {
        response_pending_ = false;
        tx_pending_ = false;
        frame_size_ = 0;
        if (calibration_requested_)
            fail_calibration(result);
        else
            due_ = now_ + kPollIntervalMs;
    }

    void fail_calibration(std::uint8_t result) {
        calibration_requested_ = false;
        control_.phase = ControlPhase::failed;
        control_.result = result;
        due_ = now_ + kPollIntervalMs;
    }

    void complete_calibration() {
        calibration_requested_ = false;
        control_.phase = ControlPhase::succeeded;
        control_.result = unsigned(serial::Result::success);
        stage_ = Stage::running;
        due_ = now_ + kCalibrationDelayMs;
    }

    void retry_startup() { due_ = now_ + kPollIntervalMs; }

    serial::Handle handle_{};
    serial::Status serial_status_{};
    PhysicalIdentity identity_{};
    PinId reset_{};
    runtime_pins::Owner reset_owner_{};
    ControlStatus control_{};
    std::array<std::uint8_t, kMaximumResponse> frame_{};
    std::array<std::uint8_t, kMaximumCommand> transmit_{};
    std::array<std::uint8_t, kCalibrationLength> calibration_registers_{};
    Sample sample_{};
    ObservationAge primary_age_{};
    bool primary_controlled_{}, primary_on_{}, primary_observed_{};
    Stage stage_{Stage::calibration};
    Request transmit_kind_{Request::none}, waiting_kind_{Request::none};
    std::size_t frame_size_{}, transmit_size_{}, transmit_offset_{};
    std::uint32_t now_{}, due_{}, response_deadline_{}, last_byte_at_{}, reset_release_at_{},
        last_data_at_{};
    std::uint32_t system_configuration_{}, current_raw_{}, power_raw_{};
    std::uint16_t voltage_raw_{}, frequency_raw_{}, frequency_reference_{}, frequency_gain_{};
    std::uint32_t requested_reference_{};
    CalibrationField requested_field_{CalibrationField::power};
    std::uint8_t address_{};
    bool tx_pending_{}, response_pending_{}, reset_claimed_{}, reset_faulted_{}, reset_pending_{},
        calibration_requested_{};
};
} // namespace

#if USE_MCP39F501
/** @brief Creates an MCP39F501 instance over the shared UART owner.
 * @param configuration UART RX/TX binding, optional reset pin, and channel configuration.
 * @return Meter owner, or nullptr when UART/reset ownership or allocation fails.
 */
std::unique_ptr<Device> create_mcp39f501(const Configuration& configuration) {
    auto meter = std::unique_ptr<Mcp39f501>(new (std::nothrow) Mcp39f501(configuration));
    if (!meter || !meter->open(configuration))
        return {};
    return meter;
}
#endif
} // namespace newmota::energy
