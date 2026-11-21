/*
  energy_service.h - Single-provider energy ownership and accounting

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  MaxPower/SO33 and Yesterday reference: Tasmota xdrv_03_esp32_energy.ino, settings.ino and
  support_command.ino, Copyright (C) Theo Arends, GPL version 3 or later.
  Modbus user-field resolution reference: Tasmota xnrg_29_modbus.ino,
  Copyright (C) 2022 Theo Arends, GPL version 3 or later.
*/

/** @file energy_service.h
 * @brief Single-provider energy ownership and accounting.
 */

#pragma once
#include "newmota_features.h"
#include "services/energy/energy_presentation.h"
#include "services/energy/energy_types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <string>
#include <type_traits>
#include "newmota/core/hardware/pin.h"

// Drivers that append members of their own to the ENERGY object.
#define NEWMOTA_ENERGY_JSON_FIELDS (USE_MODBUS_ENERGY || USE_BL0939 || USE_BL0940 || USE_BL0906)
// Drivers that show their chip temperature as a row of the energy card.
#define NEWMOTA_ENERGY_CHIP_TEMPERATURE (USE_BL0939 || USE_BL0940 || USE_BL0906)

namespace newmota {
class PowerService;
struct SensorReading;
class Settings;
class TimeService;
class Hardware;
enum class PowerChangeResult : std::uint8_t;
enum class PowerOperationSource : std::uint8_t;
class CommandOutput;
struct CommandRequest;
enum class CommandStatus : std::uint8_t;

namespace energy {
/** @brief Saturates a wide driver result into a 32-bit Sample measurement field.
 * @param value Integer or floating-point canonical value; NaN maps to zero.
 * @return value clamped to the int32_t range.
 */
template <typename T> [[nodiscard]] constexpr std::int32_t saturate_i32(const T value) {
    constexpr auto low = std::numeric_limits<std::int32_t>::min();
    constexpr auto high = std::numeric_limits<std::int32_t>::max();
    if constexpr (std::is_floating_point_v<T>) {
        if (!(value == value))
            return 0;
        return value <= T(low) ? low : value >= T(high) ? high : static_cast<std::int32_t>(value);
    } else if constexpr (std::is_signed_v<T>) {
        return value < low ? low : value > high ? high : static_cast<std::int32_t>(value);
    } else {
        return value > static_cast<std::make_unsigned_t<std::int32_t>>(high)
                   ? high
                   : static_cast<std::int32_t>(value);
    }
}

/** @brief Checks that a canonical value fits a 32-bit Sample measurement field. */
[[nodiscard]] constexpr bool fits_i32(const std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

/** @brief One driver-defined measurement in a unit of its own, for the dashboard card. */
struct UserMeasurement {
    const char* name; ///< Shown label; lives for the visitor call.
    const char* unit;
    std::int64_t micro; ///< Millionths of the unit.
    std::uint8_t decimals;
    std::size_t phase; ///< Zero-based.
};

using UserMeasurementVisitor = bool (*)(void*, const UserMeasurement&);

/** @brief Canonical instantaneous measurements in milli-V, milli-A and milli-W. */
struct Sample {
    // Members are grouped by alignment (64-bit, 32-bit, bytes) to avoid padding.
    std::int64_t power{};
    // Pulse devices report exact edge deltas with a rational micro-Wh scale / 36000.
    // Their measured power is never integrated a second time.
    std::uint64_t pulse_scale{};
    std::int64_t apparent_mva{}, reactive_mvar{};
    std::int64_t import_power_mw{}, export_power_mw{}, maximum_power_mw{};
    /** @brief Confirmed read-to-clear energy increments, consumed once independently of fields. */
    std::uint64_t import_increment{}, export_increment{};
    EnergyValue import_counter{}, export_counter{};
    EnergyValue total_active{}, import_reactive{}, export_reactive{}, total_reactive{},
        resettable_active{};
    /** @brief Monotonic imported-energy floor, used with measured-power accounting.
     * A fresh hardware lifetime total may raise the account, never lower or add to it.
     */
    EnergyValue import_floor{};
#if USE_ENERGY_HARDWARE_TOTAL
    /** @brief Fresh source EnergyUpdateTotal opportunity, independent of counter mode.
     * Only source-total devices may provide it. Drivers clear this on non-event polls;
     * availability/freshness of a cached counter is insufficient. Not a physical delta,
     * floor or baseline. The value is normalized nonnegative Wh plus microWh remainder.
     */
    EnergyValue import_reference{};
#endif

    /** Drivers range-check sources into the 32-bit fields (+-2.1 MV, +-2.1 MA). */
    std::int32_t voltage{}, current{};
    std::uint32_t pulse_count{};
    /** @brief Typed auxiliary fields: bit 0 frequency, 1 factor, 2 address, 3 DC type. */
    std::uint32_t auxiliary_fields{};
    std::uint32_t frequency_millihz{};
    std::int32_t power_factor_milli{};
    std::int32_t phase_angle_millidegrees{};
    std::int32_t temperature_millicelsius{}; /**< Auxiliary bit 16 means independently fresh chip
                                                temperature. */
    std::int32_t neutral_current_ma{};       /**< Auxiliary bit 15: shared neutral current. */
    std::int32_t battery_voltage_mv{}, battery_current_ma{}; /**< Auxiliary bits 17 and 18. */
    std::uint32_t increment_denominator{1};

    bool valid{};
    bool watchdog_reset{};  /**< Explicit available reset zeros, not a confirmed measurement. */
    bool source_retained{}; /**< Driver-updated source array remains available but physically stale.
                             */

    /** @brief Allows explicit source retained/reset arrays in presentation and reporting.
     * @return true for fresh measurements or driver-declared available source state;
     * physical accounting still requires valid observations or confirmed increments.
     */
    bool presentation_valid() const { return valid || watchdog_reset || source_retained; }

    std::uint8_t available_fields{7}; /**< Bits 0..2 mark voltage, current, and power. */
    bool use_pulses{};                /**< Pulse device; see pulse_count and pulse_scale. */
    // Counter and confirmed-increment devices must never also integrate measured power.
    bool use_absolute{};
    bool import_counter_exporting{}; /**< Route this import-counter delta to export; retain its
                                        baseline. */
    std::uint8_t
        counter_fields{}; // bit 0 import, bit 1 export; only available readings are observed.
    std::uint8_t address{};
    bool direct_current{};
    std::uint8_t increment_fields{};
    bool import_floor_valid{}; /**< import_floor holds a fresh hardware lifetime total. */
#if USE_ENERGY_HARDWARE_TOTAL
    bool import_reference_valid{}; /**< import_reference holds a fresh source total. */
#endif
};

/** @brief Device operation kinds accepted by the shared energy service. */
enum class Control : std::uint8_t { module_address, current_range, calibration, registers };
/** @brief Lifecycle phase reported for an asynchronous device operation. */
enum class ControlPhase : std::uint8_t { idle, queued, succeeded, failed, committing };

/** @brief Result of asking a driver to calibrate its own nonvolatile device registers. */
enum class CalibrationRequest : std::uint8_t { unsupported, rejected, queued };

/** @brief Shared asynchronous device-control status; field identifies calibration operations. */
struct ControlStatus {
    ControlPhase phase{};
    Control operation{};
    std::uint32_t value{};
    std::uint8_t result{}, exception{};
    bool rollback_failed{};
    CalibrationField field{CalibrationField::power};
};

/** @brief One dynamically allocated protocol owner per configured device. */
class Device {
  public:
    virtual ~Device() = default;

    /** @brief Reports whether the configured native resources are currently attached. */
    virtual bool online() const { return true; }

    /** @brief Identifies accounting-update boundaries within the latest poll vector.
     * @param channel Zero-based configured channel, visited in ascending order.
     * @return Same ID for a contiguous group; a changed ID flushes the current phase carries.
     * Default drivers submit one grouped vector; sequential source updates override this.
     */
#if USE_CSE7761 || USE_CSE7761_POWCT
    virtual std::uint8_t accounting_update_group(std::size_t channel) const {
        (void)channel;
        return 0;
    }
#endif

    /** @brief Declares one physical voltage measurement shared by this instance's channels.
     * This is topology metadata, never inferred from equal sample values.
     */
    virtual bool common_voltage() const { return false; }

    /** @brief Declares one physical frequency measurement shared by configured channels. */
    virtual bool common_frequency() const { return false; }

#if USE_BERRY_ENERGY_API
    /** @brief Copies an exact source-owned observation age for a zero-based channel.
     * @param channel Configured channel, never inferred from sample validity or transport state.
     * @param output Receives the raw age byte only when this model/mode exposes its real owner.
     * @return false if no faithful event/tick owner is wired; callers must report unknown.
     * This pure read must not tick, renew observations or run measurement-reset hooks.
     */
    virtual bool source_observation_age(std::size_t channel, std::uint8_t& output) const {
        (void)channel;
        (void)output;
        return false;
    }
#endif

#if USE_BERRY_ENERGY_API
    /** @brief Reports whether safe script writes/seeding are supported by this virtual owner. */
    virtual bool script_writable() const { return false; }
#endif

#if USE_ENERGY_POWER_POLICY
    /** @brief Supplies the source global observation gate only to the primary device.
     * @param on Any logical Power is ON or SetOption21 disables the source power check.
     * @return true only when this driver implements its model-specific gated behavior.
     * Default is unsupported: the service must not zero samples or discard counters generically.
     */
    virtual bool set_primary_power(bool on) {
        (void)on;
        return false;
    }
#endif
#if USE_ENERGY_DUMMY
    /** @brief Supplies actual logical Power bits to an explicit source channel simulator.
     * @param mask Bit zero is logical Power1; SetOption21 never sets these bits.
     * Default is unsupported; this is not a generic meter-to-relay association.
     */
    virtual void set_primary_power_mask(std::uint32_t mask) { (void)mask; }
#endif
    /** @brief Polls without waiting and copies current samples into caller-owned storage. */
    virtual void poll(std::uint32_t now, Sample* samples, std::size_t count) = 0;

    /** @brief Queues a device-specific operation without waiting for wire completion. */
    virtual bool control(Control, std::uint16_t) { return false; }

    /** @brief Reports asynchronous operation completion through the common energy output. */
    virtual ControlStatus control_status() const { return {}; }

    /** @brief Prevents instance replacement while a physical configuration write is unresolved. */
    virtual bool configuration_locked() const { return false; }

#if USE_PZEM004T || USE_PZEM_AC || USE_PZEM_DC
    /** @brief Proposes a wire-confirmed physical identity update while retaining accounts. */
    virtual bool identity_change(PhysicalIdentity&) const { return false; }

    /** @brief Acknowledges durable identity storage; false requests protocol rollback. */
    virtual void identity_result(bool) {}
#endif

#if USE_ENERGY_INJECT
    /** @brief Accepts virtual input only for drivers which explicitly support injection.
     * @param channel Zero-based channel within this instance.
     * @param sample Canonical reading and available-field mask to inject.
     * @return true when the selected driver accepts the sample.
     */
    virtual bool inject(std::size_t channel, const Sample& sample) {
        (void)channel;
        (void)sample;
        return false;
    }
#endif

    /** @brief Applies driver coefficients without reopening an existing device connection. */
    virtual bool calibrate(const Calibration&) { return false; }

    /** @brief Identifies absolute counters whose scale changes with a calibration update.
     * The service clears these baseline bits in the same durable transaction as the
     * coefficients, preserving accumulated totals and preventing artificial consumption.
     * @return Bit 0 for import and bit 1 for export; zero leaves baselines unchanged.
     */
    virtual std::uint8_t calibration_counter_fields(const Calibration&, const Calibration&) const {
        return 0;
    }

    /** @brief Reports support for independently retained per-channel coefficients. */
    virtual bool independent_calibration() const { return false; }

    /** @brief Identifies one common voltage coefficient shared by all channel accounts. */
    virtual bool shared_voltage_calibration() const { return false; }

    /** @brief Resolves default coefficients before durable storage or use.
     * @param value Requested or loaded coefficient values.
     * @return Canonical coefficients; default implementation preserves every field.
     */
    virtual Calibration canonical_calibration(Calibration value) const { return value; }

#if USE_ENERGY_CALIBRATION_LEARNING
    /** @brief Reports common coefficients already latched/applied from valid hardware data.
     * @param value Receives the effective common calibration, including retained native units.
     * @return true when a complete valid snapshot is available; false leaves settings alone.
     * Read-only/app-task contract: never polls, saves, resets baselines or mutates the device.
     * Independently calibrated devices require their own channel-aware protocol instead.
     */
    virtual bool learned_calibration(Calibration& value) const {
        (void)value;
        return false;
    }
#endif
    /** @brief Applies coefficients to one zero-based channel without changing other channels. */
    virtual bool calibrate_channel(std::size_t channel, const Calibration& calibration) {
        return !channel && calibrate(calibration);
    }

    /** @brief Returns the driver default for Tasmota coefficient reset/query operations. */
    virtual std::uint32_t default_coefficient(CalibrationField) const { return 0; }

    /** @brief Bounds unsigned primary Cal command input in this driver's native units.
     * @param field Selected coefficient quantity.
     * @return Inclusive parser maximum; driver calibration still validates the transaction.
     * Ordinary drivers retain the existing command limits. Signed-native commands use their
     * separate signed parser; this bound does not change reset/query aliases or Set references.
     */
    virtual std::uint32_t maximum_coefficient(CalibrationField field) const {
        (void)field;
        return INT32_MAX;
    }

#if USE_CSE7766
    /** @brief Supplies the primary CSE runtime SetOption39 threshold, including zero.
     * @param value Current source invalid-power retry limit; never changes physical counters.
     * Other device families ignore this CSE-specific policy.
     */
    virtual void set_primary_invalid_power(std::uint8_t value) { (void)value; }
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Identifies hardware totals passed through upstream EnergyUpdateTotal.
     * Defaults false for injected/custom counters and confirmed-increment providers.
     * Opt-in does not enable filtering outside SO39 range1..127.
     */
    virtual bool source_total_counter() const { return false; }
#endif

#if USE_ENERGY_OVERTEMP || USE_BERRY_ENERGY_API
    /** @brief Reports the driver's source global-temperature/all-output policy request.
     * This is metadata, not an enforcement toggle or a temperature-sensing capability.
     * Enforcement remains selected separately by USE_ENERGY_OVERTEMP.
     * @return true when this model requests the shared policy.
     */
    virtual bool use_overtemperature() const { return false; }
#endif

    /** @brief Reports native signed coefficients, including zero, stored as exact 32-bit patterns.
     * Commands/output use signed decimal for these devices; ordinary pulse/divisor
     * drivers retain unsigned calibration and their existing default-reset aliases.
     */
    virtual bool signed_calibration() const { return false; }

    /** @brief Supplies coefficients only for a newly configured physical identity.
     * Returning false preserves the supplied configuration. Persisted coefficients,
     * including an explicit native zero, never pass through this defaulting path.
     */
    virtual bool initial_calibration(Calibration&) const { return false; }

    /** @brief Selects decimal amperes for the CurrentSet command. */
    virtual bool primary_current_in_amperes() const { return false; }

    /** @brief Native Set reference units per milliampere, normally one; 100 retains centi-mA. */
    virtual std::uint32_t current_reference_scale() const { return 1; }

    /** @brief Whether source Cal payload1 restores a model-specific coefficient. */
    virtual bool coefficient_reset_alias() const { return false; }

    /** @brief Derives coefficients using a fresh reference from one zero-based channel.
     * @param channel Zero-based device channel.
     * @param field Measurement to calibrate.
     * @param reference Canonical milli-units; current uses current_reference_scale() units per mA.
     * @param output Receives coefficients without modifying live device state.
     * @return true when the fresh reference permits a supported coefficient calculation.
     */
    virtual bool calibration_for_channel(std::size_t channel, CalibrationField field,
        std::uint32_t reference, Calibration& output) const {
        return !channel && calibration_for(field, reference, output);
    }

    /** @brief Applies operating choices while preserving the physical accounting identity. */
    virtual bool set_options(const Options&) { return false; }

    /** @brief Identifies absolute baselines to clear when an accounting mode changes. */
    virtual std::uint8_t options_counter_fields(const Options&, const Options&) const { return 0; }

    /** @brief Returns the model-specific runtime local-export reporting default at initialization.
     */
    virtual bool export_reporting_default() const { return false; }

    /** @brief Validates an instance command and constructs its complete durable profile.
     * @param payload Driver-specific command and arguments.
     * @param profile Receives a bounded profile without modifying the live device.
     * @return true only when the command and generated profile are valid.
     */
#if USE_ENERGY_ANALOG
    virtual bool update_profile(std::string_view payload, std::string& profile) const {
        (void)payload;
        (void)profile;
        return false;
    }

    /** @brief Atomically validates/applies a complete retained profile without reclaiming pads.
     * @param profile Empty selects defaults; nonempty contains driver-specific retained values.
     * @return true when every field was valid and applied; false leaves live settings unchanged.
     */
    virtual bool apply_profile(std::string_view profile) {
        (void)profile;
        return false;
    }
#endif

#if USE_ENERGY_REGISTERS
    /** @brief Applies volatile device register settings; implementation validates atomically. */
    virtual bool configure_registers(std::string_view) { return false; }

    /** @brief Writes the device register settings as a complete JSON response. */
    virtual bool write_registers(CommandOutput&) const { return false; }
#endif

    /** @brief Quiesces device outputs before a firmware restart without releasing uncertain claims.
     */
    virtual bool prepare_restart() { return true; }

#if USE_MODBUS_ENERGY
    /** @brief Visits the driver's own rows after the common ones, like Tasmota's web sensor call.
     * @return false when the visitor stops.
     */
    virtual bool visit_user_measurements(UserMeasurementVisitor, void*) const { return true; }
#endif

#if NEWMOTA_ENERGY_JSON_FIELDS
    /** @brief Appends the driver's own members to the ENERGY object, like Tasmota's JSON append. */
    virtual bool write_json_fields(CommandOutput&) const { return true; }
#endif

#if NEWMOTA_ENERGY_CHIP_TEMPERATURE
    /** @brief Reports the chip temperature that the source shows in its web table.
     * @return false while the driver has none.
     */
    virtual bool chip_temperature(std::int32_t& millicelsius) const {
        (void)millicelsius;
        return false;
    }
#endif

    /** @brief Derives one coefficient from a known reference in canonical milli-units.
     * @param field Measurement field to calibrate.
     * @param reference Known milli-units; current is multiplied by current_reference_scale().
     * @param output Receives the updated coefficient set.
     * @return true when this driver can derive the requested coefficient.
     */
    virtual bool calibration_for(
        CalibrationField field, std::uint32_t reference, Calibration& output) const {
        (void)field;
        (void)reference;
        (void)output;
        return false;
    }

    /** @brief Queues a hardware calibration using a canonical milli-unit reference.
     * @param field Measurement field to calibrate.
     * @param reference Known value in canonical milli-units, including milli-Hz for frequency.
     * @return Whether the operation is unsupported, rejected, or queued asynchronously.
     */
    virtual CalibrationRequest request_calibration(
        CalibrationField field, std::uint32_t reference) {
        (void)field;
        (void)reference;
        return CalibrationRequest::unsupported;
    }
};

using Factory = std::unique_ptr<Device> (*)(const Configuration&);

/** @brief Constant feature-generated driver contract; state is allocated only on configuration. */
struct Driver {
    std::uint16_t id;
    const char* name;
    std::uint8_t maximum_channels;
    Factory create;
    Options default_options{}; /**< Applied only to newly configured physical identities. */
};

/** @brief Whether this build compiles energy driver @p id. A branch for a driver that is not
 * built folds to false, so its code leaves the image without a feature guard of its own.
 */
constexpr bool driver_built(std::uint16_t id) {
#define NEWMOTA_ENERGY_ID(symbol, driver, name, channels, flags, frames) driver,
    constexpr std::uint16_t built[]{NEWMOTA_ENERGY_DRIVERS(NEWMOTA_ENERGY_ID) 0};
#undef NEWMOTA_ENERGY_ID
    for (const auto driver : built)
        if (driver && driver == id)
            return true;
    return false;
}

/** @brief Whether @p id is one of the listed drivers that this build compiles. */
template <std::uint16_t... ids> constexpr bool built_one_of(std::uint16_t id) {
    return ((driver_built(ids) && id == ids) || ...);
}

/** @brief Value-only snapshot; no script receives pointers into device or account storage. */
struct Snapshot {
    Sample sample{};
    Account account{};
    Options options{};
    std::uint32_t generation{};
    std::uint16_t driver{};
    std::uint8_t channels{};
    bool online{}, writable{};
#if USE_BERRY_ENERGY_API
    /** @brief Copied driver policy metadata, independent of protection enforcement selection. */
    bool overtemperature_requested{};
    /** @brief False when configuration has no runtime device from which to read metadata. */
    bool overtemperature_available{};
    /** @brief Shared provider Power/SO21 predicate; a missing observer reports true.
     */
    bool power_on{true};
    /** @brief Copied Device topology; virtual option reads retain their stored flags instead. */
    bool common_voltage{}, common_frequency{};
    /** @brief False without a runtime device, including a retained configuration that failed open.
     */
    bool topology_available{};
    /** @brief Copied channel-1 AC/DC classification and any-channel V/I sample-presence bits.
     * These are current sample metadata, not persistent hardware capability declarations.
     */
    bool direct_current{}, voltage_available{}, current_available{};
    /** @brief False without a runtime Device; callers must not infer metadata from cached defaults.
     */
    bool sample_metadata_available{};
    /** @brief Copied raw source observation byte; not a Sample.valid-to-age conversion. */
    std::uint8_t observation_age{};
    /** @brief False for models/modes without an exact source-owned event/tick implementation. */
    bool observation_age_available{};
#endif
#if USE_BERRY_ENERGY_PERIOD_API
    /** @brief Copied signed primary-channel pending interval, never a mutable publish preview. */
    SignedEnergy pending_period{};
    /** @brief False for mismatched ownership or missing/faulted collection. */
    bool pending_period_available{};
#endif
#if USE_BERRY_ENERGY_ACCOUNTING_API
    /** @brief Copied source daily projections; no script can mutate their backing owners. */
    LiveDaily live_daily{};
    SignedEnergy yesterday{};
    bool yesterday_present{};
#endif
#if USE_BERRY_ENERGY_BALANCED_API
    /** @brief Copied meter-wide runtime sums; never a date-filtered Calendar bucket. */
    DirectionalEnergy balanced_daily{};
    /** @brief False for missing, faulted or incompletely allocated/shaped collection. */
    bool balanced_daily_available{};
#endif
};

#if USE_MATTER_ENERGY
/** @brief A one-channel meter in Matter units: mV, mA, mW, mVA, mvar, mHz and power factor in
 * 1/10000. */
struct MatterReading {
    /** Bits of present; an absent value is null in Matter. */
    enum : std::uint8_t {
        voltage_bit = 1,
        current_bit = 2,
        power_bit = 4,
        apparent_bit = 8,
        reactive_bit = 16,
        factor_bit = 32,
        frequency_bit = 64
    };

    std::int64_t voltage{}, current{}, power{}, apparent{}, reactive{}, frequency{}, factor{};
    std::uint64_t imported_mwh{}, exported_mwh{};
    std::uint8_t present{};
    bool direct_current{}, exports{};
};
#endif

/** @brief Owns the selected provider's lifecycle, accounting and output. */
class Service {
  public:
    static constexpr std::size_t kMaximumChannels =
        32; /**< Native phase/channel bound for the selected provider. */
    using CivilClock = bool (*)(
        void*, std::uint32_t now, std::uint32_t& local_seconds, bool& daylight);

    /** @brief Binds the application civil clock; callbacks execute only on the application task. */
    void bind_calendar_clock(CivilClock clock, void* context) {
        calendar_clock_ = clock;
        calendar_context_ = context;
    }

    /** @brief Changes the schedule using Tariff1/2/9-compatible payloads and saves
     * atomically.
     * @param index Boundary 1/2 or weekend option 9.
     * @param payload Time tuple, ON/OFF forced mode, or weekend 0/1.
     * @return false for invalid input or storage failure; previous schedule is retained.
     */
    bool configure_tariff(unsigned index, std::string_view payload);
    /** @brief Streams the bounded tariff schedule query. */
    bool write_tariff(CommandOutput& output) const;

    /** @brief Binds SetOption162 policy; queried only on the application task before accounting. */
    void bind_daily_policy(bool (*policy)(void*), void* context) {
        daily_policy_ = policy;
        daily_policy_context_ = context;
    }

    /** @brief Sets runtime-only local export reporting without changing durable direction routing.
     * @param enabled New volatile reporting state.
     * @param generation Optional checked handle identity; zero permits normal command routing.
     * @return false if absent or changed during the pre-mutation owner poll.
     */
    bool set_export_reporting(bool enabled, std::uint32_t generation = 0);
    /** @brief Returns runtime reporting state, false for an absent meter. */
    bool export_reporting() const;
    /** @brief Seeds local export projection without changing physical/daily/tariff/Total accounts.
     * @param channel One-based channel.
     * @param payload Signed integer Wh and optional meter-wide timestamp.
     * @param generation Optional checked identity; zero permits ordinary command routing.
     * @return false when disabled, invalid, out of range, changed identity, or durable save fails.
     */
    bool seed_export(std::size_t channel, std::string_view payload, std::uint32_t generation = 0);
    /** @brief Seeds a primary pre-Today base without mutating measured directional accounts.
     * @param channel One-based primary channel.
     * @param payload Signed integer Wh base and optional meter-wide timestamp. Without
     * trusted civil time, omitted timestamps remain the unknown epoch0.
     * @return false on invalid input, incomplete/faulted live Today, exact range exhaustion
     * or durable save failure. No calendar date is required or created.
     */
    bool seed_total(std::size_t channel, std::string_view payload);
    /** @brief Seeds one/two meter-wide tariff usage buckets, preserving all physical accounts.
     * @param exporting Selects Return rather than Usage buckets.
     * @param payload One/two signed integer-Wh seeds, bounded to int32.
     * @return false on invalid input, unavailable/faulted state, or failed durable save.
     */
    bool seed_tariff_usage(bool exporting, std::string_view payload);
    /** @brief Writes the source Usage/Export arrays. */
    bool write_tariff_usage(CommandOutput& output, std::string_view command) const;
    /** @brief Appends TotalTariff, or ExportTariff when exported, while a tariff is active. */
    bool write_tariff_fields(CommandOutput& output, bool exported) const;
    /** @brief Seeds compatibility energy without changing confirmed directions.
     * @param channel One-based primary channel.
     * @param yesterday Selects retained Yesterday instead of live Today.
     * Both accept an invalid clock without creating dated history.
     * @param payload Signed integer Wh and optional uint32 timestamp metadata.
     * @return false on invalid input, unavailable selected projection or durable save failure.
     */
    bool seed_daily(std::size_t channel, bool yesterday, std::string_view payload);
    /** @brief Streams primary live Today and retained Yesterday in numeric kWh phase vectors.
     * Incomplete/faulted live values and absent Yesterday projections read as zero; invalid
     * clock does not hide a complete live Today or explicitly seeded Yesterday.
     */
    bool write_daily(CommandOutput& output, std::string_view command) const;

    /** @brief Loads retained configurations, including records for absent compiled drivers. */
    void initialize(std::uint32_t now);
    /** @brief Selects the single provider from active Module/Template/GPIO bindings.
     * Another meter or other pins keep the retained totals and energy settings, as in Tasmota.
     */
    void select_hardware(const Hardware& hardware, const Settings& settings);
    /** @brief Polls every configured owner, expires samples, and checkpoints dirty totals. */
    void poll(std::uint32_t now);
    /** @brief Atomically checkpoints explicit wire-format records, retaining the previous copy. */
    bool save();
    /** @brief Quiesces every configured device and saves accounts before application restart. */
    bool prepare_restart();
    /** @brief Configures the provider; zero driver removes it after a durable save.
     * @param configuration Physical identity, channel count, policy, and driver settings.
     * @param apply_driver_defaults Uses descriptor options for a new physical identity;
     * false retains explicit caller-supplied options, including an explicit zero.
     * Existing identities and loaded sidecar options always retain their saved choices.
     * @return true when the replacement was created and persisted.
     */
    bool configure(const Configuration& configuration, bool apply_driver_defaults = true);
    /** @brief Durably changes supported driver coefficients without resetting accumulated energy.
     * @param power Power coefficient, with zero selecting the driver default.
     * @param voltage Voltage coefficient, with zero selecting the driver default.
     * @param current Current coefficient, with zero selecting the driver default.
     * @param frequency Native frequency coefficient; UINT32_MAX preserves each channel's value.
     * @return true when the driver accepted and the updated configuration was saved.
     */
    bool calibrate(std::uint32_t power, std::uint32_t voltage, std::uint32_t current,
        std::uint32_t frequency = UINT32_MAX);
    /** @brief Durably updates one one-based channel while preserving other channel settings. */
    bool calibrate_channel(std::size_t channel, const Calibration& calibration);
    /** @brief Reads a resolved coefficient for a primary/channel command response. */
    bool calibration_value(std::size_t channel, CalibrationField field, std::uint32_t& value) const;
    /** @brief Resolves the selected driver's unsigned primary Cal parser limit.
     * @param field Selected coefficient quantity.
     * @return Inclusive maximum, or zero when no live device is configured.
     */
    std::uint32_t maximum_coefficient(CalibrationField field) const;
    /** @brief Updates one coefficient; value one requests the selected driver's default. */
    bool set_coefficient(std::size_t channel, CalibrationField field, std::uint32_t value);
    /** @brief Calibrates one field against a known canonical milli-unit reference.
     * @param field Measurement field to calibrate.
     * @param reference Known value in canonical milli-units, including milli-Hz for frequency.
     * @param channel One-based channel; zero calibrates all supported channels atomically.
     * @param reference_divisor Sub-milliunit denominator; only supported fractional current is
     * accepted.
     * @return true when calibration was accepted or queued by the driver.
     */
    bool set_reference(CalibrationField field, std::uint32_t reference, std::size_t channel = 1,
        std::uint32_t reference_divisor = 1);
    /** @brief Calibrates power and current from a known resistive load, using measured voltage.
     * @param channel One-based channel; zero derives every independent channel atomically,
     * or updates the common coefficients once using channel one for shared calibration.
     */
    bool set_resistive_reference(
        std::size_t channel, std::uint32_t power_mw, std::uint32_t voltage_mv);
    /** @brief Writes primary Cal/Set output with resolved scalar or channel-array coefficients. */
    bool write_primary_calibration(CommandOutput&, CalibrationField field) const;
    /** @brief Reports whether this instance accepts signed native calibration register values. */
    bool signed_calibration() const;
    /** @brief Reports the driver's primary CurrentSet unit without changing canonical references.
     */
    bool primary_current_in_amperes() const;
    /** @brief Returns supported fractional current reference units per milliampere. */
    std::uint32_t current_reference_scale() const;
    /** @brief Durably changes supported options without discarding accumulated energy.
     * @param options New driver operating policy.
     * @return true when the driver accepts and the updated options are saved.
     */
    bool set_options(const Options& options);
#if USE_MODBUS_ENERGY
    /** @brief Stores and streams a bounded runtime Modbus energy map. */
    bool set_modbus_profile(std::string_view profile);
    /** @brief Imports `/modbus.json` into the provider and persists it after validation.
     * @return true when the bounded file was read, validated and stored successfully.
     */
    bool load_modbus_profile_file();
#endif
    /** @brief Queues a typed operation for one configured instance.
     * @param operation Device-specific operation kind.
     * @param value Operation value interpreted by the selected driver.
     * @return true when the driver accepts the operation.
     */
    bool control(Control operation, std::uint16_t value);
    /** @brief Streams typed device-operation status. */
    bool write_control(CommandOutput&) const;
#if USE_ENERGY_REGISTERS
    /** @brief Routes device commands, saving recognized options without replacing
     * identity/accounts. */
    bool configure_registers(std::string_view payload);
    /** @brief Applies a durable command from a registered output owned by a current device.
     * @param device Current configured device, never an uncommitted replacement.
     * @param payload Driver command routed through the same validation/storage transaction.
     * @return false if the device is not attached or the command/save fails.
     */
    bool configure_device(Device& device, std::string_view payload);
#endif

#if USE_ENERGY_POWER_POLICY || USE_ENERGY_MARGINS || USE_BERRY_ENERGY_API
    /** @brief Binds the primary source global power-on gate (any Power or SetOption21).
     * @param powered Read-only predicate evaluated by selected reporting/device consumers.
     * @param context Caller-owned predicate state retained until replaced.
     */
    void bind_primary_power(bool (*powered)(void*), void* context) {
        primary_power_ = powered;
        primary_power_context_ = context;
    }
#endif

#if USE_ENERGY_OVERTEMP
    /** @brief Binds shared canonical temperature/real-Power observation and forced shutdown.
     * @param read Returns true only with real Power ON and valid shared Celsius; fills SO42.
     * @param off Attempts every configured output OFF, returning false on an output failure.
     * @param context Caller-owned shared-service state retained until replacement.
     */
    void bind_overtemperature(bool (*read)(void*, std::uint32_t, std::int32_t&, std::uint8_t&),
        bool (*off)(void*), void* context) {
        overtemperature_read_ = read;
        overtemperature_off_ = off;
        overtemperature_context_ = context;
    }

    /** @brief Checks once per observed source second, without replay or cooldown/latch.
     * @param now Monotonic milliseconds, after current sensor observations are published.
     */
    void poll_overtemperature(std::uint32_t now);
#endif

#if USE_ENERGY_COUNTER_FILTER
    /** @brief Binds the source global hardware-total plausibility parameter.
     * @param read Returns Settings39; only1..127 enables the whole-kWh bandwidth.
     * @param context Caller-owned settings state retained until replacement.
     */
    void bind_counter_filter(std::uint8_t (*read)(void*), void* context) {
        counter_filter_read_ = read;
        counter_filter_context_ = context;
    }
#endif

#if USE_ENERGY_HARDWARE_TOTAL
    /** @brief Binds the global SetOption72 presentation policy, independent of device options.
     * @param enabled Reads the durable boolean without modifying settings or polling devices.
     * @param context Caller-owned settings context retained until replacement.
     */
    void bind_hardware_total(bool (*enabled)(void*), void* context) {
        hardware_total_enabled_ = enabled;
        hardware_total_context_ = context;
    }
#endif

#if USE_CSE7766
    /** @brief Binds the authoritative primary CSE SetOption39 settings transaction.
     * @param read Returns the retained byte parameter, including runtime zero.
     * @param write Durably updates the parameter; false preserves the previous settings.
     * @param context Caller-owned settings state, retained until replacement.
     */
    void bind_primary_invalid_power(
        std::uint8_t (*read)(void*), bool (*write)(void*, std::uint8_t), void* context) {
        primary_invalid_read_ = read;
        primary_invalid_write_ = write;
        primary_invalid_context_ = context;
    }
#endif

#if USE_ENERGY_DUMMY
    /** @brief Binds read-only logical channel Power bits for the selected simulator.
     * @param mask Callback returning real Power bits, without the SO21 global override.
     * @param context Caller-owned state retained until replacement.
     */
    void bind_primary_power_mask(std::uint32_t (*mask)(void*), void* context) {
        primary_power_mask_ = mask;
        primary_power_mask_context_ = context;
    }

    /** @brief Routes primary Dummy Cal/Set and unambiguous numeric EnergyConfig aliases. */
    CommandStatus dummy_config_command(const CommandRequest&, CommandOutput&);
#endif
#if USE_ENERGY_PROTECTION_BINDINGS
    /** @brief Binds the application-lifetime logical Power owner, independently of MQTT.
     * @param power Caller-owned output service, retained until app shutdown.
     */
    void bind_protection_power(PowerService& power) { protection_power_ = &power; }

    /** @brief Resolves source Power targets; channel0 selects all outputs, otherwise one phase.
     * @param channel Group0 or configured channel1..N.
     * @param selection Receives the configured Power endpoint mask.
     * @param on Receives current scoped Power bits only when ready.
     * @return true only when every selected endpoint is currently available.
     */
    bool protection_targets(std::size_t channel, std::uint32_t& selection, std::uint32_t& on) const;
    /** @brief Applies ready group/selected FORCE_OFF with their distinct native schedule policy.
     * @param channel Group0 or configured channel1..N; no inferred fallback.
     * @param source Explicit source provenance, including distinct MaxPower/MaxEnergy causes.
     * @return unavailable for unbound/missing endpoints, otherwise the Power owner result.
     */
    PowerChangeResult protection_trip(std::size_t channel, PowerOperationSource source);
    /** @brief Group retries use shared last-nonzero; channel retries use ordinary ON policy.
     * @param channel Group0 or configured channel1..N.
     * @param source Explicit source provenance, including distinct MaxPower/MaxEnergy causes.
     * @return unavailable for unbound/missing endpoints, otherwise the Power owner result.
     */
    PowerChangeResult protection_retry(std::size_t channel, PowerOperationSource source);
#endif
#if USE_ENERGY_STATUS9
    /** @brief Writes the primary retained reporting settings as one StatusPTH section.
     * @param output Bounded writer; no polling, history/accounting mutation or meter summing.
     * @return false on complete-output failure; unimplemented protection fields are omitted.
     * Single phase is scalar; multiple phases are arrays independently of SO129.
     */
    bool write_margin_status(CommandOutput& output) const;

    /** @brief Reports whether StatusPTH has a configured provider to describe. */
    [[nodiscard]] bool has_margin_status() const {
        return provider_.channels && provider_.configuration.channels;
    }
#endif
#if USE_ENERGY_MARGINS
    /** @brief Routes the six primary low/high threshold commands; suffix zero means all phases. */
    CommandStatus margin_command(const CommandRequest&, CommandOutput&);
#endif
#if USE_ENERGY_POWER_DELTA
    /** @brief Binds primary reporting to a copying telemetry sink and Power-operation source.
     * @param event Copies a complete event to the indicated telemetry suffix; false drops it.
     * @param sensor Requests a fresh SENSOR through the normal accepted-publication path.
     * @param revision Returns the global configured Power-operation revision.
     * @param context Shared caller-owned callback context, retained until replaced.
     * Callbacks must copy/request only and must not reenter EnergyService.
     */
    void bind_power_delta(bool (*event)(void*, std::string_view, std::string_view),
        void (*sensor)(void*), std::uint32_t (*revision)(void*), void* context);
    /** @brief Applies/query primary per-phase PowerDelta, with index zero selecting all phases. */
    CommandStatus power_delta_command(const CommandRequest&, CommandOutput&);
#endif
#if USE_ENERGY_REGISTERS
    /** @brief Writes the selected driver's register configuration response. */
    bool write_registers(CommandOutput&) const;
#endif
    /** @brief Copies one configured channel and its exact account into bounded caller storage.
     * @param channel One-based channel; its configured bounds are enforced.
     * @return false for an absent provider/channel, without exposing a retained pointer.
     */
    bool snapshot(std::size_t channel, Snapshot& output) const;
#if USE_GLOBAL_TEMPERATURE_COMMANDS
    /** @brief Visits reported auxiliary temperatures in instance/channel output order.
     * @param visitor Synchronous canonical milli-Celsius consumer; false stops enumeration.
     * @param context Caller-owned state, never retained.
     * @return false for an absent/failing consumer; no samples/accounts are modified.
     * Availability follows the independent temperature bit, not V/I/P validity.
     */
    bool visit_temperatures(bool (*visitor)(void*, std::int64_t), void* context) const;
#endif
#if USE_BERRY_ENERGY_API
    /** @brief Publishes a virtual sample only if the provider still has the expected generation. */
    bool script_inject(std::size_t channel, std::uint32_t generation, const Sample& sample);
    /** @brief Durably seeds virtual totals without modifying counter baselines or fractional carry.
     * @return false on stale identity, non-virtual device, invalid units or storage failure.
     */
    bool script_totals(
        std::size_t channel, std::uint32_t generation, EnergyValue imported, EnergyValue exported);
#endif

    /** @brief Binds runtime SetOption129 presentation; never changes any accumulated energy. */
    void bind_phase_presentation(bool (*policy)(void*), void* context) {
        phase_presentation_ = policy;
        phase_context_ = context;
    }

    /** @brief Binds the SO150 display override without changing device measurement topology.
     * @param policy Returns true to disable common voltage/frequency presentation.
     * @param context Opaque caller-owned policy context.
     */
    void bind_no_common_presentation(bool (*policy)(void*), void* context) {
        no_common_presentation_ = policy;
        no_common_context_ = context;
    }

    /** @brief Binds retained formatting without changing account units or sample precision.
     * @param policy Returns the current validated presentation record.
     * @param context Caller-owned settings context.
     */
    void bind_output_precision(OutputSettings (*policy)(void*), void* context) {
#if USE_ENERGY_PRECISION
        output_precision_ = policy;
        output_precision_context_ = context;
#else
        (void)policy;
        (void)context;
#endif
    }

    /** @brief Resolves output-only decimals; excluded builds retain their exact nine-place format.
     * @param quantity Selects the ENERGY numeric field, with factor fixed to two when selected.
     * @return Validated source precision, or Tasmota's default when precision is not built.
     */
    unsigned output_resolution(OutputQuantity quantity) const {
#if USE_ENERGY_PRECISION
        const auto value =
            output_precision_ ? output_precision_(output_precision_context_) : OutputSettings{};
#else
        const OutputSettings value{};
#endif
        return quantity == OutputQuantity::factor ? 2
               : quantity == OutputQuantity::weight
                   ? value.decimals[5]
                   : value.decimals[static_cast<unsigned>(quantity)];
    }

#if USE_MODBUS_USER_PRECISION
    /** @brief Binds existing ambient precision without sensor-unit/offset transformations.
     * @param read Returns the validated Temp/Hum/Press decimal setting for selector 26..28.
     * @param context Caller-owned Settings context retained until replacement.
     */
    void bind_modbus_ambient_precision(std::uint8_t (*read)(void*, std::uint8_t), void* context) {
        modbus_ambient_precision_ = read;
        modbus_ambient_context_ = context;
    }

    /** @brief Resolves Tasmota Modbus user-field precision at output time.
     * @param selector D21..25 maps V/I/P/energy/frequency; 26..28 maps ambient; 29 maps weight.
     * @return Current decimal places; 29 uses WeightRes and other selectors fall back to three.
     * Raw register units, samples, account values and retained JSON are never changed.
     */
    std::uint8_t modbus_user_resolution(std::uint8_t selector) const {
        constexpr OutputQuantity quantities[]{OutputQuantity::voltage, OutputQuantity::current,
            OutputQuantity::power, OutputQuantity::energy, OutputQuantity::frequency};
        if (selector >= 21 && selector <= 25)
            return static_cast<std::uint8_t>(output_resolution(quantities[selector - 21]));
        if (selector == 29)
            return static_cast<std::uint8_t>(output_resolution(OutputQuantity::weight));
        if (selector >= 26 && selector <= 28) {
            const auto value = modbus_ambient_precision_
                                   ? modbus_ambient_precision_(modbus_ambient_context_, selector)
                                   : std::uint8_t{1};
            return value <= 3 ? value : std::uint8_t{1};
        }
        return 3;
    }
#endif

    /** @brief Streams primary ENERGY fields without mutating samples or totals.
     * @param output Bounded output destination.
     * @param period Includes the explicitly prepared telemetry-only interval preview.
     * @return false when complete output cannot be written.
     */
    bool write_energy_object(CommandOutput& output, bool period = false) const;
    /** @brief Visits the dashboard card readings in the order of Tasmota's web table.
     * @param visitor Sensor reading sink; only Power keeps history.
     * @return false when the visitor stops.
     */
    bool visit_sensor_readings(bool (*visitor)(void*, const SensorReading&), void* context) const;
#if USE_MATTER_ENERGY
    /** @brief Copies the reading as ENERGY shows it, for the Matter electrical clusters.
     * @return false without a provider, or for a meter with more than one channel.
     */
    bool matter_reading(MatterReading& output) const;
#endif
    /** @brief Streams every configured instance without silently truncating channels.
     * @param output Bounded output destination.
     * @param period Includes the prepared primary interval only for actual SENSOR publication.
     * @return false when complete output cannot be written.
     */
    bool write_fields(CommandOutput&, bool period = false) const;
    /** @brief Captures the primary interval preview for one complete SENSOR document.
     * @param token Receives a generation-bound opaque acceptance token, zero without a primary.
     * @return true after a complete preview capture; faulted values are written as zero.
     */
    bool prepare_period(std::uint32_t& token);
    /** @brief Commits only a successfully sent preview; stale/zero tokens do nothing.
     * @param token Token returned by prepare_period for the accepted document.
     */
    void accept_period(std::uint32_t token);
    /** @brief Resets only a successfully seeded Today channel's runtime interval.
     * @param channel Zero-based primary channel. Other channels retain their intervals.
     */
    void reset_period(std::size_t channel);

    /** @brief Clears only a seeded primary phase carry, preserving already balanced sums.
     * @param channel Zero-based primary phase.
     */
    void reset_balanced_carry(std::size_t channel);

    /** @brief Returns the selected constant descriptor or nullptr. */
    const Driver* driver(std::uint16_t id) const;

#if USE_ENERGY_PROTECTION_POLICY
    /** @brief Binds application-lifetime source settings and event time; MQTT is optional.
     * @param settings Retained SO33 retry count and event TimeFormat owner.
     * @param time Shared civil/UTC time owner; synchronization is not required for enforcement.
     */
    void bind_protection_policy(const Settings& settings, const TimeService& time);
    /** @brief Binds copying status publication and immediate SENSOR requests.
     * @param event Returns queue acceptance; false is logged, never suppresses protection.
     * @param sensor Requests a fresh SENSOR document without advancing Period itself.
     * @param context Application-lifetime callback context.
     */
    void bind_protection_events(bool (*event)(void*, std::string_view, std::string_view, bool),
        void (*sensor)(void*), void* context);
#endif
#if USE_ENERGY_MAX_ENERGY
    /** @brief Dispatches primary MaxEnergy/Start without enabling the MaxPower engine. */
    CommandStatus max_energy_command(const CommandRequest&, CommandOutput&);
#endif
#if USE_ENERGY_MAX_POWER
    /** @brief Dispatches primary MaxPower/Hold/Window, default phase1 and explicit phase0 fanout.
     */
    CommandStatus max_power_command(const CommandRequest&, CommandOutput&);
#endif
  private:
#if USE_ENERGY_PROTECTION_POLICY
    /** @brief Bounded source event kinds; energy events intentionally have unindexed JSON keys. */
    enum class ProtectionEvent {
        power_trip,
        power_exhausted,
        power_retry,
        energy_trip,
        energy_start
    };
    const Settings* protection_settings_{};
    const TimeService* protection_time_{};
    bool (*protection_event_sink_)(void*, std::string_view, std::string_view, bool){};
    void (*protection_sensor_)(void*){};
    void* protection_event_context_{};
    std::uint32_t protection_revision_{}, protection_at_{};
    std::uint8_t protection_steady_{};
    bool protection_started_{};
    /** @brief Captures one shared source gate before either protection engine acts. */
    bool protection_tick(std::uint32_t now);
    /** @brief Formats bounded source events; queue failures never suppress output actions. */
    void publish_protection_event(ProtectionEvent kind, std::size_t channel,
        std::uint16_t watts = 0, const SignedEnergy* daily = nullptr);
#endif
#if USE_ENERGY_MAX_ENERGY
    /** @brief Source states0 ready,1 monitoring,2 tripped,3 awaiting a trusted start-hour entry. */
    struct MaxEnergyPhase {
        std::uint8_t state{};
        bool unavailable{};
    };

    std::unique_ptr<MaxEnergyPhase[]> max_energy_phases_;
    std::size_t max_energy_count_{};
    std::uint32_t max_energy_identity_{}, max_energy_midnight_{};
    std::uint8_t max_energy_hour_{};
    bool max_energy_midnight_seen_{};
    /** @brief Reconciles volatile phases and processes trusted midnight/hour transitions.
     * @param observe false for commands, which may allocate but must not manufacture clock events.
     */
    void update_max_energy_clock(bool observe = true);
    /** @brief Executes one source check against complete RTC-independent live daily values. */
    void poll_max_energy();
#endif
#if USE_ENERGY_MAX_POWER
    /** @brief Volatile source counters; allocated only for configured primary phases. */
    struct MaxPowerPhase {
        std::uint16_t hold{}, window{};
        std::uint8_t retry{};
        bool unavailable{};
    };

    std::unique_ptr<MaxPowerPhase[]> max_power_phases_;
    std::size_t max_power_count_{};
    std::uint32_t max_power_identity_{};
    /** @brief Runs one observed source second, never replays missed observations. */
    void poll_max_power(std::uint32_t now);
#endif
#if USE_ENERGY_PROTECTION_BINDINGS
    PowerService* protection_power_{};
#endif
#if USE_ENERGY_PERIOD_COMPAT
    /** @brief Runtime-only interval state allocated for configured primary channels, never saved.
     */
    struct PeriodChannel {
        SignedEnergy pending{}, prepared{};
        bool fault{};
    };

    std::unique_ptr<PeriodChannel[]> periods_;
    std::size_t period_count_{};
    std::uint32_t period_identity_generation_{}, period_generation_{}, period_token_{};
    bool period_allocation_fault_{};
    /** @brief Reconciles the interval allocation with primary physical identity. */
    void synchronize_period();
    /** @brief Captures confirmed advances before Calendar consumes the observer. */
    void observe_period(std::size_t channel, const DirectionalEnergy* delta);
    /** @brief Appends the captured phase Wh preview, not a mutable lifetime/daily difference. */
    bool write_period(CommandOutput& output) const;
#endif
#if USE_ENERGY_BALANCED_DAILY
    bool balanced_rollover_armed_{}; /**< Once armed, remains true across monotonic tick wrap. */
#endif
#if USE_ENERGY_PRECISION
#if USE_MODBUS_USER_PRECISION
    std::uint8_t (*modbus_ambient_precision_)(void*, std::uint8_t){};
    void* modbus_ambient_context_{};
#endif
    OutputSettings (*output_precision_)(void*){};
    void* output_precision_context_{};
#endif
    bool (*no_common_presentation_)(void*){};
    void* no_common_context_{};
    bool (*phase_presentation_)(void*){};
    void* phase_context_{};
    bool (*daily_policy_)(void*){};
    void* daily_policy_context_{};
    bool no_export_today_{};
    CivilClock calendar_clock_{};
    void* calendar_context_{};
    std::uint32_t calendar_local_{};
    bool calendar_valid_{}, calendar_daylight_{};
    /** @brief Applies a complete channel coefficient vector atomically with storage rollback. */
    bool apply_channel_calibrations(const Calibration* values, std::size_t count);

    struct Channel {
        Sample sample{};
        Account account{};
        LiveDaily live_daily{};
        /** Retained source Yesterday projection, independent of dated Calendar ownership. */
        SignedEnergy yesterday{};
        bool yesterday_present{};
        std::unique_ptr<CalendarData> calendar;
        bool calendar_fault{};
        std::unique_ptr<TariffData> tariff;
        bool tariff_fault{};
        Calibration calibration{};
#if USE_ENERGY_COUNTER_FILTER
        bool source_total_started{};  /**< Volatile source nonzero start, never a lifetime seed. */
        bool source_total_rejected{}; /**< Bounds repeated-buffer diagnostic logging. */
#endif
        std::array<std::uint16_t, 2> max_energy{};         /**< Retained Wh threshold/start hour. */
        std::array<std::uint16_t, 3> max_power{0, 10, 30}; /**< Retained threshold/hold/window. */
        std::uint16_t power_delta{};            /**< Retained even when reporting is excluded. */
        std::array<std::uint16_t, 6> margins{}; /**< P-low/high, V-low/high, I-low/high. */
    };

    struct Provider {
        Configuration configuration{};
        TariffSchedule tariff_schedule{};
        std::uint32_t live_daily_day{};
        bool live_daily_day_known{}, live_daily_collected{};
#if USE_ENERGY_LIVE_DAILY
        std::uint32_t live_daily_midnight{};
        bool live_daily_midnight_seen{};
#endif
#if USE_ENERGY_BALANCED_DAILY
        std::unique_ptr<BalancedDaily> balanced;
        bool balanced_fault{};
#endif
        std::unique_ptr<TariffUsage> tariff_usage;
        bool tariff_usage_fault{};
        bool export_reporting{},
            export_reporting_initialized{}; /**< Runtime-only, model-default state. */
        std::uint32_t total_time{}; /**< Meter-wide Total start epoch, shared by all channels. */
        std::unique_ptr<Device> device;
        std::unique_ptr<Channel[]> channels;
        std::unique_ptr<Sample[]> samples;
        std::uint32_t sampled_at{};
        std::uint32_t generation{};
    };

#if USE_ENERGY_LIVE_DAILY
    bool live_daily_rollover_armed_{};
    /** @brief Initializes only a missing projection from retained dated daily state, never
     * lifetime. */
    void initialize_live_daily(Provider& instance);
    /** @brief Applies confirmed update groups before trusted rollover; invalid RTC still accrues.
     */
    void observe_live_daily(
        Provider& instance, const DirectionalEnergy* deltas, const std::uint8_t* groups);
#endif
#if USE_ENERGY_CALIBRATION_LEARNING
    /** @brief Retains already-applied common device calibration through ordinary checkpoints. */
    void capture_calibration(Provider& instance);
#endif
#if USE_CSE7766
    /** @brief Applies startup zero normalization and supplies the live primary threshold. */
    void apply_primary_invalid_power();
    std::uint8_t (*primary_invalid_read_)(void*){};
    bool (*primary_invalid_write_)(void*, std::uint8_t){};
    void* primary_invalid_context_{};
    bool primary_invalid_initialized_{};
    std::uint32_t primary_invalid_retry_at_{};
#endif
#if USE_ENERGY_COUNTER_FILTER
    /** @brief Checks an opted-in primary import update without mutating physical accounting.
     * @param instance Meter owning the sample and Tasmota Total projection.
     * @param channel Corresponding source-start state and exact account.
     * @param sample Current physical reading; independent export remains outside this gate.
     * @return true for accepted updates or disabled/non-applicable filtering.
     */
    bool accept_counter_import(const Provider& instance, Channel& channel, const Sample& sample);
    std::uint8_t (*counter_filter_read_)(void*){};
    void* counter_filter_context_{};
#endif
#if USE_ENERGY_HARDWARE_TOTAL
    /** @brief Reconciles accepted fresh primary import references without measured deltas.
     * @param instance Selected provider and its native channels.
     * @param channel Current exact physical account and compatibility adjustment.
     * @param sample Current driver observation, not a cached lifetime presentation.
     * @param accepted Import passed the existing source plausibility/validation path.
     */
    void reconcile_hardware_total(
        const Provider& instance, Channel& channel, const Sample& sample, bool accepted);
    bool (*hardware_total_enabled_)(void*){};
    void* hardware_total_context_{};
#endif
#if USE_ENERGY_OVERTEMP
    bool (*overtemperature_read_)(void*, std::uint32_t, std::int32_t&, std::uint8_t&){};
    bool (*overtemperature_off_)(void*){};
    void* overtemperature_context_{};
    std::uint32_t overtemperature_second_{};
#endif
    Provider provider_{};
    std::unique_ptr<char[]> modbus_profile_{};
#if USE_ENERGY_POWER_POLICY || USE_ENERGY_MARGINS || USE_BERRY_ENERGY_API
    bool (*primary_power_)(void*){};
    void* primary_power_context_{};
#endif
#if USE_ENERGY_DUMMY
    std::uint32_t (*primary_power_mask_)(void*){};
    void* primary_power_mask_context_{};
#endif
#if USE_ENERGY_POWER_POLICY
    /** @brief Supplies the logical Power state before polling. */
    void apply_primary_power();
#endif
#if USE_ENERGY_MARGINS
    /** @brief Updates only known/available phase quantities; zero thresholds retain flags. */
    std::uint8_t margin_changes(std::size_t channel);
    /** @brief Appends all transitions without emitting partial or truncated event objects. */
    bool write_margin_events(CommandOutput&, const std::uint8_t* changes, bool& first) const;
#endif
#if USE_ENERGY_POWER_DELTA
    struct DeltaPhase {
        std::array<std::uint16_t, 3> history{};
        std::int16_t pending_difference{};
#if USE_ENERGY_MARGINS
        std::uint8_t margin_flags{}, pending_margins{};
#endif
    };

    std::unique_ptr<DeltaPhase[]> delta_phases_;
    std::size_t delta_count_{};
    std::uint32_t delta_identity_{}, delta_at_{}, delta_revision_{}, delta_retry_at_{};
    std::uint8_t delta_steady_{}, delta_stable_{};
    bool delta_started_{}, delta_pending_event_{}, delta_queue_failed_{};
    std::size_t delta_page_cursor_{};
    bool (*delta_event_)(void*, std::string_view, std::string_view){};
    void (*delta_sensor_)(void*){};
    std::uint32_t (*delta_power_revision_)(void*){};
    void* delta_context_{};
    /** @brief Samples once per observed second without replaying missed history slots. */
    void poll_power_delta(std::uint32_t now);
    /** @brief Copies at most one bounded page; rejected pages retain latest pending state. */
    void flush_power_delta();
#endif
    std::uint32_t now_{}, checkpoint_{}, generation_{};
    bool dirty_{}, storage_fault_{};
};

/** @brief Returns the application energy owner. */
Service& service();
/** @brief Dispatches the meter-wide EnergyUsage and EnergyExport commands. */
CommandStatus dispatch_tariff_usage(const CommandRequest&, CommandOutput&);
/** @brief Dispatches optional export-active compatibility controls. */
CommandStatus dispatch_export(const CommandRequest&, CommandOutput&);
/** @brief Dispatches optional primary total commands. */
CommandStatus dispatch_total(const CommandRequest&, CommandOutput&);
/** @brief Dispatches optional primary daily commands. */
CommandStatus dispatch_daily(const CommandRequest&, CommandOutput&);
/** @brief Dispatches explicit instance configuration, virtual input and energy queries. */
CommandStatus dispatch(const CommandRequest&, CommandOutput&);
} // namespace energy
} // namespace newmota
