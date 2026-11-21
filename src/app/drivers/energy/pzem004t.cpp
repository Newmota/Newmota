/*
  pzem004t.cpp - PZEM004T V1/V2 UART energy meter

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol conversion reference: Tasmota xnrg_03_pzem004t.ino
  Copyright (C) 2021 Theo Arends

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file pzem004t.cpp
 * @brief Portable asynchronous PZEM004T V1/V2 UART protocol and accounting input.
 */

#include "services/energy/energy_service.h"
#if USE_BERRY_ENERGY_API
#include "services/energy/energy_observation_age.h"
#endif
#include "drivers/serial/serial_service.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <array>
#include <cstring>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::array<std::uint8_t, 4> kCommands{0xb0, 0xb1, 0xb2, 0xb3};
constexpr std::array<std::uint8_t, 4> kResponses{0xa0, 0xa1, 0xa2, 0xa3};
constexpr std::uint8_t kSetAddress = 0xb4;
constexpr std::uint8_t kSetAddressResponse = 0xa4;
constexpr std::uint32_t kResponseTimeoutMs = 250;
constexpr std::uint32_t kRetryDelayMs = 250;
constexpr std::uint32_t kFreshnessMs = 4000;
constexpr std::uint32_t kStabilizationMs = 30000;
constexpr std::uint8_t kMaximumRetries = 5;

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

std::uint8_t checksum(const std::uint8_t* bytes) {
    std::uint8_t sum{};
    for (std::size_t i = 0; i < 6; ++i)
        sum = static_cast<std::uint8_t>(sum + bytes[i]);
    return sum;
}

/** @brief One bounded asynchronous instance; channel addresses are consecutive unit IDs. */
class Pzem004t final : public Device {
    struct Channel {
#if USE_BERRY_ENERGY_API
        /** @brief Berry-only source age; never participates in measurement expiry/accounting. */
        ObservationAge observation_age{};
#endif
        Sample sample{};
        std::array<std::uint32_t, 4> field_seen{};
        std::uint32_t received_at{};
        std::uint8_t fields_seen{};
    };

    std::array<Channel, 3> channels_{};
    serial::Handle handle_{};
    serial::Status serial_status_{};
    PhysicalIdentity identity_{};
    ControlStatus control_{};
    std::array<std::uint8_t, 7> frame_{}, transmit_{};
    std::size_t frame_size_{}, transmit_offset_{};
    std::uint32_t now_{}, due_{}, deadline_{}, started_at_{};
    std::uint8_t address_{}, rollback_address_{}, channel_{}, field_{}, retries_{};
    std::uint8_t expected_response_{}, control_address_{};
    std::uint8_t count_{};
    bool pending_{}, tx_frame_pending_{}, control_pending_{}, identity_pending_{}, rolling_back_{};

    std::uint8_t channel_address() const { return static_cast<std::uint8_t>(address_ + channel_); }

    void advance() {
        retries_ = 0;
        if (++field_ >= static_cast<std::uint8_t>(kCommands.size())) {
            field_ = 0;
            if (++channel_ == count_)
                channel_ = 0;
        }
        due_ = now_ + kRetryDelayMs;
    }

    void fail_control(std::uint8_t result, bool rollback_failed = false) {
        pending_ = false;
        tx_frame_pending_ = false;
        control_pending_ = false;
        transmit_offset_ = 0;
        control_.result = result;
        control_.rollback_failed = rollback_failed;
        control_.phase = ControlPhase::failed;
        rolling_back_ = false;
        due_ = now_ + kRetryDelayMs;
    }

    bool queue_frame(std::uint8_t command, std::uint8_t address) {
        transmit_ = {command, 192, 168, 1, address, 0, 0};
        transmit_[6] = checksum(transmit_.data());
        (void)serial::discard_rx(handle_);
        transmit_offset_ = 0;
        tx_frame_pending_ = true;
        pending_ = false;
        return true;
    }

    void transmit() {
        if (!tx_frame_pending_ || transmit_offset_ >= transmit_.size())
            return;
        const auto result = serial::write_some(
            handle_, transmit_.data() + transmit_offset_, transmit_.size() - transmit_offset_);
        transmit_offset_ += result.count;
        if (result.result != serial::Result::success) {
            tx_frame_pending_ = false;
            if (control_pending_)
                fail_control(unsigned(result.result), rolling_back_);
            else
                due_ = now_ + kRetryDelayMs;
            transmit_offset_ = 0;
            return;
        }
        if (transmit_offset_ == transmit_.size()) {
            tx_frame_pending_ = false;
            pending_ = true;
            deadline_ = now_ + kResponseTimeoutMs;
            expected_response_ = control_pending_ ? kSetAddressResponse : kResponses[field_];
        }
    }

    bool start_control() {
        if (!control_pending_ && control_.phase != ControlPhase::queued)
            return false;
        if (pending_ || tx_frame_pending_)
            return false;
        control_pending_ = true;
        control_address_ = rolling_back_ ? rollback_address_ : control_.value;
        return queue_frame(kSetAddress, control_address_);
    }

    void accept_control() {
        pending_ = false;
        control_pending_ = false;
        control_.result = unsigned(serial::Result::success);
        retries_ = 0;
        if (rolling_back_) {
            address_ = rollback_address_;
            identity_.address = rollback_address_;
            control_.rollback_failed = false;
            control_.phase = ControlPhase::failed;
            rolling_back_ = false;
        } else {
            address_ = control_address_;
            identity_pending_ = true;
            control_.phase = ControlPhase::committing;
        }
        due_ = now_ + kRetryDelayMs;
    }

    void accept_read() {
        auto& channel = channels_[channel_];
        auto& sample = channel.sample;
        switch (field_) {
        case 0: // Voltage in volts with one decimal digit.
            sample.voltage = (std::uint32_t(frame_[1]) * 256 + frame_[2]) * 1000 +
                             std::uint32_t(frame_[3]) * 100;
            break;
        case 1: // Current in amperes with two decimal digits.
            sample.current =
                (std::uint32_t(frame_[1]) * 256 + frame_[2]) * 1000 + std::uint32_t(frame_[3]) * 10;
            break;
        case 2: // The source driver treats the high two data bytes as whole watts.
            sample.power = (std::uint32_t(frame_[1]) * 256 + frame_[2]) * 1000;
            break;
        case 3:
            sample.import_counter = {
                std::uint32_t(frame_[1]) * 65536 + std::uint32_t(frame_[2]) * 256 + frame_[3], 0};
            sample.counter_fields = now_ - started_at_ >= kStabilizationMs ? 1 : 0;
#if USE_ENERGY_HARDWARE_TOTAL
            sample.import_reference_valid = sample.counter_fields & 1;
            sample.import_reference = sample.import_counter;
#endif
            break;
        default:
            return;
        }
        channel.field_seen[field_] = now_;
        channel.fields_seen |= static_cast<std::uint8_t>(1U << field_);
        channel.received_at = now_;
        sample.address = channel_address();
        sample.use_absolute = true;
        sample.valid = true;
        if (field_ < 3)
            sample.available_fields |= static_cast<std::uint8_t>(1U << field_);
        else
            sample.counter_fields = now_ - started_at_ >= kStabilizationMs ? 1 : 0;
        pending_ = false;
        advance();
    }

    void accept_frame() {
        if (frame_[6] != checksum(frame_.data())) {
            frame_size_ = 0;
            return;
        }
        if (!pending_ || frame_[0] != expected_response_) {
            frame_size_ = 0;
            return;
        }
        frame_size_ = 0;
#if USE_BERRY_ENERGY_API
        /** @brief Renew only an expected checksum-valid response; controls target meter zero,
         * while normal field replies belong to the current addressed read channel.
         */
        (void)channels_[control_pending_ ? 0 : channel_].observation_age.poll(now_, true);
#endif
        if (control_pending_)
            accept_control();
        else
            accept_read();
    }

    void receive() {
        std::array<std::uint8_t, 64> input{};
        const auto transfer = serial::read(handle_, input.data(), input.size());
        if (transfer.result != serial::Result::success)
            return;
        for (std::size_t i = 0; i < transfer.count; ++i) {
            const auto byte = input[i];
            if (!frame_size_ && (byte & 0xf8) != 0xa0)
                continue;
            frame_[frame_size_++] = byte;
            if (frame_size_ == frame_.size())
                accept_frame();
        }
    }

    void project(std::uint32_t now, Sample* samples, std::size_t count) {
        for (std::size_t i = 0; i < count && i < count_; ++i) {
            auto sample = channels_[i].sample;
            sample.valid =
                channels_[i].fields_seen && now - channels_[i].received_at < kFreshnessMs;
            sample.available_fields = 0;
            for (std::size_t field = 0; field < 3; ++field)
                if ((channels_[i].fields_seen & (1U << field)) &&
                    now - channels_[i].field_seen[field] < kFreshnessMs)
                    sample.available_fields |= static_cast<std::uint8_t>(1U << field);
            sample.counter_fields = (channels_[i].fields_seen & (1U << 3)) &&
                                            now - channels_[i].field_seen[3] < kFreshnessMs &&
                                            now - started_at_ >= kStabilizationMs
                                        ? 1
                                        : 0;
            sample.use_absolute = true;
            samples[i] = sample;
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies this channel's explicit source-event age without renewing or ticking it. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (channel >= count_)
            return false;
        output = channels_[channel].observation_age.value();
        return true;
    }
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Marks native import totals handled by the source EnergyUpdateTotal core. */
    bool source_total_counter() const override { return true; }
#endif

    explicit Pzem004t(const Configuration& configuration)
        : identity_(configuration.identity), address_(configuration.identity.address),
          count_(configuration.channels) {
        started_at_ = platform::monotonic_milliseconds();
        now_ = started_at_;
        due_ = now_;
        for (std::size_t i = 0; i < count_ && i < channels_.size(); ++i) {
            channels_[i].sample.use_absolute = true;
            channels_[i].sample.address = static_cast<std::uint8_t>(address_ + i);
        }
    }

    ~Pzem004t() override { (void)serial::request_close(handle_); }

    bool open() {
        if (!count_ || count_ > channels_.size() || !address_ || address_ > 3)
            return false;
        for (const auto& pin : identity_.pins)
            if (pin.function != PinFunction::none && pin.function != PinFunction::uart_rx &&
                pin.function != PinFunction::uart_tx)
                return false;
        if (!identity_.pin(PinFunction::uart_rx).valid() ||
            !identity_.pin(PinFunction::uart_tx).valid() || address_ + count_ - 1 > 3)
            return false;
        serial::Config config{};
        config.rx = identity_.pin(PinFunction::uart_rx);
        config.tx = identity_.pin(PinFunction::uart_tx);
        config.baud = 9600;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.parity = platform::serial::Parity::none;
        config.rx_capacity = 64;
        return serial::open(config, handle_) == serial::Result::success;
    }

    bool online() const override { return serial::active(handle_); }

    bool calibrate(const Calibration& calibration) override {
        return !calibration.power && !calibration.voltage && !calibration.current;
    }

    bool set_options(const Options& options) override {
        return !options.flags && options.invalid_power_frames == 128;
    }

    bool control(Control operation, std::uint16_t value) override {
        if (operation != Control::module_address || count_ != 1 || !online() || !value ||
            value > 3 || control_.phase == ControlPhase::queued ||
            control_.phase == ControlPhase::committing)
            return false;
        control_ = {ControlPhase::queued, operation, value, 0, 0};
        return true;
    }

    ControlStatus control_status() const override { return control_; }

    bool configuration_locked() const override {
        return control_.phase == ControlPhase::queued || control_.phase == ControlPhase::committing;
    }

    bool identity_change(PhysicalIdentity& output) const override {
        if (!identity_pending_)
            return false;
        output = identity_;
        output.address = address_;
        return true;
    }

    void identity_result(bool committed) override {
        identity_pending_ = false;
        if (committed) {
            identity_.address = address_;
            control_.phase = ControlPhase::succeeded;
        } else {
            rollback_address_ = identity_.address;
            rolling_back_ = true;
            control_.phase = ControlPhase::queued;
        }
    }

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
#if USE_BERRY_ENERGY_API
        for (std::size_t c = 0; c < count_; ++c)
            (void)channels_[c].observation_age.poll(now, false);
#endif
#if USE_ENERGY_HARDWARE_TOTAL
        for (std::size_t i = 0; i < count_; ++i)
            channels_[i].sample.import_reference_valid = false;
#endif
        now_ = now;
        const auto status = serial::status(handle_);
        if (status.dropped != serial_status_.dropped || status.errors != serial_status_.errors) {
            frame_size_ = 0;
            serial_status_ = status;
        }
        receive();
        if (pending_ && reached(now, deadline_)) {
            pending_ = false;
            if (++retries_ >= kMaximumRetries) {
                if (control_pending_)
                    fail_control(unsigned(serial::Result::timeout), rolling_back_);
                else
                    advance();
            } else
                due_ = now + kRetryDelayMs;
        }
        if (tx_frame_pending_)
            transmit();
        if (!pending_ && !tx_frame_pending_ && reached(now, due_) &&
            control_.phase != ControlPhase::committing) {
            if (control_.phase == ControlPhase::queued) {
                (void)start_control();
            } else {
                (void)queue_frame(kCommands[field_], channel_address());
                transmit();
            }
        }
        project(now, samples, count);
    }
};

} // namespace

#if USE_PZEM004T
/** @brief Creates a PZEM004T V1/V2 instance over the shared UART owner.
 * @param configuration UART pin binding, baud options, and consecutive slave addresses.
 * @return Meter owner, or nullptr when configuration or serial ownership fails.
 */
std::unique_ptr<Device> create_pzem004t(const Configuration& configuration) {
    if (configuration.identity.driver != 3 || configuration.identity.bus ||
        configuration.calibration.power || configuration.calibration.voltage ||
        configuration.calibration.current || configuration.options.flags ||
        configuration.options.invalid_power_frames != 128)
        return {};
    auto meter = std::unique_ptr<Pzem004t>(new (std::nothrow) Pzem004t(configuration));
    if (!meter || !meter->open())
        return {};
    return meter;
}
#endif
} // namespace newmota::energy
