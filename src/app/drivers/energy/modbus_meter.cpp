/*
  modbus_meter.cpp - Fair register polling and exact typed Modbus meter conversion

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  User-field resolution reference: Tasmota xnrg_29_modbus.ino,
  Copyright (C) 2022 Theo Arends, GPL version 3 or later.
*/

/** @file modbus_meter.cpp
 * @brief Fair register polling and exact typed Modbus meter conversion.
 */

#include "drivers/energy/modbus_meter.h"
#if USE_BERRY_ENERGY_API
#include "services/energy/energy_observation_age.h"
#endif
#include "core/command/command.h"
#include "core/storage/filesystem.h"
#include "newmota_features.h"
#include "newmota/platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace newmota::energy::registers {
namespace {
constexpr auto field_count = std::size_t(Field::count);

bool reached(std::uint32_t now, std::uint32_t deadline) {
    return now - deadline < 0x80000000U;
}

std::uint8_t words(Encoding encoding) {
    return encoding == Encoding::signed16 || encoding == Encoding::unsigned16 ? 1
           : encoding == Encoding::signed64                                   ? 4
                                                                              : 2;
}

struct Number {
    std::int64_t integer{};
    double real{};
    bool floating{};
};

/** @brief Decodes bytes without host endianness assumptions or signed-shift overflow. */
bool decode(const std::uint8_t* p, Encoding encoding, Number& value) {
    const unsigned count = words(encoding) * 2;
    std::uint64_t raw{};
    for (unsigned i = 0; i < count; ++i)
        raw = (raw << 8) | p[i];
    if (encoding == Encoding::signed32_swapped || encoding == Encoding::unsigned32_swapped)
        raw = ((raw & 65535) << 16) | (raw >> 16);
    switch (encoding) {
    case Encoding::float32: {
        const auto bits = std::uint32_t(raw);
        float real{};
        std::memcpy(&real, &bits, 4);
        value.real = real;
        value.floating = true;
        return std::isfinite(real);
    }
    case Encoding::signed16:
        value.integer = raw & 0x8000 ? std::int64_t(raw) - 0x10000 : raw;
        break;
    case Encoding::signed32:
    case Encoding::signed32_swapped:
        value.integer = raw & 0x80000000U ? std::int64_t(raw) - 0x100000000LL : raw;
        break;
    case Encoding::signed64:
        if (raw > INT64_MAX)
            return false; // Energy counters are nonnegative signed64 Wh.
        value.integer = raw;
        break;
    case Encoding::unsigned16:
    case Encoding::unsigned32:
    case Encoding::unsigned32_swapped:
        value.integer = raw;
        break;
    default:
        return false;
    }
    return true;
}

/** @brief Converts energy without multiplying an entire 64-bit Wh counter into micro-Wh. */
bool energy_value(const Number& number, const Register& reg, EnergyValue& output) {
    if (!reg.denominator || reg.numerator < 0)
        return false;
    if (!number.floating) {
        if (number.integer < 0)
            return false;
        const auto raw = std::uint64_t(number.integer), scale = std::uint64_t(reg.numerator);
        const auto whole = raw / reg.denominator, remainder = raw % reg.denominator;
        if (scale && whole > UINT64_MAX / scale)
            return false;
        output.wh = whole * scale;
        const auto product = remainder * scale;
        if (!output.add({product / reg.denominator,
                std::uint32_t((product % reg.denominator) * 1000000 / reg.denominator)}))
            return false;
        return true;
    }
    const long double wh = static_cast<long double>(number.real) * reg.numerator / reg.denominator;
    if (!std::isfinite(wh) || wh < 0 || wh >= 18446744073709551616.0L)
        return false;
    output.wh = static_cast<std::uint64_t>(wh);
    output.micro_wh = static_cast<std::uint32_t>((wh - output.wh) * 1000000);
    return output.micro_wh < 1000000;
}

#if USE_MODBUS_ENERGY
/** @brief Streams a signed six-fractional-digit reading at literal precision 0..20.
 * Extra decimal places are bounded zero padding, never invented register precision;
 * formatting never computes 10^20 or multiplies a wide signed magnitude.
 */
bool write_user_value(CommandOutput& output, std::int64_t micro, std::uint8_t decimals) {
    decimals = std::min<std::uint8_t>(decimals, 20);
    const auto retained_digits = std::min<std::uint8_t>(decimals, 6);
    const bool negative = micro < 0;
    const auto magnitude = negative ? std::uint64_t(-(micro + 1)) + 1 : std::uint64_t(micro);
    const auto divisor = std::uint64_t{1000000};
    auto whole = magnitude / divisor;
    auto fraction = magnitude % divisor;
    if (decimals < 6) {
        std::uint64_t rounding = 1;
        for (auto i = decimals; i < 6; ++i)
            rounding *= 10;
        fraction = (fraction + rounding / 2) / rounding;
        std::uint64_t limit = 1;
        for (std::uint8_t i = 0; i < decimals; ++i)
            limit *= 10;
        if (fraction == limit) {
            ++whole;
            fraction = 0;
        }
    }
    if (!output.format("%s%llu", negative ? "-" : "", static_cast<unsigned long long>(whole)))
        return false;
    if (!decimals)
        return true;
    /** @brief The rounded fraction already has retained_digits places; scaling it again
     * would erase lower-precision fractional readings. Six-place values need no rounding.
     */
    if (!output.format(".%0*llu", int(retained_digits), static_cast<unsigned long long>(fraction)))
        return false;
    constexpr std::string_view zeros{"00000000000000"};
    return decimals <= 6 || output.write(zeros.substr(0, decimals - 6));
}
#endif

/** @brief One client per physical meter; all register steps fairly share the RTU bus. */
class Meter final : public Device {
    struct UserReading {
        std::int64_t micro{};
        bool available{};
    };

    struct Channel {
#if USE_BERRY_ENERGY_API
        /** @brief Berry-only source age; never participates in measurement expiry/accounting. */
        ObservationAge observation_age{};
#endif
#if USE_ENERGY_HARDWARE_TOTAL
        /** @brief Current-poll register events: bit0 import, bit1 total; never cached freshness. */
        std::uint8_t reference_updates{};
#endif
        Sample sample{};
        std::array<std::uint32_t, field_count> seen{};
        std::uint32_t received{}, response_at{};
        bool response_seen{};
    };

    std::unique_ptr<ConfiguredProfile> owned_profile_;
    Profile profile_{};
    std::unique_ptr<Channel[]> channels_;
    std::unique_ptr<UserReading[]> user_readings_;
    std::array<modbus::Client, 3> clients_{};
    std::array<std::uint8_t, 3> channel_client_{};
    std::array<std::uint8_t, 3> unit_addresses_{};
    std::array<std::uint8_t, 3> client_addresses_{};
    std::size_t step_{}, end_{};
    std::uint32_t watchdog_started_at_{}, due_{}, attach_at_{}, freshness_{};
    std::uint16_t requested_address_{};
    std::uint8_t bus_{}, flags_{}, client_count_{}, pending_client_{}, current_unit_address_{};
    bool pending_{}, extensions_{true}, cycle_seen_{}, user_watchdog_reset_{};

    bool selected(const Register& r) const {
        return (flags_ & r.required_flags) == r.required_flags && !(flags_ & r.forbidden_flags) &&
               (extensions_ || !r.optional_extension);
    }

    /** @brief Resolves User-field precision at the time each output is written. */
    bool phase_zero_address(std::size_t user) const {
        for (std::size_t i = 0; i < profile_.size; ++i) {
            const auto& reg = profile_.registers[i];
            if (reg.field == Field::custom && reg.user_index == user && reg.channel == 0)
                return true;
        }
        return false;
    }

    std::uint8_t user_decimals(const UserField& field) const {
#if USE_MODBUS_USER_PRECISION
        return field.resolution_selector
                   ? service().modbus_user_resolution(field.resolution_selector)
                   : field.decimals;
#else
        return field.decimals;
#endif
    }

    void assign(std::size_t c, const Register& r, const Number& number, std::uint32_t now) {
        auto& channel = channels_[c];
        auto& s = channel.sample;
        const auto field = std::size_t(r.field);
        if (r.field == Field::custom) {
            if (!user_readings_ || r.user_index >= profile_.user_field_count)
                return;
            long double scaled = number.floating ? static_cast<long double>(number.real)
                                                 : static_cast<long double>(number.integer);
            if (r.user_exponent > 0) {
                for (int i = 0; i < r.user_exponent; ++i)
                    scaled *= 10;
            } else {
                for (int i = 0; i > r.user_exponent; --i)
                    scaled /= 10;
            }
            scaled *= 1000000;
            if (!std::isfinite(scaled) || scaled < static_cast<long double>(INT64_MIN) ||
                scaled >= 9223372036854775808.0L)
                return;
            auto& reading = user_readings_[c * profile_.user_field_count + r.user_index];
            reading.micro = static_cast<std::int64_t>(scaled);
            reading.available = true;
            channel.received = now;
            s.address = current_unit_address_;
            return;
        }
        const bool energy = r.field == Field::import_counter || r.field == Field::export_counter ||
                            r.field == Field::total_active || r.field == Field::import_reactive ||
                            r.field == Field::export_reactive || r.field == Field::total_reactive ||
                            r.field == Field::resettable_active;
        if (energy) {
            EnergyValue value{};
            if (!energy_value(number, r, value))
                return;
            switch (r.field) {
            case Field::import_counter:
#if USE_ENERGY_HARDWARE_TOTAL
                channel.reference_updates |= 1;
#endif
                s.import_counter = value;
                break;
            case Field::export_counter:
                s.export_counter = value;
                break;
            case Field::total_active:
#if USE_ENERGY_HARDWARE_TOTAL
                channel.reference_updates |= 2;
#endif
                s.total_active = value;
                break;
            case Field::import_reactive:
                s.import_reactive = value;
                break;
            case Field::export_reactive:
                s.export_reactive = value;
                break;
            case Field::total_reactive:
                s.total_reactive = value;
                break;
            case Field::resettable_active:
                s.resettable_active = value;
                break;
            default:
                break;
            }
        } else {
            const long double scaled =
                (number.floating ? static_cast<long double>(number.real)
                                 : static_cast<long double>(number.integer)) *
                r.numerator / r.denominator;
            if (!std::isfinite(scaled) || scaled < static_cast<long double>(INT64_MIN) ||
                scaled >= 9223372036854775808.0L)
                return;
            const auto value = static_cast<std::int64_t>(scaled);
            switch (r.field) {
            case Field::voltage:
                if (!fits_i32(value))
                    return;
                s.voltage = static_cast<std::int32_t>(value);
                break;
            case Field::current:
                if (!fits_i32(value))
                    return;
                s.current = static_cast<std::int32_t>(value);
                break;
            case Field::power:
                s.power = value;
                break;
            case Field::apparent:
                s.apparent_mva = value;
                break;
            case Field::reactive:
                s.reactive_mvar = value;
                break;
            case Field::factor:
                if (!fits_i32(value))
                    return;
                s.power_factor_milli = static_cast<std::int32_t>(value);
                break;
            case Field::frequency:
                if (value < 0 || std::uint64_t(value) > UINT32_MAX)
                    return;
                s.frequency_millihz = value;
                break;
            case Field::phase_angle:
                if (!fits_i32(value))
                    return;
                s.phase_angle_millidegrees = static_cast<std::int32_t>(value);
                break;
            case Field::import_power:
                s.import_power_mw = value;
                break;
            case Field::export_power:
                s.export_power_mw = value;
                break;
            case Field::maximum_power:
                s.maximum_power_mw = value;
                break;
            default:
                return;
            }
        }
        channel.seen[field] = now;
        channel.received = now;
        s.address = current_unit_address_;
        s.auxiliary_fields |= 4;
    }

    void accept(const modbus::Response& response, std::uint32_t now) {
        if (response.result != modbus::Result::success) {
            if (profile_.registers[step_].optional_extension &&
                response.result == modbus::Result::exception &&
                (response.exception_code == 2 || response.exception_code == 3))
                extensions_ = false;
            return;
        }
#if USE_BERRY_ENERGY_API
        /** @brief Static sources renew before numeric conversion; iEM3000 is phase-zero-only
         * despite its three metrology channels. An exception/error response never gets here.
         */
        if (profile_.observation_scope == ObservationScope::phase_zero)
            (void)channels_[0].observation_age.poll(now, true);
        else if (profile_.observation_scope == ObservationScope::all_phases)
            for (std::size_t c = 0; c < profile_.channels; ++c)
                (void)channels_[c].observation_age.poll(now, true);
#endif
        for (auto i = step_; i < end_; ++i) {
            const auto& r = profile_.registers[i];
            if (!selected(r))
                continue;
#if USE_BERRY_ENERGY_API
            /** @brief Configured common V/F publication does not fan out source age resets:
             * a scalar register represents phase zero; an explicit array row owns its phase.
             * Custom/ignored rows and numeric decode rejection still count as source replies.
             */
            if (profile_.observation_scope == ObservationScope::mapped_phase) {
                const auto phase = r.channel == common ? 0 : r.channel;
                if (phase < profile_.channels)
                    (void)channels_[phase].observation_age.poll(now, true);
            }
#endif
            /** @brief Tasmota refreshes phase validity on a successful register frame,
             * including a custom or otherwise ignored reading.
             */
            if (r.channel == common) {
                for (std::size_t c = 0; c < profile_.channels; ++c) {
                    channels_[c].response_at = now;
                    channels_[c].response_seen = true;
                }
            } else if (r.channel < profile_.channels) {
                channels_[r.channel].response_at = now;
                channels_[r.channel].response_seen = true;
            }
            if (r.field == Field::ignore)
                continue;
            Number number{};
            if (!decode(response.data.data() + 1 + 2 * (r.address - requested_address_), r.encoding,
                    number))
                continue;
            if (r.channel == common) {
                const bool energy_counter =
                    r.field == Field::import_counter || r.field == Field::export_counter ||
                    r.field == Field::total_active || r.field == Field::import_reactive ||
                    r.field == Field::export_reactive || r.field == Field::total_reactive ||
                    r.field == Field::resettable_active;
                const auto limit = energy_counter ? std::size_t{1} : std::size_t(profile_.channels);
                for (std::size_t c = 0; c < limit; ++c)
                    assign(c, r, number, now);
            } else
                assign(r.channel, r, number, now);
        }
    }

    /** @brief Applies the configurable-Modbus User-array portion of the energy watchdog.
     * The User array resets after every configured channel has gone five seconds without a
     * successful register frame. Tasmota resets a field only when its phase-zero address is set.
     */
    void update_user_watchdog(std::uint32_t now) {
        if (!user_readings_)
            return;
        bool all_channels_invalid = now - watchdog_started_at_ >= 5000;
        for (std::size_t c = 0; c < profile_.channels; ++c) {
            const auto& channel = channels_[c];
            const bool stale = channel.response_seen ? now - channel.response_at >= 5000
                                                     : now - watchdog_started_at_ >= 5000;
            all_channels_invalid = all_channels_invalid && stale;
        }
        if (!all_channels_invalid) {
            user_watchdog_reset_ = false;
            return;
        }
        if (user_watchdog_reset_)
            return;

        for (std::size_t user = 0; user < profile_.user_field_count; ++user) {
            if (!phase_zero_address(user))
                continue;
            for (std::size_t c = 0; c < profile_.channels; ++c) {
                auto& reading = user_readings_[c * profile_.user_field_count + user];
                reading.micro = 0;
                reading.available = true;
            }
        }
        user_watchdog_reset_ = true;
    }

    void project(std::uint32_t now, Sample* samples, std::size_t count) {
        update_user_watchdog(now);
        for (std::size_t c = 0; c < std::min(count, std::size_t(profile_.channels)); ++c) {
            auto& channel = channels_[c];
            auto s = channel.sample;
            const auto fresh = [&](Field f) {
                return channel.seen[std::size_t(f)] &&
                       now - channel.seen[std::size_t(f)] < freshness_;
            };
            s.valid = channel.received && now - channel.received < 4000;
            s.available_fields = (fresh(Field::voltage) ? 1 : 0) | (fresh(Field::current) ? 2 : 0) |
                                 (fresh(Field::power) ? 4 : 0);
            s.auxiliary_fields = 4;
            if (fresh(Field::frequency))
                s.auxiliary_fields |= 1;
            if (fresh(Field::factor))
                s.auxiliary_fields |= 2;
            const Field auxiliary[]{Field::apparent, Field::reactive, Field::phase_angle,
                Field::import_reactive, Field::export_reactive, Field::total_active,
                Field::total_reactive, Field::import_power, Field::export_power,
                Field::maximum_power, Field::resettable_active};
            for (unsigned i = 0; i < 11; ++i)
                if (fresh(auxiliary[i]))
                    s.auxiliary_fields |= 1U << (4 + i);
            s.counter_fields =
                (fresh(Field::import_counter) ? 1 : 0) | (fresh(Field::export_counter) ? 2 : 0);
            if (profile_.total_is_import && fresh(Field::total_active) &&
                (!profile_.prefer_import || !(s.counter_fields & 1))) {
                s.import_counter = s.total_active;
                s.counter_fields |= 1;
            }
            if (!cycle_seen_)
                s.counter_fields = 0;
#if USE_ENERGY_HARDWARE_TOTAL
            const bool use_total = profile_.total_is_import && fresh(Field::total_active) &&
                                   (!profile_.prefer_import || !fresh(Field::import_counter));
            s.import_reference_valid = s.valid && (s.counter_fields & 1) &&
                                       (channel.reference_updates & (use_total ? 2 : 1));
            s.import_reference = s.import_counter;
#endif
            samples[c] = s;
        }
    }

  public:
#if USE_BERRY_ENERGY_API
    /** @brief Copies this channel's explicit source-event age without renewing or ticking it. */
    bool source_observation_age(std::size_t channel, std::uint8_t& output) const override {
        if (!channels_ || channel >= profile_.channels)
            return false;
        output = channels_[channel].observation_age.value();
        return true;
    }
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Marks native import totals handled by the source EnergyUpdateTotal core. */
    bool source_total_counter() const override { return true; }
#endif

    /** @brief Explicit shared-register maps describe common voltage, not matching readings. */
    bool common_voltage() const override {
        for (std::size_t i = 0; i < profile_.size; ++i)
            if (selected(profile_.registers[i]) && profile_.registers[i].field == Field::voltage &&
                profile_.registers[i].channel == common)
                return true;
        return false;
    }

    /** @brief One frequency register for a multiphase profile denotes its common frequency. */
    bool common_frequency() const override {
        unsigned rows = 0;
        for (std::size_t i = 0; i < profile_.size; ++i)
            if (selected(profile_.registers[i]) &&
                profile_.registers[i].field == Field::frequency) {
                if (profile_.registers[i].channel == common)
                    return true;
                ++rows;
            }
        return rows == 1;
    }

    Meter(const Configuration& c, Profile profile,
        std::unique_ptr<ConfiguredProfile> owned_profile = {})
        : owned_profile_(std::move(owned_profile)), profile_(profile),
          channels_(new (std::nothrow) Channel[c.channels]),
          watchdog_started_at_(platform::monotonic_milliseconds()),
          due_(watchdog_started_at_ + profile.startup_ms), attach_at_(watchdog_started_at_),
          freshness_(
              std::max<std::uint32_t>(4000, std::uint32_t(profile.size) * profile.interval_ms * 3)),
          bus_(c.identity.bus - 1), flags_(c.options.flags) {
        if (owned_profile_) {
            owned_profile_->bind();
            profile_ = owned_profile_->profile;
        }
        if (profile_.user_field_count)
            user_readings_.reset(
                new (std::nothrow) UserReading[c.channels * profile_.user_field_count]());
        for (std::size_t channel = 0; channel < c.channels; ++channel) {
            unit_addresses_[channel] =
                profile_.address_count
                    ? profile_.addresses[profile_.address_count == 1 ? 0 : channel]
                    : c.identity.address;
            std::size_t client{};
            while (client < client_count_ && unit_addresses_[channel] != client_addresses_[client])
                ++client;
            if (client == client_count_) {
                client_addresses_[client] = unit_addresses_[channel];
                ++client_count_;
            }
            channel_client_[channel] = static_cast<std::uint8_t>(client);
        }
        if (channels_)
            for (std::size_t i = 0; i < c.channels; ++i) {
                channels_[i].sample.use_absolute = true;
                channels_[i].sample.available_fields = 0;
            }
    }

    ~Meter() override {
        for (std::size_t i = 0; i < client_count_; ++i)
            modbus::close_client(clients_[i]);
    }

    bool allocated() const {
        return bool(channels_) && (!profile_.user_field_count || user_readings_);
    }

#if USE_MODBUS_ENERGY
    /** @brief Visits the named User rows of the source web table. A field without a GUI name, a
     * first-phase register or a current value stays hidden, as there.
     */
    bool visit_user_measurements(UserMeasurementVisitor visitor, void* context) const override {
        if (!user_readings_)
            return true;
        for (std::size_t user = 0; user < profile_.user_field_count; ++user) {
            const auto& field = profile_.user_fields[user];
            if (!field.gui_name[0] || !phase_zero_address(user))
                continue;
            for (std::size_t c = 0; c < profile_.channels; ++c) {
                const auto& reading = user_readings_[c * profile_.user_field_count + user];
                if (reading.available &&
                    !visitor(context, {field.gui_name.data(), field.unit.data(), reading.micro,
                                          user_decimals(field), c}))
                    return false;
            }
        }
        return true;
    }
#endif

#if USE_MODBUS_ENERGY
    /** @brief Appends each user field with a first-phase register under its JSON name, as the
     * source does: one value, or one per phase when every phase has a value.
     */
    bool write_json_fields(CommandOutput& output) const override {
        if (!user_readings_)
            return true;
        const auto count = profile_.channels;
        for (std::size_t user = 0; user < profile_.user_field_count; ++user) {
            const auto reading = [&](std::size_t c) -> const UserReading& {
                return user_readings_[c * profile_.user_field_count + user];
            };
            if (!phase_zero_address(user) || !reading(0).available)
                continue;
            bool phases = count > 1;
            for (std::size_t c = 1; c < count; ++c)
                phases = phases && reading(c).available;
            const auto& field = profile_.user_fields[user];
            const auto& name = field.json_name;
            if (!output.write(",") ||
                !output.json_string(
                    std::string_view(name.data(), std::char_traits<char>::length(name.data()))) ||
                !output.write(phases ? ":[" : ":"))
                return false;
            for (std::size_t c = 0; c < (phases ? count : 1); ++c)
                if ((c && !output.write(",")) ||
                    !write_user_value(output, reading(c).micro, user_decimals(field)))
                    return false;
            if (phases && !output.write("]"))
                return false;
        }
        return true;
    }
#endif

    bool online() const override {
        if (!client_count_ || !modbus::bus_available(bus_))
            return false;
        for (std::size_t i = 0; i < client_count_; ++i)
            if (!modbus::active(clients_[i]))
                return false;
        return true;
    }

    bool set_options(const Options& options) override {
        if (options.flags & ~profile_.allowed_flags || pending_)
            return false;
        if (flags_ != options.flags) {
            for (std::size_t c = 0; c < profile_.channels; ++c)
                channels_[c] = {};
            if (user_readings_)
                for (std::size_t i = 0; i < profile_.channels * profile_.user_field_count; ++i)
                    user_readings_[i] = {};
            for (std::size_t c = 0; c < profile_.channels; ++c)
                channels_[c].sample.use_absolute = true;
            step_ = 0;
            extensions_ = true;
            cycle_seen_ = false;
            watchdog_started_at_ = platform::monotonic_milliseconds();
            user_watchdog_reset_ = false;
        }
        flags_ = options.flags;
        return true;
    }

    void poll(std::uint32_t now, Sample* samples, std::size_t count) override {
#if USE_BERRY_ENERGY_API
        for (std::size_t c = 0; c < profile_.channels; ++c)
            (void)channels_[c].observation_age.poll(now, false);
#endif
#if USE_ENERGY_HARDWARE_TOTAL
        for (std::size_t c = 0; c < profile_.channels; ++c)
            channels_[c].reference_updates = 0;
#endif
        if (reached(now, attach_at_)) {
            attach_at_ = now + 5000;
            for (std::size_t i = 0; i < client_count_; ++i)
                if (!modbus::active(clients_[i]))
                    (void)modbus::open_client(bus_, client_addresses_[i], clients_[i]);
        }
        if (pending_) {
            modbus::Response response{};
            if (modbus::take_response(clients_[pending_client_], response)) {
                accept(response, now);
                pending_ = false;
                bool cycle_end = true;
                for (auto next = end_; next < profile_.size; ++next)
                    if (selected(profile_.registers[next]))
                        cycle_end = false;
                if (cycle_end)
                    cycle_seen_ = true;
                step_ = end_ % profile_.size;
                due_ = now + profile_.interval_ms;
            }
        }
        if (!pending_ && modbus::bus_available(bus_) && reached(now, due_)) {
            for (std::size_t skipped = 0;
                skipped < profile_.size && !selected(profile_.registers[step_]); ++skipped)
                step_ = (step_ + 1) % profile_.size;
            const auto& r = profile_.registers[step_];
            if (selected(r)) {
                const auto channel = r.channel == common ? 0 : r.channel;
                pending_client_ = channel_client_[channel];
                current_unit_address_ = unit_addresses_[channel];
                if (!modbus::active(clients_[pending_client_])) {
                    due_ = now + profile_.interval_ms;
                    project(now, samples, count);
                    return;
                }
                requested_address_ = r.address;
                std::uint16_t size = words(r.encoding);
                end_ = step_ + 1;
                if (profile_.block_words) {
                    requested_address_ = profile_.block_start;
                    size = profile_.block_words;
                    end_ = profile_.size;
                } else if (profile_.high_rate && (flags_ & 1)) {
                    while (end_ < profile_.size) {
                        const auto& next = profile_.registers[end_];
                        if (!selected(next) || next.encoding != r.encoding ||
                            next.address != requested_address_ + size ||
                            size + words(next.encoding) > 80)
                            break;
                        size += words(next.encoding);
                        ++end_;
                    }
                }
                const std::uint8_t request[]{std::uint8_t(requested_address_ >> 8),
                    std::uint8_t(requested_address_), std::uint8_t(size >> 8), std::uint8_t(size)};
                modbus::Transaction transaction{};
                transaction.response_data_size = 1 + 2 * size;
                transaction.match = modbus::ResponseMatch::byte_count;
                transaction.timeout_ms = 1000;
                transaction.retries = 2;
                if (modbus::submit(clients_[pending_client_], profile_.function, request,
                        sizeof(request), transaction) == modbus::Result::success)
                    pending_ = true;
                else
                    due_ = now + profile_.interval_ms;
            }
        }
        project(now, samples, count);
    }
};

std::unique_ptr<Device> create_meter(
    const Configuration& c, Profile profile, std::unique_ptr<ConfiguredProfile> owned_profile) {
    if (c.channels != profile.channels || !profile.size || c.identity.bus < 1 ||
        c.identity.bus > modbus::kMaximumBuses || !c.identity.address || c.identity.address > 247 ||
        c.options.flags & ~profile.allowed_flags || c.calibration.power || c.calibration.current ||
        c.calibration.voltage || profile.user_field_count > maximum_user_fields ||
        (profile.address_count && profile.address_count != 1 &&
            profile.address_count != profile.channels))
        return {};
    for (const auto& pin : c.identity.pins)
        if (pin.function != PinFunction::none)
            return {};
    if (profile.address_count) {
        if (profile.addresses[0] != c.identity.address)
            return {};
        for (std::size_t i = 0; i < profile.address_count; ++i)
            if (!profile.addresses[i] || profile.addresses[i] > 247)
                return {};
    }
    modbus::BusConfiguration bus{};
    if (!modbus::configuration(c.identity.bus - 1, bus) || bus.uart.data_bits != 8 ||
        bus.uart.stop_bits != profile.stop_bits || bus.uart.parity != profile.parity ||
        (profile.baud && bus.uart.baud != profile.baud))
        return {};
    for (std::size_t i = 0; i < profile.size; ++i) {
        const auto& r = profile.registers[i];
        if (!r.denominator || (r.channel != common && r.channel >= profile.channels) ||
            std::uint32_t(r.address) + words(r.encoding) > 65536 ||
            (r.field == Field::custom && r.user_index >= profile.user_field_count))
            return {};
    }
    auto meter =
        std::unique_ptr<Meter>(new (std::nothrow) Meter(c, profile, std::move(owned_profile)));
    if (!meter || !meter->allocated())
        return {};
    return meter;
}
} // namespace

std::unique_ptr<Device> create(const Configuration& c, const Profile& profile) {
    return create_meter(c, profile, {});
}

#if USE_MODBUS_ENERGY
std::unique_ptr<Device> create_configured(const Configuration& c) {
    auto resolved = c;
    std::unique_ptr<char[]> file_profile;
    if (resolved.modbus_profile.empty()) {
        file_profile.reset(new (std::nothrow) char[Configuration::kMaximumModbusProfile]);
        std::size_t size{};
        if (!file_profile ||
            filesystem::read_file("/modbus.json", file_profile.get(),
                Configuration::kMaximumModbusProfile, size) != filesystem::ReadResult::succeeded ||
            !size)
            return {};
        /** @brief Tasmota uses /modbus.json as its first map source when no inline map is set.
         * Newmota bounds this compatibility file to the same per-instance map capacity.
         */
        resolved.modbus_profile = std::string_view(file_profile.get(), size);
    }
    if (resolved.modbus_profile.size() > Configuration::kMaximumModbusProfile)
        return {};
    auto parsed = std::unique_ptr<ConfiguredProfile>(new (std::nothrow) ConfiguredProfile{});
    if (!parsed || !parse_configured_profile(resolved, *parsed))
        return {};
    parsed->bind();
    return create_meter(c, parsed->profile, std::move(parsed));
}
#endif
} // namespace newmota::energy::registers
