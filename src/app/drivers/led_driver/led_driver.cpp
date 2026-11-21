/*
  led_driver.cpp - two-wire LED driver chips below the Light service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Bus timing reference: Tasmota xlgt_08_bp5758d.ino, Copyright (C) 2022 Theo Arends and
  oomg, and OpenBeken drv_soft_i2c.c, GPL version 3 or later.
*/

/** @file led_driver.cpp
 * @brief Chip selection, the shared open-drain bus and change-only frame sending.
 */

#include "drivers/led_driver/led_driver.h"
#include "gpio_ids.h"
#include "newmota_features.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"

namespace newmota::led_driver {
namespace {

/** @brief The active binding of any instance of one function base, or nullptr.
 * @param instance Receives the one-based instance of the binding found.
 */
const PinBinding* assigned(
    const Hardware& hardware, const std::uint16_t base, std::uint8_t& instance) {
    namespace gpio = generated::gpio;
    for (std::size_t index = 0U; index < hardware.binding_count(); ++index) {
        const auto* binding = hardware.binding(index);
        if (binding->active() && (binding->function >> gpio::kEncodingShift) == base) {
            instance = static_cast<std::uint8_t>((binding->function & gpio::kInstanceMask) + 1U);
            return binding;
        }
    }
    return nullptr;
}

} // namespace

void Bus::level(const PinId pin, const bool high) {
    const auto result = high ? platform::gpio_configure_input(pin, platform::GpioPull::up)
                             : platform::gpio_configure_output(pin, false);
    ok = ok && result == platform::GpioResult::success;
}

void Bus::idle() {
    level(data, true);
    level(clock, true);
}

void Bus::start(const std::uint8_t command) {
    level(data, false);
    platform::delay_microseconds(half_period_us);
    level(clock, false);
    write(command);
}

void Bus::write(const std::uint8_t value) {
    for (std::uint8_t bit = 0x80U; bit != 0U; bit >>= 1U) {
        level(data, (value & bit) != 0U);
        platform::delay_microseconds(half_period_us);
        level(clock, true);
        platform::delay_microseconds(half_period_us);
        level(clock, false);
    }
    // The chips acknowledge, but nothing can be done about a missing ACK.
    level(data, true);
    platform::delay_microseconds(half_period_us);
    level(clock, true);
    platform::delay_microseconds(half_period_us);
    level(clock, false);
}

void Bus::stop() {
    level(data, false);
    platform::delay_microseconds(half_period_us);
    level(clock, true);
    platform::delay_microseconds(half_period_us);
    level(data, true);
    platform::delay_microseconds(half_period_us);
}

// open() selects only a chip whose feature is built; the chip calls below are guarded the same
// way, so USE_LED_DRIVER links without any chip.
OpenResult Driver::open(const Hardware& hardware) {
    namespace gpio = generated::gpio;
    chip_ = Chip::none;
    std::uint8_t found = 0U;
    const auto take = [&](const Chip chip, const std::uint16_t clock_base,
                          const std::uint16_t data_base) {
        std::uint8_t clock_instance = 0U;
        std::uint8_t data_instance = 0U;
        const auto* clock = assigned(hardware, clock_base, clock_instance);
        const auto* data = assigned(hardware, data_base, data_instance);
        if (clock == nullptr || data == nullptr)
            return;
        ++found;
        chip_ = chip;
        bus_.clock = clock->pin;
        bus_.data = data->pin;
        data_index_ = data_instance;
    };
#if USE_BP5758D
    take(Chip::bp5758d, gpio::kGpioBp5758DClkBase, gpio::kGpioBp5758DDatBase);
#endif
#if USE_BP1658CJ
    take(Chip::bp1658cj, gpio::kGpioBp1658CjClkBase, gpio::kGpioBp1658CjDatBase);
#endif
#if USE_SM2335
    take(Chip::sm2335, gpio::kGpioSm2335ClkBase, gpio::kGpioSm2335DatBase);
#endif
#if USE_SM2135
    take(Chip::sm2135, gpio::kGpioSm2135ClkBase, gpio::kGpioSm2135DatBase);
#endif
    // Tasmota clocks SM2135 at 4 us half periods, the other chips at 2 us.
    bus_.half_period_us = chip_ == Chip::sm2135 ? 4U : 2U;
    (void)take;
    if (found > 1U) {
        chip_ = Chip::none;
        return OpenResult::several;
    }
    return chip_ == Chip::none ? OpenResult::none : OpenResult::chip;
}

std::array<std::uint8_t, 2> Driver::currents(const std::array<std::uint8_t, 2>& configured) const {
    std::array<std::uint8_t, 2> fallback{};
    switch (chip_) {
    case Chip::bp5758d:
        fallback = {{14U, 14U}}; // Tasmota drives every output at 14 mA.
        break;
    case Chip::bp1658cj:
    case Chip::sm2335:
        // Tasmota's DAT<n> selects current code n-1 for RGB and white alike.
        fallback.fill(static_cast<std::uint8_t>(data_index_ - 1U));
        break;
    case Chip::sm2135:
#if USE_SM2135
        fallback = chips::sm2135_currents(data_index_);
#endif
        break;
    case Chip::none:
        break;
    }
    std::array<std::uint8_t, 2> effective{};
    for (std::size_t i = 0U; i < effective.size(); ++i)
        // A value saved for another chip type may be out of this chip's range.
        effective[i] = valid_current(configured[i], i == 1U) ? configured[i] : fallback[i];
    return effective;
}

bool Driver::valid_current(const std::uint8_t value, const bool white) const {
    switch (chip_) {
    case Chip::bp5758d:
        return value >= 1U && value <= 93U; // mA; bit 6 weighs 30 mA, bits 0..5 up to 63 mA.
    case Chip::bp1658cj:
    case Chip::sm2335:
        return value <= 15U; // One current nibble each for RGB and white.
    case Chip::sm2135:
        return value <= (white ? 10U : 7U); // Codes up to 45 mA RGB, 60 mA white.
    case Chip::none:
        break;
    }
    return false;
}

bool Driver::begin(
    const std::array<std::uint8_t, 5>& channels, const std::array<std::uint8_t, 2>& configured) {
    if (chip_ == Chip::none)
        return false;
    channels_ = channels;
    sent_currents_ = currents(configured);
    sent_valid_ = false;
    sleeping_ = false;
    bus_.ok = true;
    bus_.idle();
    // Only BP5758D has a setup frame; the others carry their currents in every frame.
    if (chip_ == Chip::bp5758d) {
#if USE_BP5758D
        chips::bp5758d_setup(bus_, channels_, sent_currents_, true);
#endif
    }
    started_ = bus_.ok;
    return started_;
}

bool Driver::write(
    const std::array<std::uint16_t, 5>& values, const std::array<std::uint8_t, 2>& configured) {
    if (chip_ == Chip::none)
        return false;
    const auto effective = currents(configured);
    // A failed frame is sent again in full, setup included.
    if (!started_ && !begin(channels_, configured))
        return false;
    // The first frame always goes out: a chip may have kept its outputs across a restart.
    if (sent_valid_ && effective == sent_currents_ && values == sent_)
        return true;
    bus_.ok = true;
    switch (chip_) {
    case Chip::bp5758d:
#if USE_BP5758D
        if (effective != sent_currents_)
            chips::bp5758d_setup(bus_, channels_, effective, false);
        chips::bp5758d_write(bus_, values, sleeping_);
#endif
        break;
    case Chip::bp1658cj:
#if USE_BP1658CJ
        chips::bp1658cj_write(bus_, values, effective);
#endif
        break;
    case Chip::sm2335:
#if USE_SM2335
        chips::sm2335_write(bus_, values, effective);
#endif
        break;
    case Chip::sm2135:
#if USE_SM2135
        chips::sm2135_write(bus_, values, effective, data_index_);
#endif
        break;
    case Chip::none:
        break;
    }
    sent_currents_ = effective;
    sent_ = values;
    sent_valid_ = true;
    started_ = bus_.ok;
    return bus_.ok;
}

void chips::nibble_current_write(Bus& bus, const std::uint8_t command, const std::uint8_t standby,
    const bool split, const std::array<std::uint16_t, 5>& values,
    const std::array<std::uint8_t, 2>& currents) {
    bool dark = true;
    for (const auto value : values)
        dark = dark && ten_bits(value) == 0U;
    // Standby clears the current byte and all ten value bytes, as Tasmota sends it.
    bus.start(dark ? standby : command);
    bus.write(dark ? 0U : static_cast<std::uint8_t>((currents[0] << 4U) | currents[1]));
    for (const auto value : values) {
        const auto level = dark ? 0U : ten_bits(value);
        bus.write(static_cast<std::uint8_t>(split ? level & 0x1FU : level >> 8U));
        bus.write(static_cast<std::uint8_t>(split ? level >> 5U : level & 0xFFU));
    }
    bus.stop();
}

} // namespace newmota::led_driver
