/*
  energy_service.cpp - Selected energy lifecycle and durable wide accounting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Primary CSE retry policy reference: Tasmota xnrg_02_cse7766.ino,
  Copyright (C) 2021 Theo Arends, GPL version 3 or later.
  Retained Yesterday reference: Tasmota xdrv_03_esp32_energy.ino,
  Copyright (C) 2023 Theo Arends, GPL version 3 or later.
*/

/** @file energy_service.cpp
 * @brief Selected energy lifecycle and durable wide accounting.
 */

#include "services/energy/energy_service.h"
#include "newmota/platform/platform.h"
#include "core/command/command.h"
#include "core/storage/filesystem.h"
#include "newmota/log.h"
#include "newmota_features.h"
#include "core/hardware/runtime_pin_claims.h"
#include "services/energy/energy_storage.h"
#if USE_MODBUS_ENERGY
#include "drivers/energy/modbus_meter.h"
#endif
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace newmota::energy {
#define DECLARE_ENERGY(symbol, id, name, channels, flags, invalid_frames)                          \
    std::unique_ptr<Device> symbol(const Configuration&);
NEWMOTA_ENERGY_DRIVERS(DECLARE_ENERGY)
#undef DECLARE_ENERGY

namespace {
#define ENERGY_ENTRY(symbol, id, name, channels, flags, invalid_frames)                            \
    Driver{id, name, channels, symbol, Options{flags, invalid_frames}},
constexpr std::array<Driver, NEWMOTA_ENERGY_DRIVER_COUNT> drivers{
    NEWMOTA_ENERGY_DRIVERS(ENERGY_ENTRY)};
#undef ENERGY_ENTRY
Service owner;
constexpr std::size_t kMaximumChannels = Service::kMaximumChannels;
using namespace storage;
constexpr std::size_t kFileCapacity = kHeaderSize + 4 + 50 + 14 + 69 + 5 +
                                      Configuration::kMaximumModbusProfile + 402 * kMaximumChannels;
constexpr char kPath[] = "/.newmota/energy.bin";
constexpr char kPrevious[] = "/.newmota/energy.prev";
constexpr char kTemporary[] = "/.newmota/energy.tmp";
#if USE_MODBUS_ENERGY
constexpr char kModbusMapPath[] = "/modbus.json";
#endif

/** @brief Allocates only the actual bounded file length; sparse configurations do not reserve max
 * state. */
filesystem::ReadResult read_record(
    const char* path, std::unique_ptr<std::uint8_t[]>& bytes, std::size_t& size) {
    filesystem::EntryType type{};
    std::uint32_t length{};
    const auto result = filesystem::stat(path, type, length);
    if (result != filesystem::ReadResult::succeeded)
        return result;
    if (type != filesystem::EntryType::file || length < 10 || length > kFileCapacity)
        return filesystem::ReadResult::failed;
    bytes.reset(new (std::nothrow) std::uint8_t[length]);
    if (!bytes)
        return filesystem::ReadResult::failed;
    return filesystem::read_file(path, bytes.get(), length, size);
}

} // namespace

Service& service() {
    return owner;
}

const Driver* Service::driver(std::uint16_t id) const {
    for (const auto& item : drivers)
        if (item.id == id)
            return &item;
    return nullptr;
}

void Service::initialize(std::uint32_t now) {
#if USE_CSE7766
    primary_invalid_initialized_ = false;
    primary_invalid_retry_at_ = now;
#endif
    now_ = checkpoint_ = now;
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size{};
    auto result = read_record(kPath, bytes, size);
    bool loaded = result == filesystem::ReadResult::succeeded && validate(bytes.get(), size);
    const bool active_missing = result == filesystem::ReadResult::not_found;
    const bool active_foreign = result == filesystem::ReadResult::succeeded &&
                                has_magic(bytes.get(), size) && bytes[4] != kFormat;
    if (!loaded) {
        result = read_record(kPrevious, bytes, size);
        loaded = result == filesystem::ReadResult::succeeded && validate(bytes.get(), size);
    }
    if (!loaded) {
        const bool previous_missing = result == filesystem::ReadResult::not_found;
        const bool previous_foreign = result == filesystem::ReadResult::succeeded &&
                                      has_magic(bytes.get(), size) && bytes[4] != kFormat;
        // Another record format is not energy data for this firmware; start empty.
        if ((active_missing || active_foreign) && (previous_missing || previous_foreign)) {
            if (active_foreign || previous_foreign)
                AddLog(LogLevel::info, "NRG: unsupported energy record format; starting empty");
            return;
        }
        storage_fault_ = true;
        AddLog(LogLevel::error, "NRG: invalid or unreadable energy storage; writes blocked");
        return;
    }
    const auto* cursor = bytes.get() + kHeaderSize;
    const auto* end = bytes.get() + size - 4;
    auto& instance = provider_;
    auto& config = instance.configuration;
    (void)read_configuration(cursor, end, config);
    (void)read_schedule(cursor, end, instance.tariff_schedule);
    instance.total_time = get(cursor, 4);
    TariffUsage values{};
    std::uint8_t flags{};
    (void)read_tariff_usage(cursor, end, values, flags);
    instance.tariff_usage_fault = flags & 2;
    if (flags & 1) {
        instance.tariff_usage.reset(new (std::nothrow) TariffUsage(values));
        if (!instance.tariff_usage) {
            storage_fault_ = true;
            return;
        }
#if !USE_ENERGY_TARIFF_COMPAT
        if (!instance.tariff_usage->incomplete) {
            instance.tariff_usage->incomplete = true;
            dirty_ = true;
        }
#endif
    }
    instance.live_daily_day = get(cursor, 4);
    flags = static_cast<std::uint8_t>(get(cursor, 1));
    instance.live_daily_day_known = flags & 1;
    instance.live_daily_collected = flags & 2;
    if (!config.modbus_profile.empty()) {
        auto profile =
            std::unique_ptr<char[]>(new (std::nothrow) char[config.modbus_profile.size()]);
        if (!profile) {
            storage_fault_ = true;
            return;
        }
        std::memcpy(profile.get(), config.modbus_profile.data(), config.modbus_profile.size());
        modbus_profile_ = std::move(profile);
        config.modbus_profile =
            std::string_view(modbus_profile_.get(), config.modbus_profile.size());
    }
    if (!config.identity.driver)
        return;
    instance.channels.reset(new (std::nothrow) Channel[config.channels]);
    instance.samples.reset(new (std::nothrow) Sample[config.channels]);
    if (!instance.channels || !instance.samples) {
        storage_fault_ = true;
        return;
    }
    const auto energy = [&] { return EnergyValue{get(cursor, 8), std::uint32_t(get(cursor, 4))}; };
    const auto signed_energy = [&](SignedEnergy& value) {
        value.magnitude = energy();
        value.negative = get(cursor, 1);
    };
    for (std::size_t c = 0; c < config.channels; ++c) {
        auto& channel = instance.channels[c];
        auto& account = channel.account;
        account.imported = energy();
        account.exported = energy();
        account.import_remainder = get(cursor, 2);
        account.export_remainder = get(cursor, 2);
        account.pulse_remainder = get(cursor, 4);
        account.import_baseline = energy();
        account.export_baseline = energy();
        account.import_resets = get(cursor, 4);
        account.export_resets = get(cursor, 4);
        account.baseline_fields = get(cursor, 1);
        account.import_increment_remainder = get(cursor, 4);
        account.import_increment_denominator = get(cursor, 4);
        account.export_increment_remainder = get(cursor, 4);
        account.export_increment_denominator = get(cursor, 4);
        channel.calibration = {std::uint32_t(get(cursor, 4)), std::uint32_t(get(cursor, 4)),
            std::uint32_t(get(cursor, 4)), std::uint32_t(get(cursor, 4))};
        CalendarData calendar{};
        (void)read_calendar(cursor, end, calendar, flags);
        channel.calendar_fault = flags & 2;
        if (flags & 1) {
            channel.calendar.reset(new (std::nothrow) CalendarData(calendar));
            if (!channel.calendar) {
                storage_fault_ = true;
                return;
            }
        }
        TariffData tariff{};
        (void)read_tariff(cursor, end, tariff, flags);
        channel.tariff_fault = flags & 2;
        if (flags & 1) {
            channel.tariff.reset(new (std::nothrow) TariffData(tariff));
            if (!channel.tariff) {
                storage_fault_ = true;
                return;
            }
        }
        signed_energy(account.total_adjustment);
        account.total_fault = get(cursor, 1);
        signed_energy(account.export_adjustment);
        channel.power_delta = get(cursor, 2);
        for (auto& margin : channel.margins)
            margin = get(cursor, 2);
        for (auto& setting : channel.max_power)
            setting = get(cursor, 2);
        auto& live = channel.live_daily;
        for (auto* value : {&live.value, &live.carry, &live.offset})
            signed_energy(*value);
        flags = static_cast<std::uint8_t>(get(cursor, 1));
        live.present = flags & 1;
        live.fault = flags & 2;
        live.incomplete = flags & 4;
#if USE_ENERGY_LIVE_DAILY
        /** EnergySnsInit restores retained Today as a starting offset, not current-boot
         * measured Wh. Preserve Newmota's exact carry independently across restart.
         */
        if (live.present && (live.offset.magnitude != live.value.magnitude ||
                                live.offset.negative != live.value.negative)) {
            live.offset = live.value;
            dirty_ = true;
        }
#endif
        for (auto& setting : channel.max_energy)
            setting = get(cursor, 2);
        signed_energy(channel.yesterday);
        channel.yesterday_present = get(cursor, 1);
    }
    // Opening is deferred until the normal GPIO resolver selects this identity.
    instance.sampled_at = now;
    instance.generation = ++generation_;
}

bool Service::prepare_restart() {
    bool ready = true;
    if (provider_.device && !provider_.device->prepare_restart())
        ready = false;
    return save() && ready;
}

bool Service::save() {
    if (storage_fault_)
        return false;
    if (!dirty_)
        return true;
    std::size_t capacity = kHeaderSize + 4;
    {
        const auto& instance = provider_;
        capacity +=
            70 + instance.configuration.modbus_profile.size() + (instance.tariff_usage ? 68 : 0);
        for (std::size_t c = 0; c < instance.configuration.channels; ++c)
            capacity += 204 + (instance.channels[c].calendar ? 126 : 0) +
                        (instance.channels[c].tariff ? 72 : 0);
    }
    if (capacity > kFileCapacity)
        return false;
    auto bytes = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[capacity]);
    auto verified = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[capacity]);
    if (!bytes || !verified)
        return false;
    auto* cursor = bytes.get();
    std::memcpy(cursor, "NMEG", 4);
    cursor += 4;
    put(cursor, kFormat, 1);
    {
        const auto& instance = provider_;
        const auto& config = instance.configuration;
        put(cursor, config.identity.driver, 2);
        put(cursor, config.identity.bus, 1);
        put(cursor, config.identity.address, 1);
        put(cursor, config.channels, 1);
        put(cursor, config.options.flags, 1);
        put(cursor, config.options.invalid_power_frames, 2);
        put(cursor, config.calibration.power, 4);
        put(cursor, config.calibration.voltage, 4);
        put(cursor, config.calibration.current, 4);
        put(cursor, config.calibration.frequency, 4);
        for (const auto& binding : config.identity.pins) {
            put(cursor, unsigned(binding.function), 1);
            put(cursor, binding.pin.value(), 2);
        }
        put(cursor, config.modbus_profile.size(), 2);
        if (!config.modbus_profile.empty())
            std::memcpy(cursor, config.modbus_profile.data(), config.modbus_profile.size());
        cursor += config.modbus_profile.size();
        for (const auto& boundary : instance.tariff_schedule.minutes)
            for (const auto minute : boundary)
                put(cursor, minute, 2);
        put(cursor, instance.tariff_schedule.forced, 1);
        put(cursor, instance.tariff_schedule.weekend, 1);
        put(cursor, instance.total_time, 4);
        put(cursor,
            (instance.tariff_usage ? 1 : 0) | (instance.tariff_usage_fault ? 2 : 0) |
                (instance.tariff_usage && instance.tariff_usage->incomplete ? 4 : 0),
            1);
        if (instance.tariff_usage) {
            for (const auto* pair :
                {&instance.tariff_usage->usage, &instance.tariff_usage->returned})
                for (const auto& value : *pair) {
                    put(cursor, value.magnitude.wh, 8);
                    put(cursor, value.magnitude.micro_wh, 4);
                    put(cursor, value.negative, 1);
                }
            put(cursor, instance.tariff_usage->pending_export.wh, 8);
            put(cursor, instance.tariff_usage->pending_export.micro_wh, 4);
            put(cursor, instance.tariff_usage->started_local, 4);
        }
        /** EnergySaveState checkpoints an invalid clock as source day0. Excluded collection
         * preserves its retained discriminator; save failure leaves live RAM unchanged.
         */
        const bool known =
            instance.live_daily_day_known && (!USE_ENERGY_LIVE_DAILY || calendar_valid_);
        put(cursor, known ? instance.live_daily_day : 0, 4);
        put(cursor, known | (USE_ENERGY_LIVE_DAILY ? 2 : 0), 1);
        for (std::size_t c = 0; c < config.channels; ++c) {
            const auto& account = instance.channels[c].account;
            put(cursor, account.imported.wh, 8);
            put(cursor, account.imported.micro_wh, 4);
            put(cursor, account.exported.wh, 8);
            put(cursor, account.exported.micro_wh, 4);
            put(cursor, account.import_remainder, 2);
            put(cursor, account.export_remainder, 2);
            put(cursor, account.pulse_remainder, 4);
            put(cursor, account.import_baseline.wh, 8);
            put(cursor, account.import_baseline.micro_wh, 4);
            put(cursor, account.export_baseline.wh, 8);
            put(cursor, account.export_baseline.micro_wh, 4);
            put(cursor, account.import_resets, 4);
            put(cursor, account.export_resets, 4);
            put(cursor, account.baseline_fields, 1);
            put(cursor, account.import_increment_remainder, 4);
            put(cursor, account.import_increment_denominator, 4);
            put(cursor, account.export_increment_remainder, 4);
            put(cursor, account.export_increment_denominator, 4);
            const auto& calibration = instance.channels[c].calibration;
            put(cursor, calibration.power, 4);
            put(cursor, calibration.voltage, 4);
            put(cursor, calibration.current, 4);
            put(cursor, calibration.frequency, 4);
            put_calendar(
                cursor, instance.channels[c].calendar.get(), instance.channels[c].calendar_fault);
            const auto& channel = instance.channels[c];
            put(cursor, (channel.tariff ? 1 : 0) | (channel.tariff_fault ? 2 : 0), 1);
            if (channel.tariff)
                for (const auto* bucket : {&channel.tariff->totals[0], &channel.tariff->totals[1],
                         &channel.tariff->unassigned})
                    for (const auto* energy : {&bucket->imported, &bucket->exported}) {
                        put(cursor, energy->wh, 8);
                        put(cursor, energy->micro_wh, 4);
                    }
            put(cursor, account.total_adjustment.magnitude.wh, 8);
            put(cursor, account.total_adjustment.magnitude.micro_wh, 4);
            put(cursor, account.total_adjustment.negative, 1);
            put(cursor, account.total_fault, 1);
            put(cursor, account.export_adjustment.magnitude.wh, 8);
            put(cursor, account.export_adjustment.magnitude.micro_wh, 4);
            put(cursor, account.export_adjustment.negative, 1);
            put(cursor, instance.channels[c].power_delta, 2);
            for (const auto margin : instance.channels[c].margins)
                put(cursor, margin, 2);
            for (const auto setting : instance.channels[c].max_power)
                put(cursor, setting, 2);
            const auto& live = instance.channels[c].live_daily;
            for (const auto* value : {&live.value, &live.carry, &live.offset}) {
                put(cursor, value->magnitude.wh, 8);
                put(cursor, value->magnitude.micro_wh, 4);
                put(cursor, value->negative, 1);
            }
            put(cursor,
                live.present | (live.fault ? 2 : 0) |
                    ((live.incomplete || (!USE_ENERGY_LIVE_DAILY && live.present)) ? 4 : 0),
                1);
            for (const auto setting : instance.channels[c].max_energy)
                put(cursor, setting, 2);
            const auto& yesterday = instance.channels[c].yesterday;
            put(cursor, yesterday.magnitude.wh, 8);
            put(cursor, yesterday.magnitude.micro_wh, 4);
            put(cursor, yesterday.negative, 1);
            put(cursor, instance.channels[c].yesterday_present, 1);
        }
    }
    const auto payload = std::size_t(cursor - bytes.get());
    put(cursor, crc(bytes.get(), payload), 4);
    const auto size = std::size_t(cursor - bytes.get());
    using M = filesystem::MutationResult;
    if (filesystem::ensure_directory("/.newmota") != M::succeeded ||
        filesystem::write_file(kTemporary, bytes.get(), size) != M::succeeded)
        return false;
    std::size_t read_size{};
    if (filesystem::read_file(kTemporary, verified.get(), capacity, read_size) !=
            filesystem::ReadResult::succeeded ||
        read_size != size || std::memcmp(bytes.get(), verified.get(), size) ||
        !validate(verified.get(), size))
        return false;
    // Preserve an existing verified recovery copy when the active file is corrupt.
    std::size_t active_size{};
    const auto active = read_record(kPath, verified, active_size);
    if (active != filesystem::ReadResult::succeeded && active != filesystem::ReadResult::not_found)
        return false;
    const bool active_valid =
        active == filesystem::ReadResult::succeeded && validate(verified.get(), active_size);
    if (active_valid) {
        if (filesystem::remove_if_exists(kPrevious) != M::succeeded ||
            filesystem::rename(kPath, kPrevious) != M::succeeded)
            return false;
    } else if (active != filesystem::ReadResult::not_found &&
               filesystem::remove_if_exists(kPath) != M::succeeded)
        return false;
    if (filesystem::rename(kTemporary, kPath) != M::succeeded)
        return false;
#if USE_ENERGY_LIVE_DAILY
    if (!calendar_valid_) {
        auto& instance = provider_;
        instance.live_daily_day = 0;
        instance.live_daily_day_known = false;
    }
#endif
    dirty_ = false;
    return true;
}

bool Service::configure(const Configuration& config, bool apply_driver_defaults) {
    runtime_pins::EnergyClaimScope claims;
    if (storage_fault_)
        return false;
    auto resolved = config;
    auto& current = provider_;
    const auto current_driver = current.configuration.identity.driver;
    if (resolved.modbus_profile.empty() &&
        (!resolved.identity.driver ||
            (built_one_of<28>(resolved.identity.driver) &&
                (!current_driver || current_driver == 28)) ||
            (built_one_of<33>(resolved.identity.driver) &&
                current.configuration.identity == resolved.identity))) {
        resolved.modbus_profile = current.configuration.modbus_profile;
    }
    const auto* descriptor = driver(resolved.identity.driver);
    if (resolved.identity.driver &&
        (!descriptor || !resolved.channels || resolved.channels > descriptor->maximum_channels ||
            resolved.channels > kMaximumChannels))
        return false;
    if (!resolved.identity.driver && resolved.channels)
        return false;
    if (!resolved.identity.valid() ||
        resolved.modbus_profile.size() > Configuration::kMaximumModbusProfile)
        return false;
    if (current.device && current.device->configuration_locked())
        return false;
    if (current.configuration.identity == resolved.identity) {
        // Channel shape is fixed while a device is configured. Reapplying its resources must
        // preserve separate options, calibration, and every accumulated counter.
        if (current.configuration.channels != resolved.channels)
            return false;
        if (resolved.identity.driver && !current.device) {
            current.device = descriptor->create(current.configuration);
#if USE_CSE7766
            if (current.device) {
                primary_invalid_initialized_ = false;
                primary_invalid_retry_at_ = now_;
            }
#endif
#if USE_ENERGY_COUNTER_FILTER
            if (current.device)
                for (std::size_t c = 0; c < current.configuration.channels; ++c) {
                    current.channels[c].source_total_started = false;
                    current.channels[c].source_total_rejected = false;
                }
#endif
            if (current.device && current.device->independent_calibration())
                for (std::size_t c = 0; c < current.configuration.channels; ++c)
                    if (!current.device->calibrate_channel(c, current.channels[c].calibration)) {
                        current.device.reset();
                        break;
                    }
        }
        return !resolved.identity.driver || current.device != nullptr;
    }
    Provider replacement{};
    replacement.configuration = resolved;
    replacement.sampled_at = now_;
    if (resolved.identity.driver) {
        replacement.channels.reset(new (std::nothrow) Channel[resolved.channels]);
        replacement.samples.reset(new (std::nothrow) Sample[resolved.channels]);
        if (!replacement.channels || !replacement.samples)
            return false;
        replacement.device = descriptor->create(resolved);
        if (!replacement.device)
            return false;
        if (apply_driver_defaults &&
            replacement.device->initial_calibration(resolved.calibration)) {
            if (!replacement.device->calibrate(resolved.calibration))
                return false;
            replacement.configuration.calibration = resolved.calibration;
        }
        for (std::size_t c = 0; c < resolved.channels; ++c)
            replacement.channels[c].calibration = resolved.calibration;
    }
    if (generation_ == INT32_MAX)
        return false;
    replacement.generation = ++generation_;
    /* As in Tasmota, another meter or other pins keep the totals and energy settings. Calibration
     * and hardware counter baselines belong to the driver, so only the same driver keeps them.
     * Swapping the kept state makes a second call undo the first.
     */
    const bool same_driver = current_driver == resolved.identity.driver;
    const auto swap_kept = [&](Provider& a, Provider& b) {
        using std::swap;
        if (!a.channels || !b.channels)
            return;
        swap(a.tariff_schedule, b.tariff_schedule);
        swap(a.total_time, b.total_time);
        swap(a.live_daily_day, b.live_daily_day);
        swap(a.live_daily_day_known, b.live_daily_day_known);
        swap(a.live_daily_collected, b.live_daily_collected);
#if USE_ENERGY_BALANCED_DAILY
        swap(a.balanced, b.balanced);
        swap(a.balanced_fault, b.balanced_fault);
#endif
        swap(a.tariff_usage, b.tariff_usage);
        swap(a.tariff_usage_fault, b.tariff_usage_fault);
        const auto channels = std::min(a.configuration.channels, b.configuration.channels);
        for (std::size_t c = 0; c < channels; ++c) {
            auto& x = a.channels[c];
            auto& y = b.channels[c];
            if (same_driver) {
                swap(x.account, y.account);
                swap(x.calibration, y.calibration);
            } else {
                swap(x.account.imported, y.account.imported);
                swap(x.account.exported, y.account.exported);
                swap(x.account.import_remainder, y.account.import_remainder);
                swap(x.account.export_remainder, y.account.export_remainder);
                swap(x.account.total_adjustment, y.account.total_adjustment);
                swap(x.account.total_fault, y.account.total_fault);
                swap(x.account.export_adjustment, y.account.export_adjustment);
            }
            swap(x.live_daily, y.live_daily);
            swap(x.yesterday, y.yesterday);
            swap(x.yesterday_present, y.yesterday_present);
            swap(x.calendar, y.calendar);
            swap(x.calendar_fault, y.calendar_fault);
            swap(x.tariff, y.tariff);
            swap(x.tariff_fault, y.tariff_fault);
            swap(x.max_energy, y.max_energy);
            swap(x.max_power, y.max_power);
            swap(x.power_delta, y.power_delta);
            swap(x.margins, y.margins);
        }
    };
    swap_kept(current, replacement);
    if (same_driver && replacement.device && replacement.device->independent_calibration())
        for (std::size_t c = 0; c < resolved.channels; ++c)
            if (!replacement.device->calibrate_channel(c, replacement.channels[c].calibration)) {
                swap_kept(current, replacement);
                return false;
            }
    auto previous = std::move(current);
    current = std::move(replacement);
    dirty_ = true;
    if (save()) {
#if USE_CSE7766
        primary_invalid_initialized_ = false;
        primary_invalid_retry_at_ = now_;
#endif
        if (resolved.modbus_profile.empty())
            modbus_profile_.reset();
        return true;
    }
    swap_kept(previous, current);
    current = std::move(previous);
    return false;
}

#if USE_CSE7766
/** @brief Keeps the primary CSE threshold in the existing SetOption settings authority.
 * Startup zero becomes128 before decoding; a failed durable normalization is retried,
 * with128 applied meanwhile. Runtime zero after initialization is never normalized.
 */
void Service::apply_primary_invalid_power() {
    auto& primary = provider_;
    if (!primary.device || primary.configuration.identity.driver != 2 || !primary_invalid_read_)
        return;
    auto value = primary_invalid_read_(primary_invalid_context_);
    if (!primary_invalid_initialized_) {
        if (!value) {
            value = 128;
            if (primary_invalid_write_ &&
                static_cast<std::int32_t>(now_ - primary_invalid_retry_at_) >= 0) {
                if (primary_invalid_write_(primary_invalid_context_, value))
                    primary_invalid_initialized_ = true;
                else {
                    AddLog(LogLevel::error, "NRG: SetOption39 default save failed; retry in60s");
                    primary_invalid_retry_at_ = now_ + 60000;
                }
            }
        } else
            primary_invalid_initialized_ = true;
    }
    primary.device->set_primary_invalid_power(value);
}
#endif

void Service::poll(std::uint32_t now) {
    /** A command may already have polled after the application's captured loop timestamp.
     * Keep sample time monotonic across that later loop poll, including uint32 wrap.
     */
    if (static_cast<std::int32_t>(now - now_) < 0)
        now = now_;
    now_ = now;
#if USE_ENERGY_BALANCED_DAILY
    if (now >= 11000)
        balanced_rollover_armed_ = true;
#endif
    no_export_today_ = daily_policy_ && daily_policy_(daily_policy_context_);
#if USE_ENERGY_PERIOD_COMPAT
    synchronize_period();
#endif
#if USE_ENERGY_CALENDAR
    calendar_valid_ = calendar_clock_ &&
                      calendar_clock_(calendar_context_, now, calendar_local_, calendar_daylight_);
    {
        auto& instance = provider_;
        if (instance.configuration.identity.driver && calendar_valid_ && !instance.total_time) {
            instance.total_time = calendar_local_;
            dirty_ = true;
        }
#if USE_ENERGY_BALANCED_DAILY
        if (instance.configuration.identity.driver && !instance.balanced &&
            !instance.balanced_fault) {
            instance.balanced.reset(
                new (std::nothrow) BalancedDaily(instance.configuration.channels));
            if (!instance.balanced || !instance.balanced->carries)
                instance.balanced_fault = true;
            else if (instance.channels && instance.channels[0].calendar &&
                     instance.channels[0].calendar->days[0].present) {
                // Read the retained day before this poll's Calendar prepass can rotate it.
                instance.balanced->observed_day = instance.channels[0].calendar->days[0].day;
                instance.balanced->day_known = true;
            }
        }
#endif
#if USE_ENERGY_TARIFF_COMPAT
        if (instance.configuration.identity.driver && !instance.tariff_usage) {
            instance.tariff_usage.reset(new (std::nothrow) TariffUsage);
            if (instance.tariff_usage)
                instance.tariff_usage->started_local = calendar_valid_ ? calendar_local_ : 0;
            else
                instance.tariff_usage_fault = true;
            dirty_ = true;
        }
#endif
#if USE_ENERGY_LIVE_DAILY
        initialize_live_daily(instance);
#endif
        for (std::size_t c = 0; instance.channels && c < instance.configuration.channels; ++c) {
            auto& channel = instance.channels[c];
            if (!channel.calendar) {
                channel.calendar.reset(new (std::nothrow) CalendarData);
                if (!channel.calendar) {
                    channel.calendar_fault = true;
#if USE_ENERGY_TARIFF
                    channel.tariff_fault = true;
#endif
                    dirty_ = true;
                    continue;
                }
                dirty_ = true;
            }
#if USE_ENERGY_TARIFF
            if (!channel.tariff) {
                channel.tariff.reset(new (std::nothrow) TariffData);
                if (!channel.tariff)
                    channel.tariff_fault = true;
                dirty_ = true;
            }
#endif
            channel.calendar->incoming = {};
            const auto previous_day = channel.calendar->days[0];
            if (!channel.calendar->observe(calendar_local_, calendar_valid_, no_export_today_)) {
                channel.calendar_fault = true;
                dirty_ = true;
            } else if (previous_day.present != channel.calendar->days[0].present ||
                       previous_day.day != channel.calendar->days[0].day)
                dirty_ = true;
        }
    }
#endif
#if USE_ENERGY_POWER_POLICY
    apply_primary_power();
#endif
#if USE_CSE7766
    apply_primary_invalid_power();
#endif
    if (auto& instance = provider_; instance.device && instance.channels) {
        if (!instance.export_reporting_initialized) {
            instance.export_reporting = instance.device->export_reporting_default();
            instance.export_reporting_initialized = true;
        }
#if USE_ENERGY_TARIFF_COMPAT || USE_ENERGY_BALANCED_DAILY || USE_ENERGY_LIVE_DAILY
        std::array<DirectionalEnergy, kMaximumChannels> confirmed_deltas{};
#endif
        const auto elapsed = now - instance.sampled_at;
        std::array<EnergyValue, kMaximumChannels> previous_exports{};
        for (std::size_t c = 0; c < instance.configuration.channels; ++c)
            previous_exports[c] = instance.channels[c].account.exported;
        // No extrapolation across a stalled scheduler or boot; protocol staleness is per device.
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            auto& channel = instance.channels[c];
            auto* confirmed =
                USE_ENERGY_CALENDAR && channel.calendar ? &channel.calendar->incoming : nullptr;
            if (elapsed <= 3000 && channel.sample.valid && (channel.sample.available_fields & 4) &&
                !channel.sample.use_pulses && !channel.sample.use_absolute && elapsed &&
                channel.sample.power) {
                if (!channel.account.integrate(channel.sample.power, elapsed, confirmed)) {
                    channel.sample.valid = false;
                    AddLog(LogLevel::error, "NRG: energy accumulator exhausted");
                } else
                    dirty_ = true;
            }
        }
        auto* samples = instance.samples.get();
        for (std::size_t c = 0; c < instance.configuration.channels; ++c)
            samples[c] = {};
        {
            runtime_pins::EnergyClaimScope claims;
            instance.device->poll(now, samples, instance.configuration.channels);
        }
#if USE_ENERGY_CALIBRATION_LEARNING
        capture_calibration(instance);
#endif
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            auto& channel = instance.channels[c];
            auto* confirmed =
                USE_ENERGY_CALENDAR && channel.calendar ? &channel.calendar->incoming : nullptr;
            channel.sample = samples[c];
#if USE_ENERGY_COUNTER_FILTER
            const bool accept_import = accept_counter_import(instance, channel, samples[c]);
#else
            constexpr bool accept_import = true;
#endif
            if (samples[c].increment_fields & 1) {
                if (channel.account.add_increment(samples[c].import_increment,
                        samples[c].increment_denominator, false, confirmed))
                    dirty_ = true;
                else {
                    channel.sample.valid = false;
                    AddLog(LogLevel::error, "NRG: import increment exhausted");
                }
            }
            if (samples[c].increment_fields & 2) {
                if (channel.account.add_increment(samples[c].export_increment,
                        samples[c].increment_denominator, true, confirmed))
                    dirty_ = true;
                else {
                    channel.sample.valid = false;
                    AddLog(LogLevel::error, "NRG: export increment exhausted");
                }
            }
            if (samples[c].import_floor_valid && samples[c].valid && accept_import) {
                const auto floor = samples[c].import_floor;
                auto& total = channel.account.imported;
                if (floor.micro_wh >= 1000000) {
                    channel.sample.valid = false;
                    AddLog(LogLevel::error, "NRG: invalid hardware energy floor");
                } else if (total.wh < floor.wh ||
                           (total.wh == floor.wh && total.micro_wh < floor.micro_wh)) {
                    total = floor;
                    dirty_ = true;
                }
            }
            if (samples[c].use_absolute && samples[c].valid) {
                for (unsigned direction = 0; direction < 2; ++direction) {
                    const auto mask = 1U << direction;
                    if (!(samples[c].counter_fields & mask) || (!direction && !accept_import))
                        continue;
                    const auto value =
                        direction ? samples[c].export_counter : samples[c].import_counter;
                    const auto previous = direction ? channel.account.export_baseline
                                                    : channel.account.import_baseline;
                    const bool changed =
                        !(channel.account.baseline_fields & mask) || value != previous;
                    if (!channel.account.observe_absolute(value, direction,
                            !direction && samples[c].import_counter_exporting, confirmed)) {
                        channel.sample.valid = false;
                        AddLog(LogLevel::error, "NRG: absolute accumulator exhausted");
                    } else if (changed)
                        dirty_ = true;
                }
            }
            if (samples[c].use_pulses && samples[c].pulse_count) {
                if (channel.account.add_pulses(
                        samples[c].pulse_count, samples[c].pulse_scale, confirmed))
                    dirty_ = true;
                else {
                    channel.sample.valid = false;
                    AddLog(LogLevel::error, "NRG: pulse accumulator exhausted");
                }
            }
            if (no_export_today_) {
                SignedEnergy advance{channel.account.exported, false};
                if (!advance.add(previous_exports[c], true) || advance.negative ||
                    !channel.account.total_adjustment.add(advance.magnitude)) {
                    channel.account.total_fault = true;
                    dirty_ = true;
                    AddLog(LogLevel::error, "NRG: Total adjustment exhausted");
                }
            }
            /** A selected remote floor constrains the Total projection as well as raw import.
             * This is an unmeasured correction: it must not enter daily/tariff observers.
             */
            if (samples[c].import_floor_valid && samples[c].valid && accept_import &&
                samples[c].import_floor.micro_wh < 1000000 && !channel.account.total_fault) {
                SignedEnergy projected{};
                const auto floor = samples[c].import_floor;
                if (!channel.account.compatibility_total(projected)) {
                    channel.account.total_fault = true;
                    dirty_ = true;
                } else if (projected.negative || projected.magnitude.wh < floor.wh ||
                           (projected.magnitude.wh == floor.wh &&
                               projected.magnitude.micro_wh < floor.micro_wh)) {
                    SignedEnergy net{}, adjustment{floor, false};
                    if (!net.add(channel.account.imported) ||
                        !net.add(channel.account.exported, true) ||
                        !adjustment.add(net.magnitude, !net.negative))
                        channel.account.total_fault = true;
                    else {
                        channel.account.total_adjustment = adjustment;
#if USE_ENERGY_CALENDAR
                        if (calendar_valid_) {
                            bool offset = false;
                            if (channel.calendar)
                                for (const auto& day : channel.calendar->days)
                                    if (day.present && day.day == calendar_local_ / 86400)
                                        offset = day.offset_nonzero;
                            instance.total_time =
                                offset ? calendar_local_ / 86400 * 86400 : calendar_local_;
                        }
#endif
                    }
                    dirty_ = true;
                }
            }
#if USE_ENERGY_HARDWARE_TOTAL
            reconcile_hardware_total(instance, channel, samples[c], accept_import);
#endif
#if USE_ENERGY_PERIOD_COMPAT
            observe_period(c, channel.calendar ? &channel.calendar->incoming : nullptr);
#endif
#if USE_ENERGY_TARIFF_COMPAT || USE_ENERGY_BALANCED_DAILY || USE_ENERGY_LIVE_DAILY
            /** Snapshot only confirmed advances before Calendar consumes its transient observer.
             * Lifetime seeds, policy adjustments and hardware floors never enter this observer.
             */
            if (channel.calendar)
                confirmed_deltas[c] = channel.calendar->incoming;
            else {
#if USE_ENERGY_TARIFF_COMPAT
                instance.tariff_usage_fault = true;
                dirty_ = true;
#endif
#if USE_ENERGY_BALANCED_DAILY
                instance.balanced_fault = true;
#endif
#if USE_ENERGY_LIVE_DAILY
                channel.live_daily.fault = true;
#endif
            }
#endif
#if USE_ENERGY_TARIFF
            if (channel.tariff && channel.calendar &&
                !channel.tariff->accrue(
                    channel.calendar->incoming, instance.tariff_schedule.active(calendar_local_,
                                                    calendar_valid_, calendar_daylight_))) {
                channel.tariff_fault = true;
                dirty_ = true;
                AddLog(LogLevel::error, "NRG: tariff accumulator exhausted");
            }
#endif
#if USE_ENERGY_CALENDAR
            if (channel.calendar &&
                !channel.calendar->observe(calendar_local_, calendar_valid_, no_export_today_)) {
                channel.calendar_fault = true;
                dirty_ = true;
                AddLog(LogLevel::error, "NRG: calendar accumulator exhausted");
            }
#endif
        }
#if USE_ENERGY_BALANCED_DAILY || USE_ENERGY_LIVE_DAILY
        std::array<std::uint8_t, kMaximumChannels> update_groups{};
#if USE_CSE7761 || USE_CSE7761_POWCT
        for (std::size_t c = 0; c < instance.configuration.channels; ++c)
            update_groups[c] = instance.device->accounting_update_group(c);
#endif
#if USE_ENERGY_LIVE_DAILY
        observe_live_daily(instance, confirmed_deltas.data(), update_groups.data());
#endif
#endif
#if USE_ENERGY_BALANCED_DAILY
        if (instance.balanced && !instance.balanced_fault &&
            !instance.balanced->observe(confirmed_deltas.data(), update_groups.data(),
                instance.configuration.channels, calendar_local_, calendar_valid_,
                balanced_rollover_armed_))
            instance.balanced_fault = true;
#endif
#if USE_ENERGY_TARIFF_COMPAT
        const auto old_pending =
            instance.tariff_usage ? instance.tariff_usage->pending_export : EnergyValue{};
        const bool was_incomplete = instance.tariff_usage && instance.tariff_usage->incomplete;
        /** Pinned EnergyUpdateToday gates native export on phase 0, then sums all finite phases.
         * Fresh native export counters are distinct from the runtime local-reporting flag.
         */
        if (instance.tariff_usage && !instance.tariff_usage_fault &&
            !instance.tariff_usage->observe(confirmed_deltas.data(),
                instance.configuration.channels,
                instance.tariff_schedule.active(
                    calendar_local_, calendar_valid_, calendar_daylight_),
                no_export_today_,
                instance.export_reporting ||
                    (samples[0].valid && (samples[0].counter_fields & 2)))) {
            instance.tariff_usage_fault = true;
            dirty_ = true;
            AddLog(LogLevel::error, "NRG: tariff usage totals exhausted");
        }
        if (instance.tariff_usage && (instance.tariff_usage->pending_export != old_pending ||
                                         instance.tariff_usage->incomplete != was_incomplete))
            dirty_ = true;
#endif
#if USE_PZEM004T || USE_PZEM_AC || USE_PZEM_DC
        PhysicalIdentity changed{};
        if (instance.device->identity_change(changed)) {
            const auto old = instance.configuration.identity;
            auto expected = old;
            expected.address = changed.address;
            bool committed = false;
            if (changed.valid() && changed == expected) {
                const bool was_dirty = dirty_;
                instance.configuration.identity = changed;
                dirty_ = true;
                committed = save();
                if (!committed) {
                    instance.configuration.identity = old;
                    dirty_ = was_dirty;
                    AddLog(
                        LogLevel::error, "NRG: address storage failed; requesting device rollback");
                }
            }
            instance.device->identity_result(committed);
        }
#endif
        instance.sampled_at = now;
    }
#if USE_ENERGY_MAX_ENERGY
    update_max_energy_clock();
#endif
#if USE_ENERGY_PROTECTION_POLICY
    if (protection_tick(now)) {
#if USE_ENERGY_MAX_POWER
        poll_max_power(now);
#endif
#if USE_ENERGY_MAX_ENERGY
        poll_max_energy();
#endif
    }
#endif
#if USE_ENERGY_POWER_DELTA
    poll_power_delta(now);
#endif
    if (now - checkpoint_ >= 60000) {
        checkpoint_ = now;
        if (!save())
            AddLog(LogLevel::error, "NRG: energy checkpoint failed");
    }
}

bool Service::set_options(const Options& options) {
    if (storage_fault_)
        return false;
    poll(now_);
    auto& instance = provider_;
#if USE_CSE7766
    /** Primary CSE has one durable authority; never mirror the threshold into two files. */
    if (instance.configuration.identity.driver == 2 && primary_invalid_write_) {
        if (!instance.device || options.flags || !options.invalid_power_frames ||
            options.invalid_power_frames > 255)
            return false;
        return primary_invalid_write_(primary_invalid_context_, options.invalid_power_frames);
    }
#endif
    const auto previous = instance.configuration.options;
    if (!instance.device || !instance.device->set_options(options))
        return false;
    std::array<std::uint8_t, 32> baselines{};
    const auto reset = instance.device->options_counter_fields(previous, options);
    for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
        auto& fields = instance.channels[c].account.baseline_fields;
        baselines[c] = fields;
        fields &= ~reset;
    }
    instance.configuration.options = options;
    dirty_ = true;
    if (save())
        return true;
    instance.configuration.options = previous;
    (void)instance.device->set_options(previous);
    for (std::size_t c = 0; c < instance.configuration.channels; ++c)
        instance.channels[c].account.baseline_fields = baselines[c];
    return false;
}

bool Service::export_reporting() const {
    const auto& instance = provider_;
    return instance.export_reporting_initialized
               ? instance.export_reporting
               : instance.device && instance.device->export_reporting_default();
}

bool Service::set_export_reporting(bool enabled, std::uint32_t generation) {
    if (!provider_.configuration.identity.driver)
        return false;
    poll(platform::monotonic_milliseconds());
    auto& instance = provider_;
    /** @brief Device control completion can change identity during poll; a checked script
     * handle must not set reporting on a replacement meter. Ordinary command callers use zero.
     */
    if (!instance.configuration.identity.driver ||
        (generation && instance.generation != generation))
        return false;
    instance.export_reporting = enabled;
    instance.export_reporting_initialized = true;
    return true;
}

#if USE_MODBUS_ENERGY
bool Service::set_modbus_profile(std::string_view profile) {
    runtime_pins::EnergyClaimScope claims;
    if (storage_fault_ || profile.size() > Configuration::kMaximumModbusProfile ||
        profile.find('\0') != std::string_view::npos)
        return false;
    auto& instance = provider_;
    const auto previous = instance.configuration;
    if (previous.identity.driver && previous.identity.driver != 28)
        return false;
    if (profile.empty() && previous.identity.driver == 28)
        return false;
    if (profile == previous.modbus_profile)
        return true;
    std::unique_ptr<char[]> profile_storage;
    if (!profile.empty()) {
        profile_storage.reset(new (std::nothrow) char[profile.size()]);
        if (!profile_storage)
            return false;
        std::memcpy(profile_storage.get(), profile.data(), profile.size());
    }
    auto updated = previous;
    updated.modbus_profile = profile.empty()
                                 ? std::string_view{}
                                 : std::string_view(profile_storage.get(), profile.size());
#if USE_MODBUS_ENERGY
    if (!profile.empty()) {
        bool valid = false;
        const auto first_channels = updated.channels ? updated.channels : std::uint8_t{1};
        const auto last_channels = updated.channels ? updated.channels : std::uint8_t{3};
        for (auto channels = first_channels; channels <= last_channels && !valid; ++channels) {
            auto validation_config = updated;
            validation_config.channels = channels;
            registers::ConfiguredProfile validation_profile{};
            valid = registers::parse_configured_profile(validation_config, validation_profile);
        }
        if (!valid)
            return false;
    }
#endif
    std::unique_ptr<Device> replacement;
    if (previous.identity.driver == 28) {
        const auto* descriptor = driver(28);
        if (!descriptor || !(replacement = descriptor->create(updated)))
            return false;
    }
    /** @brief Map replacement keeps physical accounts but invalidates counter interpretation.
     * Stage baseline validity in the same save as the new map so reboot cannot apply a
     * previous register/unit baseline. Numeric baselines are inert while their bits are clear.
     * No poll/callback runs during save; failure restores the old map, device and validity.
     */
    std::array<std::uint8_t, kMaximumChannels> baselines{};
    if (replacement)
        for (std::size_t c = 0; c < previous.channels; ++c) {
            baselines[c] = instance.channels[c].account.baseline_fields;
            instance.channels[c].account.baseline_fields = 0;
        }
    const auto was_dirty = dirty_;
    instance.configuration = updated;
    dirty_ = true;
    if (!save()) {
        instance.configuration = previous;
        if (replacement)
            for (std::size_t c = 0; c < previous.channels; ++c)
                instance.channels[c].account.baseline_fields = baselines[c];
        dirty_ = was_dirty;
        return false;
    }
    modbus_profile_ = std::move(profile_storage);
    if (replacement) {
        instance.device = std::move(replacement);
        /** @brief Old-map cached power must not integrate before the new device's first poll. */
        for (std::size_t c = 0; c < previous.channels; ++c) {
            instance.channels[c].sample = {};
            instance.samples[c] = {};
#if USE_ENERGY_COUNTER_FILTER
            instance.channels[c].source_total_started = false;
            instance.channels[c].source_total_rejected = false;
#endif
        }
        instance.sampled_at = now_;
    }
    return true;
}

bool Service::load_modbus_profile_file() {
    std::unique_ptr<char[]> contents(new (std::nothrow) char[Configuration::kMaximumModbusProfile]);
    if (!contents)
        return false;
    std::size_t size{};
    if (filesystem::read_file(kModbusMapPath, contents.get(), Configuration::kMaximumModbusProfile,
            size) != filesystem::ReadResult::succeeded ||
        !size)
        return false;
    return set_modbus_profile(std::string_view(contents.get(), size));
}

#endif

#if USE_GLOBAL_TEMPERATURE_COMMANDS
bool Service::visit_temperatures(bool (*visitor)(void*, std::int64_t), void* context) const {
    if (!visitor)
        return false;
    if (const auto& instance = provider_; instance.channels)
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            const auto& sample = instance.channels[c].sample;
            if ((sample.auxiliary_fields & (1U << 16)) &&
                !visitor(context, sample.temperature_millicelsius))
                return false;
        }
    return true;
}
#endif

bool Service::snapshot(std::size_t channel, Snapshot& output) const {
    const auto& instance = provider_;
    if (!instance.configuration.identity.driver || !channel ||
        channel > instance.configuration.channels || !instance.channels)
        return false;
    output = {instance.channels[channel - 1].sample, instance.channels[channel - 1].account,
        instance.configuration.options, instance.generation, instance.configuration.identity.driver,
        instance.configuration.channels, instance.device && instance.device->online(),
#if USE_BERRY_ENERGY_API
        instance.device && instance.device->script_writable()
#else
        false
#endif
    };
#if USE_BERRY_ENERGY_API
    /** @brief Observe the shared source gate without polling/applying driver policy; an
     * unbound observer retains source init=true.
     */
    output.power_on = !primary_power_ || primary_power_(primary_power_context_);
    /** @brief A configured but unopened device has unknown metadata, not a fabricated false. */
    output.observation_age_available = instance.device && instance.device->source_observation_age(
                                                              channel - 1, output.observation_age);
    output.topology_available = instance.device != nullptr;
    if (output.topology_available) {
        output.common_voltage = instance.device->common_voltage();
        output.common_frequency = instance.device->common_frequency();
    }
    output.sample_metadata_available = instance.device != nullptr;
    if (output.sample_metadata_available) {
        /** @brief V/I presence is the union of only the relevant current sample bits, not
         * power availability, sample validity or a persistent hardware capability inference.
         */
        output.direct_current = instance.channels[0].sample.direct_current;
        for (std::size_t c = 0; c < instance.configuration.channels; ++c) {
            output.voltage_available |= (instance.channels[c].sample.available_fields & 1) != 0;
            output.current_available |= (instance.channels[c].sample.available_fields & 2) != 0;
        }
    }
    output.overtemperature_available = instance.device != nullptr;
    if (output.overtemperature_available)
        output.overtemperature_requested = instance.device->use_overtemperature();
#endif
#if USE_BERRY_ENERGY_PERIOD_API
    /** @brief Copy only the current primary owner's pending interval, including when a send
     * preview exists. Reading cannot prepare/accept a token or change any publication state.
     */
    output.pending_period_available = !period_allocation_fault_ && periods_ &&
                                      period_identity_generation_ == instance.generation &&
                                      period_count_ == instance.configuration.channels &&
                                      channel <= period_count_ && !periods_[channel - 1].fault;
    if (output.pending_period_available)
        output.pending_period = periods_[channel - 1].pending;
#endif
#if USE_BERRY_ENERGY_ACCOUNTING_API
    output.live_daily = instance.channels[channel - 1].live_daily;
    output.yesterday = instance.channels[channel - 1].yesterday;
    output.yesterday_present = instance.channels[channel - 1].yesterday_present;
#endif
#if USE_BERRY_ENERGY_BALANCED_API
    /** @brief Runtime coverage is independent of dated history or LiveDaily incompleteness.
     * Copy only a complete owner shape; allocation/update faults must not become fake zeros.
     */
    output.balanced_daily_available = instance.balanced && !instance.balanced_fault &&
                                      instance.balanced->carries &&
                                      instance.balanced->count == instance.configuration.channels;
    if (output.balanced_daily_available)
        output.balanced_daily = instance.balanced->runtime;
#endif
    return true;
}

#if USE_BERRY_ENERGY_API
bool Service::script_inject(std::size_t channel, std::uint32_t generation, const Sample& sample) {
    poll(platform::monotonic_milliseconds());
    Snapshot current{};
    if (!snapshot(channel, current) || current.generation != generation || !current.writable)
        return false;
    if (!provider_.device || !provider_.device->inject(channel - 1, sample))
        return false;
    poll(platform::monotonic_milliseconds());
    return true;
}

bool Service::script_totals(
    std::size_t channel, std::uint32_t generation, EnergyValue imported, EnergyValue exported) {
    poll(platform::monotonic_milliseconds());
    Snapshot current{};
    if (storage_fault_ || imported.micro_wh >= 1000000 || exported.micro_wh >= 1000000 ||
        !snapshot(channel, current) || current.generation != generation || !current.writable)
        return false;
    auto& account = provider_.channels[channel - 1].account;
    const auto old_import = account.imported, old_export = account.exported;
    const auto old_dirty = dirty_;
    account.imported = imported;
    account.exported = exported;
    dirty_ = true;
    if (save())
        return true;
    account.imported = old_import;
    account.exported = old_export;
    dirty_ = old_dirty;
    return false;
}
#endif

#if USE_ENERGY_REGISTERS
bool Service::configure_registers(std::string_view payload) {
    if (!provider_.device)
        return false;
#if USE_ENERGY_DUMMY
    if (provider_.configuration.identity.driver == 30)
        apply_primary_power(); // Establish primary-only simulator identity before a first command.
#endif
    auto& instance = provider_;
#if USE_ENERGY_ANALOG
    std::string profile;
    if (instance.device->update_profile(payload, profile)) {
        if (storage_fault_ || profile.size() > Configuration::kMaximumModbusProfile ||
            profile.find('\0') != std::string::npos)
            return false;
        std::unique_ptr<char[]> retained(new (std::nothrow) char[profile.size()]);
        if (!retained && !profile.empty())
            return false;
        if (!profile.empty())
            std::memcpy(retained.get(), profile.data(), profile.size());
        const auto previous = instance.configuration.modbus_profile;
        const auto was_dirty = dirty_;
        const std::string_view updated(retained.get(), profile.size());
        if (!instance.device->apply_profile(updated))
            return false;
        instance.configuration.modbus_profile = updated;
        dirty_ = true;
        if (!save()) {
            instance.configuration.modbus_profile = previous;
            if (!instance.device->apply_profile(previous))
                instance.device.reset();
            dirty_ = was_dirty;
            return false;
        }
        modbus_profile_ = std::move(retained);
        return true;
    }
#endif
    return instance.device->configure_registers(payload);
}

bool Service::configure_device(Device& device, std::string_view payload) {
    return provider_.device.get() == &device && configure_registers(payload);
}

bool Service::write_registers(CommandOutput& output) const {
    return provider_.device && provider_.device->write_registers(output);
}
#endif

bool Service::control(Control operation, std::uint16_t value) {
    return !storage_fault_ && provider_.device && provider_.device->control(operation, value);
}

bool Service::write_control(CommandOutput& output) const {
    const auto& instance = provider_;
    const auto state = instance.device ? instance.device->control_status() : ControlStatus{};
    if (!output.format("{\"Phase\":%u,\"Operation\":%u,\"Value\":%lu,\"Result\":%u,\"Exception\":%"
                       "u,\"RollbackFailed\":%s",
            unsigned(state.phase), unsigned(state.operation),
            static_cast<unsigned long>(state.value), unsigned(state.result),
            unsigned(state.exception), state.rollback_failed ? "true" : "false"))
        return false;
    if (state.operation == Control::calibration &&
        !output.format(",\"Field\":%u", unsigned(state.field)))
        return false;
    return output.write("}");
}

bool Service::write_fields(CommandOutput& output, bool period) const {
    // As in Tasmota, a build without a selected meter reports no ENERGY object.
    return !provider_.configuration.identity.driver || write_energy_object(output, period);
}

} // namespace newmota::energy
