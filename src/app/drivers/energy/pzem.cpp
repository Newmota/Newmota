/*
  pzem.cpp - Independently selectable PZEM AC and DC Modbus energy devices

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Protocol conversion reference: Tasmota xnrg_05_pzem_ac.ino and xnrg_06_pzem_dc.ino
  Copyright (C) 2021 Theo Arends

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/** @file pzem.cpp
 * @brief PZEM AC/DC protocol conversion and asynchronous factory-address controls.
 */
#include "services/energy/energy_service.h"
#if USE_BERRY_ENERGY_API
#include "services/energy/energy_observation_age.h"
#endif
#include "drivers/modbus/rtu_service.h"
#include "newmota/platform/platform.h"
#include "newmota_features.h"
#include <algorithm>
#include <memory>
#include <new>

namespace newmota::energy {
namespace {
constexpr std::size_t kChannels = 3; // Pinned Tasmota ENERGY_MAX_PHASES for these devices.

std::uint16_t word(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | p[1];
}

std::uint32_t low_word_first(const std::uint8_t* p) {
    return word(p) | (std::uint32_t(word(p + 2)) << 16);
}

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

/** @brief One configured instance; each channel retains a distinct slave-address queue. */
class Pzem final : public Device {
    struct Channel {
#if USE_BERRY_ENERGY_API
        /** @brief Berry-only source age; never participates in measurement expiry/accounting. */
        ObservationAge observation_age{};
#endif
        modbus::Client client{};
        Sample sample{};
        std::uint32_t received_at{}, due{};
        bool pending{};
    };

    std::unique_ptr<Channel[]> channels_;
    std::uint32_t ready_at_{}, attach_at_{};
    std::uint8_t bus_{}, address_{}, count_{};
    bool dc_{};
    modbus::Client programmer_{};
    ControlStatus control_{};
    bool control_pending_{}, identity_pending_{}, rolling_back_{};
    PhysicalIdentity identity_{};
    std::uint8_t rollback_address_{};

    void readdress(std::uint8_t value) {
        modbus::close_client(channels_[0].client);
        channels_[0].client = {};
        channels_[0].pending = false;
        channels_[0].sample.valid = false;
        channels_[0].sample.address = 0;
        address_ = value;
        attach_at_ = platform::monotonic_milliseconds();
    }

    bool attached() const {
        for (std::size_t i = 0; i < count_; ++i)
            if (!modbus::active(channels_[i].client))
                return false;
        return true;
    }

    void receive(Channel& channel, const modbus::Response& response, std::uint32_t now,
        std::uint8_t address) {
        if (response.result != modbus::Result::success)
            return;
#if USE_BERRY_ENERGY_API
        (void)channel.observation_age.poll(now, true);
#endif
        const auto* p =
            response.data.data() + 1; // RTU owner validated byte count and exact length.
        auto& s = channel.sample;
        s.voltage = std::int32_t(word(p)) * (dc_ ? 10 : 100);
        s.current = dc_ ? std::int32_t(word(p + 2)) * 10 : saturate_i32(low_word_first(p + 2));
        s.power = std::int64_t(low_word_first(p + (dc_ ? 4 : 6))) * 100;
        s.import_counter = {low_word_first(p + (dc_ ? 8 : 10)), 0};
        s.counter_fields = 1;
#if USE_ENERGY_HARDWARE_TOTAL
        s.import_reference_valid = true;
        s.import_reference = s.import_counter;
#endif
        if (!dc_) {
            s.frequency_millihz = std::uint32_t(word(p + 14)) * 100;
            s.power_factor_milli = std::uint32_t(word(p + 16)) * 10;
            s.auxiliary_fields |= 3;
        }
        s.address = address;
        s.valid = true;
        channel.received_at = now;
    }

    void program(std::uint32_t now) {
        if (control_.phase != ControlPhase::queued)
            return;
        if (control_pending_) {
            modbus::Response response{};
            if (!modbus::take_response(programmer_, response))
                return;
            control_pending_ = false;
#if USE_BERRY_ENERGY_API
            /** @brief The source renews before its metrology-only branch, including a matched
             * successful FC06 acknowledgement. Newmota's programmer targets the first meter.
             */
            if (response.result == modbus::Result::success)
                (void)channels_[0].observation_age.poll(now, true);
#endif
            modbus::close_client(programmer_);
            programmer_ = {};
            control_.result = unsigned(response.result);
            control_.exception = response.exception_code;
            if (response.result == modbus::Result::success &&
                control_.operation == Control::module_address) {
                readdress(rolling_back_ ? rollback_address_ : control_.value);
                if (rolling_back_) {
                    control_.phase = ControlPhase::failed;
                    control_.result = unsigned(modbus::Result::storage_error);
                    rolling_back_ = false;
                } else {
                    identity_pending_ = true;
                    control_.phase = ControlPhase::committing;
                }
            } else {
                control_.phase = response.result == modbus::Result::success
                                     ? ControlPhase::succeeded
                                     : ControlPhase::failed;
                control_.rollback_failed = rolling_back_;
                rolling_back_ = false;
            }
            channels_[0].due = now;
            return;
        }
        // Wait until the meter's existing read is complete before using its factory address.
        if (channels_[0].pending)
            return;
        if (!modbus::active(programmer_)) {
            const auto result =
                modbus::open_client(bus_, rolling_back_ ? address_ : 0xF8, programmer_, true, true);
            if (result != modbus::Result::success) {
                control_.result = unsigned(result);
                control_.phase = ControlPhase::failed;
                control_.rollback_failed = rolling_back_;
                rolling_back_ = false;
                return;
            }
        }
        const std::uint16_t value = rolling_back_ ? rollback_address_
                                    : control_.operation == Control::current_range
                                        ? (control_.value / 50) >> 1
                                        : control_.value;
        const std::uint8_t request[]{0,
            std::uint8_t(control_.operation == Control::current_range ? 3 : 2),
            std::uint8_t(value >> 8), std::uint8_t(value)};
        modbus::Transaction transaction{};
        transaction.response_data_size = 4;
        transaction.match = modbus::ResponseMatch::echo_prefix;
        transaction.echo_bytes = 4;
        transaction.timeout_ms = 1000;
        transaction.retries = 0; // Lost acknowledgement must not replay a configuration write.
        const auto result = modbus::submit(programmer_, 6, request, sizeof(request), transaction);
        if (result == modbus::Result::success)
            control_pending_ = true;
        else {
            control_.result = unsigned(result);
            control_.phase = ControlPhase::failed;
            control_.rollback_failed = rolling_back_;
            rolling_back_ = false;
            modbus::close_client(programmer_);
            programmer_ = {};
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies this channel's explicit source-event age without renewing or ticking it. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!channels_ || channel >= count_)
            return false;
        output = channels_[channel].observation_age.value();
        return true;
    }
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Marks native import totals handled by the source EnergyUpdateTotal core. */
    bool source_total_counter() const override { return true; }
#endif

    Pzem(const Configuration& c, bool dc)
        : channels_(new (std::nothrow) Channel[c.channels]),
          ready_at_(platform::monotonic_milliseconds() + 4000), attach_at_(ready_at_ - 4000),
          bus_(c.identity.bus - 1), address_(c.identity.address), count_(c.channels), dc_(dc),
          identity_(c.identity) {
        if (channels_)
            for (std::size_t i = 0; i < count_; ++i) {
                channels_[i].due = ready_at_;
                channels_[i].sample.use_absolute = true;
                channels_[i].sample.auxiliary_fields = 12;
                channels_[i].sample.direct_current = dc;
            }
    }

    ~Pzem() override {
        if (channels_)
            for (std::size_t i = 0; i < count_; ++i)
                modbus::close_client(channels_[i].client);
        modbus::close_client(programmer_);
    }

    bool allocated() const { return bool(channels_); }

    bool online() const override { return modbus::bus_available(bus_) && attached(); }

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

    bool control(Control operation, std::uint16_t value) override {
        if ((control_.phase == ControlPhase::queued ||
                control_.phase == ControlPhase::committing) ||
            count_ != 1 || !online() || modbus::client_count(bus_) != 1)
            return false;
        if (operation == Control::module_address) {
            if (!value || value > 247)
                return false;
        } else if (!dc_ || (value != 50 && value != 100 && value != 200 && value != 300))
            return false;
        control_ = {ControlPhase::queued, operation, value, 0, 0};
        return true;
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
        if (!attached() && reached(now, attach_at_)) {
            attach_at_ = now + 5000;
            if (modbus::bus_available(bus_))
                for (std::size_t i = 0; i < count_; ++i)
                    if (!modbus::active(channels_[i].client))
                        (void)modbus::open_client(bus_, address_ + i, channels_[i].client);
        }
        for (std::size_t i = 0; i < count_; ++i) {
            auto& channel = channels_[i];
            if (channel.pending) {
                modbus::Response response{};
                if (modbus::take_response(channel.client, response)) {
                    channel.pending = false;
                    receive(channel, response, now, address_ + i);
                    channel.due = now + 1000;
                }
            }
            if (now - channel.received_at >= 4000)
                channel.sample.valid = false;
        }
        program(now);
        if (control_.phase != ControlPhase::queued && control_.phase != ControlPhase::committing &&
            reached(now, ready_at_))
            for (std::size_t i = 0; i < count_; ++i) {
                auto& channel = channels_[i];
                if (!channel.pending && modbus::active(channel.client) &&
                    reached(now, channel.due)) {
                    const std::uint8_t request[]{0, 0, 0, std::uint8_t(dc_ ? 8 : 10)};
                    modbus::Transaction transaction{};
                    transaction.response_data_size = dc_ ? 17 : 21;
                    transaction.match = modbus::ResponseMatch::byte_count;
                    transaction.timeout_ms = 1000;
                    transaction.retries = 2;
                    if (modbus::submit(channel.client, 4, request, sizeof(request), transaction) ==
                        modbus::Result::success)
                        channel.pending = true;
                }
            }
        for (std::size_t i = 0; i < std::min(count, std::size_t(count_)); ++i)
            samples[i] = channels_[i].sample;
    }
};

/** @brief Validates the retained physical bus without requiring native restoration to succeed. */
std::unique_ptr<Device> create(const Configuration& c, bool dc) {
    if (!c.channels || c.channels > kChannels || c.identity.bus < 1 ||
        c.identity.bus > modbus::kMaximumBuses || !c.identity.address ||
        unsigned(c.identity.address) + c.channels - 1 > 247 || c.options.flags ||
        c.calibration.power || c.calibration.voltage || c.calibration.current)
        return {};
    for (const auto& binding : c.identity.pins)
        if (binding.function != PinFunction::none)
            return {};
    modbus::BusConfiguration bus{};
    if (!modbus::configuration(c.identity.bus - 1, bus) || bus.uart.baud != 9600 ||
        bus.uart.data_bits != 8 || bus.uart.parity != platform::serial::Parity::none ||
        bus.uart.stop_bits != (dc ? 2 : 1))
        return {};
    auto device = std::unique_ptr<Pzem>(new (std::nothrow) Pzem(c, dc));
    if (!device || !device->allocated())
        return {};
    return device;
}
} // namespace
#if USE_PZEM_AC
/** @brief Creates a PZEM AC instance on its retained Modbus bus.
 * @param c Persisted bus, starting address, and channel count.
 * @return Meter owner, or nullptr when the retained bus configuration is incompatible.
 */
std::unique_ptr<Device> create_pzem_ac(const Configuration& c) {
    return create(c, false);
}
#endif
#if USE_PZEM_DC
/** @brief Creates a PZEM DC instance on its retained Modbus bus.
 * @param c Persisted bus, starting address, and channel count.
 * @return Meter owner, or nullptr when the retained bus configuration is incompatible.
 */
std::unique_ptr<Device> create_pzem_dc(const Configuration& c) {
    return create(c, true);
}
#endif
} // namespace newmota::energy
