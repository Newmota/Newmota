/*
  energy_types.h - Portable energy values and persistent resource identities

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_types.h
 * @brief Portable energy values and persistent resource identities.
 */

#pragma once
#include "newmota/core/hardware/pin.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace newmota::energy {
/** @brief Exact nonnegative energy, normalized to whole Wh and 0..999999 micro-Wh. */
struct EnergyValue {
    std::uint64_t wh{};
    std::uint32_t micro_wh{};

    /** @brief Compares the normalized whole and fractional energy components.
     * @param other Value to compare.
     * @return true when both components match.
     */
    bool operator==(const EnergyValue& other) const {
        return wh == other.wh && micro_wh == other.micro_wh;
    }

    /** @brief Reports whether either normalized energy component differs.
     * @param other Value to compare.
     * @return true when the values differ.
     */
    bool operator!=(const EnergyValue& other) const { return !(*this == other); }

    /** @brief Adds without overflow; failure leaves this value unchanged.
     * @param delta Nonnegative normalized energy to add.
     * @return true when the addition succeeds.
     */
    bool add(EnergyValue delta);
};

/** @brief Exact confirmed energy advances, independent of lifetime baseline/seed corrections. */
struct DirectionalEnergy {
    EnergyValue imported{}, exported{};
    /** @brief Adds one direction atomically; no signed or micro-Wh range conversion. */
    bool add(EnergyValue value, bool exporting);
};

/** @brief Exact signed energy using a normalized nonnegative magnitude, without int64 narrowing. */
struct SignedEnergy {
    EnergyValue magnitude{};
    bool negative{};
    /** @brief Adds one signed amount atomically and normalizes negative zero.
     * @param delta Nonnegative normalized magnitude.
     * @param subtract Whether delta is negative.
     * @return false on magnitude overflow; this value remains unchanged.
     */
    bool add(EnergyValue delta, bool subtract = false);
};

/** @brief Source-like signed daily projection, separate from dated history and physical totals.
 * Value contains released0.01Wh quanta plus offset; offset retains the seed/restored checkpoint
 * separately so rollover preserves measured sign. Carry retains confirmed sub-quanta.
 * Fields remain present when collection is excluded so identity-keyed checkpoints survive.
 */
struct LiveDaily {
    SignedEnergy value{}, carry{}, offset{};
    bool present{}, fault{}, incomplete{};
};

/** @brief One dated bucket; started_local exposes a partially observed first day. */
struct CalendarDay {
    std::uint32_t day{}, started_local{};
    DirectionalEnergy energy{};
    bool present{};
    SignedEnergy
        compatibility{}; /**< Prospective signed daily value, separate from observed directions. */
    bool seeded{};       /**< Explicit compatibility seed; directional energy remains unchanged. */
    std::uint32_t
        seed_time{}; /**< Optional source-compatible timestamp metadata, not a date selector. */
    bool offset_nonzero{}; /**< Transient Today-offset state; rebuilt from saved Today at boot. */
};

/** @brief Optional per-channel calendar state, retained even when collection is not selected. */
struct CalendarData {
    std::array<CalendarDay, 2> days{};
    DirectionalEnergy unassigned{};
    DirectionalEnergy incoming{}; /**< Transient confirmed deltas; never serialized. */
    /** @brief Rotates only forwards; invalid/older civil dates retain deltas as unassigned.
     * @param local_seconds Local Unix seconds from the configured civil-time owner.
     * @param valid Whether the clock has a trustworthy civil date.
     * @param no_export Excludes only new export deltas from signed daily compatibility.
     * @return false on exact accumulator exhaustion; previous state remains unchanged.
     */
    bool observe(std::uint32_t local_seconds, bool valid, bool no_export = false);
};

/** @brief Runtime source-balanced daily counters with exact per-phase fractional carries. */
struct BalancedDaily {
    CalendarData calendar{};
    DirectionalEnergy runtime{}; /**< Source sums accrue even without a trusted civil clock. */
    std::uint32_t observed_day{}, midnight_day{};
    bool day_known{}, midnight_seen{};
    std::unique_ptr<SignedEnergy[]> carries;
    std::size_t count{};
    /** @brief Allocates only the configured phase carries.
     * @param channels Nonzero configured phase count, bounded by the service channel limit.
     */
    explicit BalancedDaily(std::size_t channels);
    /** @brief Balances one complete poll vector, never combining releases across poll calls.
     * @param deltas Confirmed physical advances for every configured channel.
     * @param groups Contiguous channel runs identify each driver accounting-update boundary.
     * @param channels Exact configured vector length.
     * @param local Local civil seconds; unknown dates remain explicitly unassigned.
     * @param valid Whether the civil date is trustworthy.
     * @param allow_rollover Core boot-grace latch for source uptime greater than ten seconds.
     * @return false on allocation, shape or exact range exhaustion; state remains unchanged.
     */
    bool observe(const DirectionalEnergy* deltas, const std::uint8_t* groups, std::size_t channels,
        std::uint32_t local, bool valid, bool allow_rollover);
};

/** @brief Per-meter Tasmota-compatible two-boundary schedule, in local minutes. */
struct TariffSchedule {
    std::array<std::array<std::uint16_t, 2>, 2> minutes{}; /**< [tariff][STD/DST]. */
    std::uint8_t forced{}; /**< 0 automatic, 1 off-peak, 2 standard. */
    bool weekend{};
    /** @brief Selects the source-compatible tariff without mutating any energy buckets.
     * @param local_seconds Local Unix seconds; used only with a valid clock.
     * @param valid Whether civil time can select an automatic interval.
     * @param daylight Whether the automatic daylight rule is active.
     * @return 1/2, or zero when automatic selection has no valid clock.
     */
    unsigned active(std::uint32_t local_seconds, bool valid, bool daylight) const;
};

/** @brief Exact lifetime directional tariff advances, independent of dated daily buckets. */
struct TariffData {
    std::array<DirectionalEnergy, 2> totals{};
    DirectionalEnergy unassigned{};
    /** @brief Adds both confirmed directions atomically without deriving a lifetime difference.
     * @param delta Confirmed advances from the shared account observer.
     * @param tariff 1/2 for a selected tariff, zero for unassigned time.
     * @return false on invalid index or exact accumulator exhaustion; totals remain unchanged.
     */
    bool accrue(const DirectionalEnergy& delta, unsigned tariff);
};

/** @brief Source-compatible meter-wide signed tariff projections, separate from physical tariffs.
 */
struct TariffUsage {
    std::array<SignedEnergy, 2> usage{}, returned{};
    EnergyValue pending_export{}; /**< Confirmed export awaiting runtime reporting availability. */
    std::uint32_t started_local{};
    bool incomplete{}; /**< Missing automatic time or an omitted collector prevents complete
                          attribution. */
    /** @brief Atomically applies confirmed phase deltas and releases real deferred export.
     * @param delta Per-channel confirmed advances; never lifetime seed/floor changes.
     * @param count Number of configured channels, bounded to 32.
     * @param tariff 1/2, or zero for unknown automatic tariff (no dated assignment).
     * @param no_export Prospective SO162 usage policy.
     * @param reporting Runtime local or native phase-zero export availability.
     * @return false on range exhaustion; all projections/pending state remain unchanged.
     */
    bool observe(const DirectionalEnergy* delta, std::size_t count, unsigned tariff, bool no_export,
        bool reporting);
};

/** @brief Typed energy resources; identifiers are stored, so renumbering needs a new record format.
 */
enum class PinFunction : std::uint8_t {
    none,
    uart_rx,
    uart_tx,
    cf,
    cf1,
    selector,
    interrupt,
    reset,
    spi_cs,
    spi_miso,
    spi_mosi,
    spi_clock,
    analog,
    tx_enable,
    spi_cs2,
    interrupt2,
    adc_voltage1,
    adc_voltage2,
    adc_voltage3,
    adc_voltage4,
    adc_voltage5,
    adc_voltage6,
    adc_voltage7,
    adc_current1,
    adc_current2,
    adc_current3,
    adc_current4,
    adc_current5,
    adc_current6,
    adc_current7
};

/** @brief Associates one typed driver role with its physical pad. */
struct PinBinding {
    PinFunction function{PinFunction::none};
    PinId pin{};
};

/** @brief A bounded physical identity, independent of calibration and operating options. */
struct PhysicalIdentity {
    static constexpr std::size_t kMaximumPins = 8;
    std::uint16_t driver{};
    std::uint8_t bus{}, address{};
    std::array<PinBinding, kMaximumPins> pins{};
    /** @brief Finds a configured role or returns an invalid pin.
     * @param function Resource role to look up.
     * @return Bound pad, or an invalid PinId when the role is absent.
     */
    PinId pin(PinFunction function) const;
    /** @brief Adds, replaces, or removes one role while rejecting duplicate physical pads.
     * @param function Role to update; none is not a valid target.
     * @param pin Pad to bind, or an invalid PinId to remove the role.
     * @return true when the resulting identity is valid and was applied.
     */
    bool bind(PinFunction function, PinId pin);
    /** @brief Rejects malformed roles, duplicate roles, and duplicate physical pads.
     * @return true when all retained bindings form a valid identity.
     */
    bool valid() const;
    /** @brief Compares physical resources without depending on binding-array order.
     * @param other Identity to compare.
     * @return true when driver, bus, address, and role-to-pad assignments match.
     */
    bool operator==(const PhysicalIdentity& other) const;
    /** @brief Reports whether this identity describes any physical resource.
     * @return true when at least one pin, bus or nonzero address is present.
     */
    bool has_resources() const;
};

/** @brief Measurement field selected for a calibration operation. */
enum class CalibrationField : std::uint8_t { power, voltage, current, frequency };

/** @brief Driver coefficients; zero selects the driver or chip default.
 * Frequency uses the device's native reference coefficient when supported.
 */
struct Calibration {
    std::uint32_t power{}, voltage{}, current{}, frequency{};
};

/** @brief Operating choices which do not replace the physical accounting identity. */
struct Options {
    std::uint8_t flags{};
    std::uint16_t invalid_power_frames{128};
};

/** @brief Persisted resources plus separately mutable channel policy, options and calibration. */
struct Configuration {
    static constexpr std::size_t kMaximumModbusProfile = 511;
    PhysicalIdentity identity{};
    Calibration calibration{};
    Options options{};
    std::uint8_t channels{};
    /** @brief Feature-retained bounded profile: Modbus registers or ADC per-pin endpoints. */
    std::string_view modbus_profile{};
};

/** @brief Returns a stable command spelling for a resource role. */
const char* pin_function_name(PinFunction function);
/** @brief Parses one role spelling, returning none for an unknown role. */
PinFunction parse_pin_function(const char* text, std::size_t size);

/** @brief Wide independent import/export totals in micro-Wh with fractional integration carry. */
struct Account {
    EnergyValue imported{}, exported{}, import_baseline{}, export_baseline{};
    SignedEnergy
        total_adjustment{}; /**< Tasmota net-Total projection offset, never a measured delta. */
    bool total_fault{};
    SignedEnergy
        export_adjustment{}; /**< Local export-active projection seed, not a physical counter. */
    /** @brief Derives local ExportActive from physical export plus its explicit seed offset.
     * @param output Receives exact signed energy without changing any accounting state.
     * @return false when the projection exceeds the exact magnitude range.
     */
    bool compatibility_export(SignedEnergy& output) const;
    /** @brief Derives Tasmota's signed Total from the existing directional lifetime accounts and
     * offset.
     * @param output Receives exact signed energy when the projection is representable.
     * @return false on a retained projection fault or magnitude overflow.
     */
    bool compatibility_total(SignedEnergy& output) const;
    std::uint16_t import_remainder{}, export_remainder{};
    std::uint32_t pulse_remainder{}, import_resets{}, export_resets{};
    std::uint8_t baseline_fields{};
    std::uint32_t import_increment_remainder{}, export_increment_remainder{};
    std::uint32_t import_increment_denominator{1}, export_increment_denominator{1};
    /** @brief Adds rational micro-Wh from a confirmed hardware delta with persistent carry.
     * @param confirmed Optional per-poll directional advance; lifetime seeds/floors never enter it.
     */
    bool add_increment(std::uint64_t numerator, std::uint32_t denominator, bool exporting = false,
        DirectionalEnergy* confirmed = nullptr);
    /** @brief Integrates milli-W over milliseconds with fractional micro-Wh carry.
     * @param confirmed Optional confirmed advance after fractional carry produces whole micro-Wh.
     */
    bool integrate(
        std::int64_t power, std::uint32_t elapsed_ms, DirectionalEnergy* confirmed = nullptr);
    /** @brief Adds counted energy with rational scale/36000 micro-Wh per pulse.
     * @param confirmed Optional confirmed imported-energy advance.
     */
    bool add_pulses(
        std::uint32_t count, std::uint64_t scale, DirectionalEnergy* confirmed = nullptr);
    /** @brief Observes one exact import/export counter; decreases establish a new baseline.
     * @param exporting Selects the retained counter baseline (import or export).
     * @param export_delta Routes this advance to export without changing its baseline identity.
     * @param confirmed Receives advances only; first baselines, resets and replacements add none.
     */
    bool observe_absolute(EnergyValue value, bool exporting = false, bool export_delta = false,
        DirectionalEnergy* confirmed = nullptr);
};

} // namespace newmota::energy
