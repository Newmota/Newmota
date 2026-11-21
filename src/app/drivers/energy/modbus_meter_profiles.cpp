/*
  modbus_meter_profiles.cpp - Individually selectable RTU energy meter maps

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Register maps and protocol scales in this file are adapted from the following
  Tasmota drivers:
  - xnrg_08_sdm120.ino: Copyright (C) 2021 Gennaro Tortone and Theo Arends
  - xnrg_09_dds2382.ino: Copyright (C) 2021 Matteo Campanella, based on Gennaro Tortone
  - xnrg_10_sdm630.ino: Copyright (C) 2021 Gennaro Tortone and Theo Arends
  - xnrg_11_ddsu666.ino: Copyright (C) 2021 Pablo Zerón and Theo Arends
  - xnrg_13_fif_le01mr.ino: Copyright (C) 2021 Przemyslaw Wistuba
  - xnrg_16_iem3000.ino: Copyright (C) 2022 Marius Bezuidenhout
  - xnrg_17_ornowe517.ino: Copyright (C) 2021 Maxime Vincent, based on Gennaro Tortone
    and Theo Arends
  - xnrg_18_sdm72.ino: Copyright (C) 2021 Gennaro Tortone, Theo Arends and Norbert Richter
  - xnrg_21_sdm230.ino: Copyright (C) 2022 dad401 and Theo Arends

  The corresponding Tasmota sources are distributed under GPL-3.0-or-later.
*/

/** @file modbus_meter_profiles.cpp
 * @brief Register maps and scale rules for independent Modbus energy drivers.
 */

#include "drivers/energy/modbus_meter.h"
#include "newmota_features.h"
#include <iterator>

namespace newmota::energy {
namespace {
using registers::Encoding;

#if USE_SDM630 || USE_ORNO_WE517
/** @brief These two pinned sources renew all phase ages on each successful register reply. */
constexpr registers::Profile all_phase_observations(registers::Profile profile) {
#if USE_BERRY_ENERGY_API
    profile.observation_scope = registers::ObservationScope::all_phases;
#endif
    return profile;
}
#endif
using registers::Field;
using registers::Profile;
using registers::Register;
constexpr std::uint8_t common = 255;

/** @brief Defines one register address, decoded field, and canonical scale conversion.
 * @param address Starting register address.
 * @param field Destination measurement.
 * @param channel Zero-based meter channel or common for a shared field.
 * @param encoding Wire representation of the register value.
 * @param numerator Scale numerator applied after decoding.
 * @param denominator Scale denominator applied after decoding.
 * @param required_flags Profile flags that must be set for this register.
 * @param forbidden_flags Profile flags that must be clear for this register.
 * @param optional_extension Whether a failed read may be skipped.
 * @return Immutable register mapping.
 */
constexpr Register map(std::uint16_t address, Field field, std::uint8_t channel,
    Encoding encoding = Encoding::float32, std::int32_t numerator = 1000,
    std::uint32_t denominator = 1, std::uint8_t required_flags = 0,
    std::uint8_t forbidden_flags = 0, bool optional_extension = false) {
    return {address, field, channel, encoding, numerator, denominator, required_flags,
        forbidden_flags, optional_extension};
}

#if USE_SDM120
/** @brief SDM120 map with optional separate counters on newer compatible variants. */
constexpr Register sdm120_map[]{
    map(0x0000, Field::voltage, 0),
    map(0x0006, Field::current, 0),
    map(0x000C, Field::power, 0),
    map(0x0012, Field::apparent, 0),
    map(0x0018, Field::reactive, 0),
    map(0x001E, Field::factor, 0),
    map(0x0046, Field::frequency, 0),
    map(0x0156, Field::total_active, 0),
    map(0x0048, Field::import_counter, 0, Encoding::float32, 1000, 1, 0, 0, true),
    map(0x004A, Field::export_counter, 0, Encoding::float32, 1000, 1, 0, 0, true),
    map(0x004C, Field::import_reactive, 0, Encoding::float32, 1000, 1, 0, 0, true),
    map(0x004E, Field::export_reactive, 0, Encoding::float32, 1000, 1, 0, 0, true),
    map(0x0024, Field::phase_angle, 0, Encoding::float32, 1000, 1, 0, 0, true),
};
constexpr Profile sdm120_profile{sdm120_map, std::size(sdm120_map), 1, 4, 0,
    platform::serial::Parity::none, 250, 0, 0, 0, true, true, false, 2400};
#endif

#if USE_DDS2382
/** @brief DDS2382 map; SetOption71 selects counter offsets within its 18-register reply. */
constexpr Register dds2382_map[]{
    map(4, Field::export_counter, 0, Encoding::unsigned32, 10, 1, 0, 1),
    map(6, Field::import_counter, 0, Encoding::unsigned32, 10, 1, 0, 1),
    map(8, Field::export_counter, 0, Encoding::unsigned32, 10, 1, 1, 0),
    map(10, Field::import_counter, 0, Encoding::unsigned32, 10, 1, 1, 0),
    map(12, Field::voltage, 0, Encoding::unsigned16, 100, 1),
    map(13, Field::current, 0, Encoding::unsigned16, 10, 1),
    map(14, Field::power, 0, Encoding::unsigned16, 1000, 1),
    map(15, Field::reactive, 0, Encoding::signed16, 1000, 1),
    map(16, Field::factor, 0, Encoding::unsigned16, 1, 1),
    map(17, Field::frequency, 0, Encoding::unsigned16, 10, 1),
};
constexpr Profile dds2382_profile{dds2382_map, std::size(dds2382_map), 1, 3, 1,
    platform::serial::Parity::none, 1000, 4000, 0, 18, false, false, false, 9600};
#endif

#if USE_SDM630
/** @brief Three-phase SDM630 map with optional apparent power and per-phase energy counters. */
constexpr Register sdm630_map[]{
    map(0x0000, Field::voltage, 0),
    map(0x0002, Field::voltage, 1),
    map(0x0004, Field::voltage, 2),
    map(0x0006, Field::current, 0),
    map(0x0008, Field::current, 1),
    map(0x000A, Field::current, 2),
    map(0x000C, Field::power, 0),
    map(0x000E, Field::power, 1),
    map(0x0010, Field::power, 2),
    map(0x0012, Field::apparent, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0014, Field::apparent, 1, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0016, Field::apparent, 2, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0018, Field::reactive, 0),
    map(0x001A, Field::reactive, 1),
    map(0x001C, Field::reactive, 2),
    map(0x001E, Field::factor, 0),
    map(0x0020, Field::factor, 1),
    map(0x0022, Field::factor, 2),
    map(0x0046, Field::frequency, 0),
    map(0x0156, Field::total_active, 0),
    map(0x0158, Field::total_reactive, 0),
    map(0x015A, Field::import_counter, 0),
    map(0x015C, Field::import_counter, 1),
    map(0x015E, Field::import_counter, 2),
    map(0x0160, Field::export_counter, 0),
    map(0x0162, Field::export_counter, 1),
    map(0x0164, Field::export_counter, 2),
};
constexpr Profile sdm630_profile = all_phase_observations({sdm630_map, std::size(sdm630_map), 3, 4,
    1, platform::serial::Parity::none, 250, 0, 0, 0, false, false, true, 9600});
#endif

#if USE_DDSU666
/** @brief DDSU666 map for its 0x2000 measurement and 0x4000 energy register groups. */
constexpr Register ddsu666_map[]{
    map(0x2000, Field::voltage, 0),
    map(0x2002, Field::current, 0),
    map(0x2004, Field::power, 0, Encoding::float32, 1000000),
    map(0x2006, Field::reactive, 0, Encoding::float32, 1000000),
    map(0x200A, Field::factor, 0),
    map(0x200E, Field::frequency, 0),
    map(0x4000, Field::import_counter, 0),
    map(0x400A, Field::export_counter, 0),
};
constexpr Profile ddsu666_profile{ddsu666_map, std::size(ddsu666_map), 1, 4, 0,
    platform::serial::Parity::none, 250, 0, 0, 0, false, false, false, 9600};
#endif

#if USE_FIF_LE01MR
/** @brief LE01MR map for its non-contiguous 16-bit and 32-bit register fields. */
constexpr Register le01mr_map[]{
    map(0x0130, Field::frequency, 0, Encoding::unsigned16, 10),
    map(0x0131, Field::voltage, 0, Encoding::unsigned16, 10),
    map(0x0158, Field::factor, 0, Encoding::signed16, 1),
    map(0x0139, Field::current, 0, Encoding::unsigned32, 1),
    map(0x0140, Field::power, 0, Encoding::unsigned32, 1000),
    map(0x0148, Field::reactive, 0, Encoding::unsigned32, 1000),
    map(0x0150, Field::apparent, 0, Encoding::unsigned32, 1000),
    map(0xA000, Field::import_counter, 0, Encoding::unsigned32, 10),
    map(0xA01E, Field::total_reactive, 0, Encoding::unsigned32, 10),
};
constexpr Profile le01mr_profile{le01mr_map, std::size(le01mr_map), 1, 3, 0,
    platform::serial::Parity::none, 250, 0, 0, 0, false, false, false, 2400};
#endif

#if USE_IEM3000
/** @brief IEM3000 map with 64-bit signed energy counters and three-phase measurements. */
constexpr Register iem3000_map[]{
    map(0x0BB7, Field::current, 0),
    map(0x0BB9, Field::current, 1),
    map(0x0BBB, Field::current, 2),
    map(0x0BD3, Field::voltage, 0),
    map(0x0BD5, Field::voltage, 1),
    map(0x0BD7, Field::voltage, 2),
    map(0x0BED, Field::power, 0, Encoding::float32, 1000000),
    map(0x0BEF, Field::power, 1, Encoding::float32, 1000000),
    map(0x0BF1, Field::power, 2, Encoding::float32, 1000000),
    map(0x0C25, Field::frequency, common),
    map(0x0DBD, Field::import_counter, 0, Encoding::signed64, 1),
    map(0x0DC1, Field::import_counter, 1, Encoding::signed64, 1),
    map(0x0DC5, Field::import_counter, 2, Encoding::signed64, 1),
    map(0x0C83, Field::total_active, 0, Encoding::signed64, 1),
};
constexpr Profile iem3000_profile{iem3000_map, std::size(iem3000_map), 3, 3, 0,
    platform::serial::Parity::none, 250, 0, 0, 0, false, false, false, 19200};
#endif

#if USE_ORNO_WE517
/** @brief Three-phase ORNO WE517 map with common frequency and active-energy total. */
constexpr Register we517_map[]{
    map(0x000E, Field::voltage, 0),
    map(0x0010, Field::voltage, 1),
    map(0x0012, Field::voltage, 2),
    map(0x0016, Field::current, 0),
    map(0x0018, Field::current, 1),
    map(0x001A, Field::current, 2),
    map(0x001E, Field::power, 0, Encoding::float32, 1000000),
    map(0x0020, Field::power, 1, Encoding::float32, 1000000),
    map(0x0022, Field::power, 2, Encoding::float32, 1000000),
    map(0x0026, Field::reactive, 0),
    map(0x0028, Field::reactive, 1),
    map(0x002A, Field::reactive, 2),
    map(0x0036, Field::factor, 0),
    map(0x0038, Field::factor, 1),
    map(0x003A, Field::factor, 2),
    map(0x0014, Field::frequency, common),
    map(0x0100, Field::total_active, 0),
};
constexpr Profile we517_profile = all_phase_observations({we517_map, std::size(we517_map), 3, 3, 0,
    platform::serial::Parity::even, 250, 0, 0, 0, true, false, false, 9600});
#endif

#if USE_SDM72
/** @brief Three-phase SDM72 map with optional import/export counters and extensions. */
constexpr Register sdm72_map[]{
    map(0x0034, Field::power, 0),
    map(0x0156, Field::total_active, 0),
    map(0x0500, Field::import_power, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0502, Field::export_power, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0048, Field::import_counter, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x004A, Field::export_counter, 0, Encoding::float32, 1000, 1, 1, 0, true),
};
constexpr Profile sdm72_profile{sdm72_map, std::size(sdm72_map), 1, 4, 1,
    platform::serial::Parity::none, 250, 0, 0, 0, true, false, false, 9600};
#endif

#if USE_SDM230
/** @brief SDM230 map with optional device-specific power and resettable-energy fields. */
constexpr Register sdm230_map[]{
    map(0x0000, Field::voltage, 0),
    map(0x0006, Field::current, 0),
    map(0x000C, Field::power, 0),
    map(0x0012, Field::apparent, 0),
    map(0x0018, Field::reactive, 0),
    map(0x001E, Field::factor, 0),
    map(0x0046, Field::frequency, 0),
    map(0x0048, Field::import_counter, 0),
    map(0x004A, Field::export_counter, 0),
    map(0x0024, Field::phase_angle, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0056, Field::maximum_power, 0, Encoding::float32, 1000, 1, 1, 0, true),
    map(0x0180, Field::resettable_active, 0, Encoding::float32, 1000, 1, 1, 0, true),
};
constexpr Profile sdm230_profile{sdm230_map, std::size(sdm230_map), 1, 4, 1,
    platform::serial::Parity::none, 250, 0, 0, 0, false, false, false, 9600};
#endif
} // namespace

#if USE_SDM120
/** @brief Creates an SDM120-compatible meter from its selected static register profile.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_sdm120(const Configuration& c) {
    return registers::create(c, sdm120_profile);
}
#endif
#if USE_DDS2382
/** @brief Creates a DDS2382 meter with the selected V1/V2 register map.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_dds2382(const Configuration& c) {
    return registers::create(c, dds2382_profile);
}
#endif
#if USE_SDM630
/** @brief Creates a three-phase SDM630 meter.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_sdm630(const Configuration& c) {
    return registers::create(c, sdm630_profile);
}
#endif
#if USE_DDSU666
/** @brief Creates a DDSU666 meter.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_ddsu666(const Configuration& c) {
    return registers::create(c, ddsu666_profile);
}
#endif
#if USE_FIF_LE01MR
/** @brief Creates a FIF LE-01MR meter.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_le01mr(const Configuration& c) {
    return registers::create(c, le01mr_profile);
}
#endif
#if USE_IEM3000
/** @brief Creates a Schneider IEM3000 meter with native 64-bit energy counters.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_iem3000(const Configuration& c) {
    return registers::create(c, iem3000_profile);
}
#endif
#if USE_ORNO_WE517
/** @brief Creates a three-phase ORNO WE517 meter.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_we517(const Configuration& c) {
    return registers::create(c, we517_profile);
}
#endif
#if USE_SDM72
/** @brief Creates a three-phase SDM72 meter.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_sdm72(const Configuration& c) {
    return registers::create(c, sdm72_profile);
}
#endif
#if USE_SDM230
/** @brief Creates an SDM230 meter with supported optional device fields.
 * @param c Persisted meter identity, channel count, and operating settings.
 * @return Meter owner, or nullptr when the configuration or RTU bus is incompatible.
 */
std::unique_ptr<Device> create_sdm230(const Configuration& c) {
    return registers::create(c, sdm230_profile);
}
#endif
} // namespace newmota::energy
