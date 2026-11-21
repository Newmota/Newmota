/*
  tuya_mcu.h - TuyaMCU serial driver for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file tuya_mcu.h
 * @brief Tasmota's TuyaMCU v2 driver: the MCU's data points become relays, switches, a Light,
 * sensors, enums and reports.
 */

#pragma once
#include "core/command/command.h"
#include "newmota_features.h"

#if USE_TUYA_MCU_SENSORS
#include "drivers/sensors/sensor_service.h"
#endif

#include <cstdint>
#include <string_view>

namespace newmota {
class Hardware;
#if USE_PWM
class LightService;
#endif
class PowerService;
class RestartController;
class Settings;
#if USE_TIME
class TimeService;
#endif
} // namespace newmota

namespace newmota::tuya {

/** @brief What the MCU's Wi-Fi LED shows, as Tasmota's TuyaGetTuyaWifiState. */
enum class LinkState : std::uint8_t {
    access_point = 1, ///< Configuration access point
    offline = 2,      ///< Neither of the others
    restarting = 3,   ///< Restart pending
    cloud = 4,        ///< MQTT connected
};

/** @brief A message the driver publishes. */
enum class Publication : std::uint8_t {
    received, ///< TuyaReceived on tele RESULT (SetOption66)
    dp_topic, ///< Raw value on stat DpType<t>Id<d> (TuyaSend9)
    result,   ///< A reported enum as its command result on stat RESULT, or @p leaf with SO4
};

/** @brief Publishes one message; @p leaf is the topic leaf, @p payload the complete payload. */
using Publisher = void (*)(
    void* context, Publication kind, std::string_view leaf, std::string_view payload);

/** @brief Selects the driver when Tuya Tx and Tuya Rx are assigned, before Power starts.
 * Adds the mapped relays to @p power and, as Tasmota, maps Relay1 to dpId 1 when no relay and
 * no dummy function (99) is mapped. A mapped light takes the last relays for its power, as
 * Tasmota's Light.device, so those relays are not added.
 * @return true when the MCU port is open.
 */
[[nodiscard]] bool initialize(const Hardware& hardware, Settings& settings, PowerService& power);

/** @brief Reports whether the driver runs. */
[[nodiscard]] bool active();

/** @brief Sets the publication sink; without one, messages are logged at debug level. */
void set_publisher(Publisher publisher, void* context);

#if USE_TIME
/** @brief Sets the clock the MCU gets when it asks for the time. */
void set_time_source(TimeService* time);
#endif

#if USE_PWM
/** @brief Light channels of the mapped MCU light, as Tasmota's light_type from TuyaMCU. */
struct LightLayout final {
    std::uint8_t channels{}; ///< 0 none, 1 dimmer, 2 CT or two dimmers, 3 RGB, 4 RGBW, 5 RGBCW
    bool independent{};      ///< Two dimmers, each its own Light endpoint (SetOption68)
    bool mode{};             ///< RGBW or RGBCW with the mode (26) mapped, as AsModuleTuyaMS
};

/** @brief Returns the MCU light; no channels without the driver or a mapped dimmer. */
[[nodiscard]] LightLayout light_layout();

/** @brief Gives the driver the Light that owns the MCU light, once its endpoints are set. */
void attach_light(LightService& light);

/** @brief Sends the Light's state: the power of its relays, then dimmers, CT, white or colour,
 * each only when it differs from the MCU's last report.
 */
void write_light();
#endif

#if USE_TUYA_MCU_SENSORS
/** @brief Visits the reported TuyaSNS values of the mapped sensor functions (71-84), as
 * Tasmota's TuyaSensorsShow; a function the MCU has not reported yet is left out.
 */
[[nodiscard]] bool visit_sensor_readings(SensorReadingVisitor visitor, void* context);
#endif

#if USE_TUYA_MCU_ENERGY
/** @brief The MCU's energy data points (fnId 31-33, 36, 37) in Newmota units, scaled as
 * Tasmota's XNRG_32; a value the MCU has not reported yet is 0.
 */
struct EnergyReading final {
    std::int64_t power_mw{};
    std::uint64_t total_wh{}; ///< fnId 37, the MCU's import total
    std::int32_t voltage_mv{};
    std::int32_t current_ma{};
    bool voltage{};     ///< Voltage mapped (33 or 36)
    bool current{};     ///< Current mapped (32 or 36)
    bool total{};       ///< Total mapped (37)
    bool total_fresh{}; ///< A total report arrived since the last read
};

/** @brief Reports whether the driver runs with power mapped (31 or 36), as Tasmota's Xnrg32
 * selects the energy driver.
 */
[[nodiscard]] bool energy_mapped();

/** @brief Returns the energy values and clears the fresh total flag. */
[[nodiscard]] EnergyReading read_energy();
#endif

/** @brief Reads the MCU and advances the protocol; call on every main pass. */
void poll(std::uint32_t now, LinkState link);

/** @brief Handles TuyaMCU, TuyaSend<x>, TuyaEnum<x>, TuyaEnumList, TuyaTempSetRes (with
 * USE_TUYA_MCU_SENSORS), TuyaRGB, TuyaDimDelay and DimmerRange (with USE_PWM); unknown
 * without the driver, as in Tasmota.
 * A valid `TuyaMCU` change is saved and restarts the device, as Tasmota does.
 */
[[nodiscard]] CommandStatus dispatch(
    const CommandRequest& request, CommandOutput& output, RestartController& restart);

} // namespace newmota::tuya
