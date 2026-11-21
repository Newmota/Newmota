/*
  led_driver.h - two-wire LED driver chips below the Light service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file led_driver.h
 * @brief Two-wire LED driver chips of Tuya bulbs, driven as five Light channels.
 */

#pragma once
#include "core/hardware/hardware.h"
#include "newmota/core/hardware/pin.h"
#include <array>
#include <cstdint>

namespace newmota::led_driver {

/** @brief Chip selected by the template's CLK and DAT functions. */
enum class Chip : std::uint8_t {
    none,
    bp5758d,
    bp1658cj,
    sm2335,
    sm2135,
};

/** @brief Outcome of looking for a chip in the template. */
enum class OpenResult : std::uint8_t {
    none,    /**< No chip has both pins assigned. */
    chip,    /**< Exactly one chip is assigned. */
    several, /**< More than one chip is assigned; none is used. */
};

/** @brief Current setting meaning "use the chip or template default". */
inline constexpr std::uint8_t kDefaultCurrent = 0xFFU;

/** @brief Scales a 16-bit electrical value to the chips' 10 bits. */
[[nodiscard]] constexpr std::uint16_t ten_bits(const std::uint16_t value) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(value) * 1023U + 32767U) / 65535U);
}

/** @brief Open-drain two-wire bus: a released pad is high through the internal pull-up. */
class Bus final {
  public:
    PinId clock;
    PinId data;
    std::uint32_t half_period_us{2U};
    bool ok{true}; /**< Cleared by the first failed pad change of a frame. */

    /** @brief Releases both pads. */
    void idle();
    /** @brief Sends a start condition followed by @p command. */
    void start(std::uint8_t command);
    /** @brief Sends one byte MSB first and clocks the ACK bit without checking it. */
    void write(std::uint8_t value);
    /** @brief Sends a stop condition. */
    void stop();

  private:
    void level(PinId pin, bool high);
};

/** @brief The configured chip, its bus and the state of its last frame. */
class Driver final {
  public:
    /** @brief Finds the chip assigned by the template and its DAT index. */
    [[nodiscard]] OpenResult open(const Hardware& hardware);

    /** @brief Configures the chip.
     * @param channels Light channel (0..4, RGBCW) carried by each frame position.
     * @param currents Configured RGB and white currents, kDefaultCurrent for the default.
     */
    [[nodiscard]] bool begin(
        const std::array<std::uint8_t, 5>& channels, const std::array<std::uint8_t, 2>& currents);

    /** @brief Sends five 16-bit electrical values in frame position order when they changed.
     * @return false when a pad could not be driven.
     */
    [[nodiscard]] bool write(
        const std::array<std::uint16_t, 5>& values, const std::array<std::uint8_t, 2>& currents);

    /** @brief Returns the configured chip. */
    [[nodiscard]] Chip chip() const { return chip_; }

    /** @brief Returns the effective RGB and white current in the chip's unit. */
    [[nodiscard]] std::array<std::uint8_t, 2> currents(
        const std::array<std::uint8_t, 2>& configured) const;

    /** @brief Reports whether @p value is a valid RGB or white current in the chip's unit. */
    [[nodiscard]] bool valid_current(std::uint8_t value, bool white) const;

  private:
    Bus bus_{};
    Chip chip_{Chip::none};
    std::uint8_t data_index_{1U}; /**< One-based DAT instance; Tasmota's current preset. */
    std::array<std::uint8_t, 5> channels_{};
    std::array<std::uint8_t, 2> sent_currents_{};
    std::array<std::uint16_t, 5> sent_{};
    bool sent_valid_{};
    bool started_{};
    bool sleeping_{};
};

/** @brief Chip frames, each in its own source behind its feature. */
namespace chips {
/** @brief Sends the BP5758D setup frame with per-output currents. */
void bp5758d_setup(Bus& bus, const std::array<std::uint8_t, 5>& channels,
    const std::array<std::uint8_t, 2>& currents, bool clear);
/** @brief Sends one BP5758D frame and enters or leaves sleep. */
void bp5758d_write(Bus& bus, const std::array<std::uint16_t, 5>& values, bool& sleeping);
/** @brief Sends one BP1658CJ frame, or its standby frame when dark. */
void bp1658cj_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents);
/** @brief Sends one SM2335 frame, or its standby frame when dark. */
void sm2335_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents);
/** @brief Returns the RGB and white current codes of a Tasmota SM2135 preset.
 * @param preset One-based `SM2135 DAT` instance.
 */
[[nodiscard]] std::array<std::uint8_t, 2> sm2135_currents(std::uint8_t preset);
/** @brief Sends the SM2135 frames of a Tasmota preset: RGB, white, or both. */
void sm2135_write(Bus& bus, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents, std::uint8_t preset);
/** @brief Shared shape of BP1658CJ and SM2335: command, current nibbles, five 10-bit values.
 * @param split Sends each value as low 5 bits then high 5 bits instead of high then low byte.
 */
void nibble_current_write(Bus& bus, std::uint8_t command, std::uint8_t standby, bool split,
    const std::array<std::uint16_t, 5>& values, const std::array<std::uint8_t, 2>& currents);
} // namespace chips

} // namespace newmota::led_driver
