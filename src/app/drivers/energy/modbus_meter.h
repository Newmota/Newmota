/*
  modbus_meter.h - Bounded common register polling and exact meter conversion

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  User-field resolution reference: Tasmota xnrg_29_modbus.ino,
  Copyright (C) 2022 Theo Arends, GPL version 3 or later.
*/

/** @file modbus_meter.h
 * @brief Bounded common register polling and exact meter conversion.
 */

#pragma once
#include "services/energy/energy_service.h"
#include "drivers/modbus/rtu_service.h"
#include <array>

namespace newmota::energy::registers {
inline constexpr std::uint8_t common = 255;
inline constexpr std::size_t maximum_user_fields = 8;
inline constexpr std::size_t maximum_registers = 51;

/** @brief Data type IDs retain the pinned configurable-Modbus meanings; signed64 is added. */
enum class Encoding : std::uint8_t {
    float32 = 0,
    signed16 = 1,
    signed32 = 2,
    unsigned16 = 3,
    unsigned32 = 4,
    signed32_swapped = 6,
    unsigned32_swapped = 8,
    signed64 = 9
};
/** @brief Canonical measurement destination for one configured register. */
enum class Field : std::uint8_t {
    voltage,
    current,
    power,
    apparent,
    reactive,
    factor,
    frequency,
    import_counter,
    export_counter,
    total_active,
    import_reactive,
    export_reactive,
    phase_angle,
    total_reactive,
    import_power,
    export_power,
    maximum_power,
    resettable_active,
    custom,
    ignore,
    count
};

/** @brief One register maps to one typed field. Energy scales are Wh; other scales are milli-units.
 */
struct Register {
    std::uint16_t address;                /**< Starting register address. */
    Field field;                          /**< Canonical reading to update. */
    std::uint8_t channel{};               /**< Zero-based channel, or common for shared fields. */
    Encoding encoding{Encoding::float32}; /**< Wire representation and word ordering. */
    std::int32_t numerator{1000}; /**< Scale numerator before conversion to canonical units. */
    std::uint32_t denominator{1}; /**< Nonzero scale denominator. */
    std::uint8_t required_flags{}, forbidden_flags{}; /**< Profile-option masks for this row. */
    bool optional_extension{};    /**< Ignore a failed read when the field is an extension. */
    std::uint8_t user_index{255}; /**< UserField index for custom readings, else 255. */
    std::int8_t user_exponent{};  /**< Decimal power applied to a custom reading. */
};

/** @brief Tasmota-compatible metadata for one user-defined Modbus register. */
struct UserField {
    std::array<char, 32> json_name{}; /**< Key used in measurement JSON. */
    std::array<char, 32> gui_name{};  /**< User-facing field name. */
    std::array<char, 16> unit{};      /**< Display unit. */
    std::uint8_t decimals{};          /**< Literal 0..20 or unselected selector fallback 3. */
#if USE_MODBUS_USER_PRECISION
    /** @brief Raw D selector 21..29; zero keeps literal/fallback decimals.
     * JSON remains the retained source of truth; this descriptor never stores settings snapshots.
     */
    std::uint8_t resolution_selector{};
#endif
};

#if USE_BERRY_ENERGY_API
/** @brief Source phase-age renewal policy, independent of register projection/freshness. */
enum class ObservationScope : std::uint8_t { phase_zero, all_phases, mapped_phase };
#endif

/** @brief Immutable per-model behavior; it does not allocate native resources. */
struct Profile {
    const Register* registers;    /**< Immutable register map. */
    std::size_t size;             /**< Number of rows in registers. */
    std::uint8_t channels{1};     /**< Number of channels reported by this model. */
    std::uint8_t function{4};     /**< Modbus function used for reads. */
    std::uint8_t allowed_flags{}; /**< Option bits accepted by this model. */
    platform::serial::Parity parity{platform::serial::Parity::none};
    std::uint16_t interval_ms{250};             /**< Minimum delay between polls. */
    std::uint16_t startup_ms{};                 /**< Delay before the first request. */
    std::uint16_t block_start{}, block_words{}; /**< Optional contiguous block read. */
    bool total_is_import{};                     /**< Treat a total-only energy counter as import. */
    bool prefer_import{};         /**< Prefer import over total when both fields are available. */
    bool high_rate{};             /**< Permit the higher-rate scheduler path. */
    std::uint32_t baud{};         /**< Required UART rate, or zero to accept the configured rate. */
    std::uint8_t stop_bits{1};    /**< Required RTU stop-bit count. */
    std::uint8_t address_count{}; /**< Number of pinned meter addresses when nonzero. */
    std::array<std::uint8_t, 3> addresses{}; /**< Pinned unit IDs for multi-channel models. */
    const UserField* user_fields{};          /**< Optional user-defined measurement metadata. */
    std::size_t user_field_count{};          /**< Number of entries in user_fields. */
#if USE_BERRY_ENERGY_API
    /** @brief Exact source data_valid reset scope for each successful expected register reply. */
    ObservationScope observation_scope{ObservationScope::phase_zero};
#endif
};

/** @brief Runtime-owned bounded profile parsed from one configured Modbus map. */
struct ConfiguredProfile {
    Profile profile{};
    std::array<Register, maximum_registers> registers{};
    std::array<UserField, maximum_user_fields> users{};
    std::size_t register_count{}, user_count{};

    /** @brief Rebinds profile views after the owning object has moved. */
    void bind();
};

/** @brief Parses and validates one bounded Tasmota-style generic Modbus JSON profile.
 * @param configuration Retained identity, channel count, and JSON map view.
 * @param output Receives fixed-capacity registers and metadata on success.
 * @return true when the complete profile is valid and fits its fixed limits.
 */
bool parse_configured_profile(const Configuration& configuration, ConfiguredProfile& output);

/** @brief Allocates one bounded protocol instance on an already retained physical bus.
 * @param configuration Persistent device identity and operating settings.
 * @param profile Immutable model register map.
 * @return Protocol owner, or nullptr when validation, allocation, or bus attachment fails.
 */
std::unique_ptr<Device> create(const Configuration& configuration, const Profile& profile);
/** @brief Parses and creates one bounded instance from a retained JSON map.
 * @param configuration Persistent device identity, channel count, and map view.
 * @return Protocol owner, or nullptr when parsing, allocation, or bus attachment fails.
 */
std::unique_ptr<Device> create_configured(const Configuration& configuration);
} // namespace newmota::energy::registers
