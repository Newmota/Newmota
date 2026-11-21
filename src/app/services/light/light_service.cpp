/*
  light_service.cpp - portable PWM and Light behavior for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LightPreparePower/LightPowerOn and LightUpdateColorMapping reference: Tasmota
  xdrv_04_light.ino, Copyright (C) 2021 Theo Arends, GPL version 3 or later.
*/

/** @file light_service.cpp
 * @brief Portable PWM and Light behavior for Newmota.
 */

#include "services/light/light_service.h"
#include "services/light/light_color.h"
#include "newmota/platform/platform.h"
#include "core/command/command.h"
#include "newmota/log.h"
#include <algorithm>
#if USE_TIME
#include "services/time/time_service.h"
#endif
#if USE_TUYA_MCU
#include "drivers/tuya/tuya_mcu.h"
#endif

namespace newmota {
namespace {
constexpr std::uint32_t kMaximumFadeSpeed = 40U;

#if USE_PWM
/** @brief Decodes SetOption37 bits 0..6 into Tasmota's channel permutation.
 * @return For each output position, the Light channel it carries.
 */
std::array<std::uint8_t, 5> color_remap(std::uint8_t option) {
    std::uint32_t param = option & 127U;
    if (param > 119U)
        param = 0U;
    std::array<std::uint8_t, 5> free{{0U, 1U, 2U, 3U, 4U}};
    std::array<std::uint8_t, 5> remap{};
    // Each digit of the mixed-radix number 4!·3!·2!·1! picks one of the remaining channels.
    std::uint32_t radix = 24U;
    for (std::size_t position = 0U; position < 4U; ++position) {
        const auto pick = param / radix;
        param %= radix;
        remap[position] = free[pick];
        for (auto i = pick; i + 1U < free.size() - position; ++i)
            free[i] = free[i + 1U];
        radix /= 4U - position;
    }
    remap[4] = free[0];
    return remap;
}
#endif
} // namespace

void LightService::initialize(
    const Hardware& hardware, Settings& settings, PowerService& power, const std::uint32_t now) {
    settings_ = &settings;
    // Kept for Tasmota templates: a fifth RGBWWTable value of 0 is Tasmota's deprecated way to
    // select white blend, which its LightInit turns into SetOption105 1 and RGBWWTable 255.
    if (settings.light_settings().rgbww_table[4] == 0U) {
        auto converted = settings.light_settings();
        converted.rgbww_table[4] = 255U;
        (void)settings.set_option(105U, 1U);
        (void)settings.set_light_settings(converted, true);
    }
#if USE_POWER_PROVENANCE || USE_POWER_TARGETS
    power_ = &power;
#endif
#if USE_POWER_TARGETS
    PowerService::TargetUpdate target_update(power_);
#endif
    std::uint8_t independent = 0U;
#if USE_PWM
    std::uint8_t light = 0U;
    (void)settings.option(15U, light);
#endif
    (void)settings.option(68U, independent);
    independent_ = independent != 0U;
    // Ownership is frozen until restart; retained handles never change owner
    // mid-fade.

    bool chip = false;
    bool conflict = false;
#if USE_TUYA_MCU
    // A TuyaMCU light takes the Light as Tasmota's TuyaMCU light_type does; PWM, chip and strip
    // pins are then left alone.
    if (const auto layout = tuya::light_layout(); layout.channels != 0U) {
        tuya_ = true;
        mode_switch_ = layout.mode;
        channel_count_ = layout.channels;
        independent_ = layout.independent;
        for (std::size_t i = 0U; i < channel_count_; ++i)
            channel_indices_[i] = static_cast<std::uint8_t>(i);
    }
#endif
#if USE_LED_DRIVER
    const auto opened = tuya_ ? led_driver::OpenResult::none : led_driver_.open(hardware);
    chip = opened == led_driver::OpenResult::chip;
    if (opened == led_driver::OpenResult::several) {
        AddLog(LogLevel::error, "LGT: more than one LED driver chip assigned, Light disabled");
        conflict = true;
    }
#endif
#if USE_WS2812
    if (const auto* binding = tuya_ ? nullptr : hardware.find_function(PinRole::led_strip, 1U);
        chip && binding) {
        AddLog(LogLevel::error, "LGT: LED driver chip and WS2812 both assigned, Light disabled");
        conflict = true;
    } else if (binding && !conflict) {
        addressable_ = true;
        strip_pin_ = binding->pin;
        channel_count_ = (settings.light_settings().strip_type & 8U) != 0U ? 4U : 3U;
        for (std::size_t i = 0U; i < channel_count_; ++i)
            channel_indices_[i] = static_cast<std::uint8_t>(i);
        const auto result = led_strip::open(strip_config(settings.light_settings()), strip_);
        if (result == led_strip::Result::success)
            (void)led_strip::set_editor(strip_, this, strip_edit, strip_fault);
        else
            last_result_ = platform::PwmResult::native_failure;
    }
#endif
#if USE_LED_DRIVER
    if (chip && !conflict) {
        chip_ = true;
        channel_count_ = 5U;
        for (std::size_t i = 0U; i < channel_count_; ++i)
            channel_indices_[i] = static_cast<std::uint8_t>(i);
    }
#endif
#if USE_PWM
    for (std::size_t i = 0U; !addressable_ && !chip && !tuya_ && !conflict && i < 5U &&
                             light != 0U && channel_count_ < 5U;
        ++i) {
        if (hardware.find_function(PinRole::pwm, static_cast<std::uint8_t>(i + 1U)) == nullptr)
            continue;
        channel_indices_[channel_count_++] = static_cast<std::uint8_t>(i);
        owned_mask_ |= static_cast<std::uint16_t>(1U << i);
    }
#endif
    channel_mask_ = addressable_ || chip_ || tuya_
                        ? static_cast<std::uint16_t>((1U << channel_count_) - 1U)
                        : owned_mask_;
    std::uint8_t remap = 0U;
    (void)settings.option(37U, remap);
    // The MCU shows colour or white, never both, so SetOption37 cannot split it.
    separate_ = !independent_ && !tuya_ && channel_count_ >= 4U && (remap & 128U) != 0U;
#if USE_PWM
    // Output position k carries channel order[k]. A permutation that would leave one of
    // this layout's outputs dark keeps the identity order.
    const auto order = color_remap(remap);
    if (!addressable_ && !tuya_ &&
        std::all_of(order.begin(), order.begin() + channel_count_,
            [this](const std::uint8_t channel) { return channel < channel_count_; })) {
        const auto positions = channel_indices_;
        for (std::size_t k = 0U; k < channel_count_; ++k)
            channel_indices_[order[k]] = positions[k];
    }
#endif
#if USE_LED_DRIVER
    if (chip_) {
        std::array<std::uint8_t, 5> carried{};
        for (std::size_t channel = 0U; channel < channel_count_; ++channel)
            carried[channel_indices_[channel]] = static_cast<std::uint8_t>(channel);
        if (!led_driver_.begin(carried, settings.light_settings().led_current))
            last_result_ = platform::PwmResult::native_failure;
    }
#endif
#if USE_TUYA_MCU
    mapping_valid_ =
        !conflict && endpoints_.configure(hardware, endpoint_count(), power.external_relays());
    if (tuya_)
        tuya::attach_light(*this);
#else
    mapping_valid_ = !conflict && endpoints_.configure(hardware, endpoint_count());
#endif
#if USE_PWM
    outputs_.initialize(hardware, settings.light_settings());
#endif
    power.attach_light(*this);
    std::uint8_t save = 0U;
    (void)settings.option(0U, save);
    const auto count = endpoint_count();
    for (std::uint8_t i = 1U; i <= count && mapping_valid_; ++i) {
        bool on = false;
        const auto policy = settings.power_on_state();
        if (policy == 4U)
            on = true;
        else if (!platform::reset_was_power_on())
            on = save != 0U && (settings.light_settings().desired_power & (1U << (i - 1U))) != 0U;
        else if (policy == 1U)
            on = true;
        else if (policy == 2U || policy == 3U) {
            on = save != 0U && (settings.light_settings().desired_power & (1U << (i - 1U))) != 0U;
            if (policy == 2U && save != 0U)
                on = !on;
        }
        if (on)
            desired_on_ |= static_cast<std::uint8_t>(1U << (i - 1U));
    }
    if (available()) {
        auto boot_intent = intent();
        if ((independent_ && !addressable_) || channel_count_ < 3U || tuya_ ||
            (!addressable_ && boot_intent.scheme > 4U))
            boot_intent.scheme = 0U;
        if (boot_intent.scheme == 1U &&
            (boot_intent.dimmer == 0U ||
                (boot_intent.channels[0] == 0U && boot_intent.channels[1] == 0U &&
                    boot_intent.channels[2] == 0U)))
            boot_intent.scheme = 0U;
        boot_intent.desired_power = desired_on_;
        (void)settings.set_light_settings(boot_intent);
    }
#if USE_PWM
    // Raw outputs restore independently, so one unavailable route cannot block
    // another.
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (!outputs_.configured(i) || owns(i))
            continue;
        std::array<std::uint16_t, 16> duties{};
        duties[i] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(settings.light_settings().raw[i]) * 65535U +
                settings.light_settings().range / 2U) /
            settings.light_settings().range);
        (void)outputs_.write(duties, static_cast<std::uint16_t>(1U << i));
    }
#endif
    if (available()) {
        active_scheme_ = intent().scheme;
        effect_channels_ = intent().channels;
        effect_wheel_ = intent().palette_index;
        effect_started_ = next_effect_ = now;
        effect_level_ = active_scheme_ == 1U ? 0U : 65535U;
        target_ = target();
        current_ = {};
        last_result_ = sample(now, channel_mask_);
    }
}

std::uint16_t LightService::endpoint_channels(const std::uint8_t instance) const {
    std::uint16_t mask = 0U;
    for (std::size_t i = 0U; i < channel_count_; ++i)
        if (endpoint_for_channel(i) + 1U == instance)
            mask |= static_cast<std::uint16_t>(1U << channel_indices_[i]);
    return mask;
}

void LightService::start_effect_at(const std::uint16_t color) {
    const auto count = intent().palette_count != 0U ? intent().palette_count : 256U;
    effect_wheel_ = static_cast<std::uint16_t>(color % count);
    effect_initial_ = true;
    next_effect_ = platform::monotonic_milliseconds();
}

void LightService::effect_step(const std::uint32_t now) {
    if (!available() || (independent_ && !addressable_) || channel_count_ < 3U || tuya_ ||
        intent().scheme == 0U)
        return;
    if (active_scheme_ != intent().scheme) {
        active_scheme_ = intent().scheme;
        effect_initial_ = false;
        effect_channels_ = intent().channels;
        effect_wheel_ = intent().palette_index;
        next_effect_ = now;
        effect_started_ = now;
        effect_level_ = active_scheme_ == 1U ? 0U : 65535U;
    }
    if ((addressable_ ? desired_on_ == 0U : (desired_on_ & 1U) == 0U) ||
        static_cast<std::int32_t>(now - next_effect_) < 0)
        return;
    if (active_scheme_ == 1U) {
        next_effect_ = now + 20U;
        const auto elapsed = now - effect_started_;
        const auto duration = static_cast<std::uint32_t>(intent().wakeup_duration) * 1000U;
        effect_level_ = elapsed >= duration
                            ? 65535U
                            : static_cast<std::uint16_t>(
                                  static_cast<std::uint64_t>(elapsed) * 65535U / duration);
        (void)retarget(now, separate_ ? endpoint_channels(1U) : channel_mask_);
        if (elapsed >= duration) {
            auto next = intent();
            next.scheme = 0U;
            if (settings_->set_light_settings(next) != SettingsUpdateResult::failed)
                active_scheme_ = 0U;
        }
        return;
    }
#if USE_WS2812
    if (addressable_ && active_scheme_ == 14U)
        return;
    if (addressable_ && active_scheme_ >= 5U) {
        if (active_scheme_ >= 6U && intent().strip_step_pixels != 0U && strip_steps_complete_)
            return;
        next_effect_ = now + (active_scheme_ == 5U ? 250U : 20U);
        (void)sample(now, channel_mask_);
        return;
    }
#endif
    next_effect_ = now + static_cast<std::uint32_t>(intent().speed) * 100U;
    const auto count = intent().palette_count != 0U ? intent().palette_count : 256U;
    if (effect_initial_) {
        effect_wheel_ = static_cast<std::uint16_t>(effect_wheel_ % count);
        effect_initial_ = false;
    } else if (active_scheme_ == 4U) {
        effect_random_ ^= effect_random_ << 13U;
        effect_random_ ^= effect_random_ >> 17U;
        effect_random_ ^= effect_random_ << 5U;
        effect_wheel_ = static_cast<std::uint16_t>(effect_random_ % count);
    } else if (active_scheme_ == 2U)
        effect_wheel_ = static_cast<std::uint16_t>((effect_wheel_ + 1U) % count);
    else
        effect_wheel_ = static_cast<std::uint16_t>((effect_wheel_ + count - 1U) % count);
    if (intent().palette_count != 0U)
        effect_channels_ = intent().palette[effect_wheel_];
    else {
        const auto hue = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(effect_wheel_) * 359U + 127U) / 255U);
        const auto sector = hue / 60U;
        const auto rise = static_cast<std::uint8_t>((hue % 60U) * 255U / 60U);
        const auto fall = static_cast<std::uint8_t>(255U - rise);
        constexpr std::uint8_t full = 255U;
        switch (sector) {
        case 0U:
            effect_channels_ = {full, rise, 0U, 0U, 0U};
            break;
        case 1U:
            effect_channels_ = {fall, full, 0U, 0U, 0U};
            break;
        case 2U:
            effect_channels_ = {0U, full, rise, 0U, 0U};
            break;
        case 3U:
            effect_channels_ = {0U, fall, full, 0U, 0U};
            break;
        case 4U:
            effect_channels_ = {rise, 0U, full, 0U, 0U};
            break;
        default:
            effect_channels_ = {full, 0U, fall, 0U, 0U};
            break;
        }
    }
    (void)retarget(now, separate_ ? endpoint_channels(1U) : channel_mask_);
}

/** @brief Applies white blend and RGBWWTable after gamma, as Tasmota's calcGammaBulbs does. */
/** @brief Applies gamma like Tasmota's calcGammaBulbCW: the whites share one curve on their sum.
 *
 * Gamma per white channel would dim a CW/WW mix and shift its colour temperature at low
 * brightness. Whites set beyond full scale together are independent and keep their own gamma.
 */
void LightService::correct(std::array<std::uint16_t, 16>& electrical) const {
    // Tasmota's margin of 8 above 1023 in 10 bits, for rounding.
    constexpr std::uint32_t kFreeWhites = 65535U + 8U * 65535U / 1023U;
    const bool pair =
        !addressable_ && !independent_ && (channel_count_ == 2U || channel_count_ == 5U);
    const std::size_t cold = channel_indices_[channel_count_ == 2U ? 0U : 3U];
    const std::size_t warm = channel_indices_[channel_count_ == 2U ? 1U : 4U];
    const std::uint32_t cold_level = electrical[cold];
    const std::uint32_t warm_level = electrical[warm];
    for (auto& duty : electrical)
        duty = light_color::gamma16(duty);
    const auto sum = cold_level + warm_level;
    if (!pair || sum == 0U || sum > kFreeWhites)
        return;
    const auto white = std::min<std::uint32_t>(sum, 65535U);
    const std::uint32_t corrected = light_color::gamma16(static_cast<std::uint16_t>(white));
    electrical[cold] = static_cast<std::uint16_t>((cold_level * corrected + white / 2U) / white);
    electrical[warm] = static_cast<std::uint16_t>((warm_level * corrected + white / 2U) / white);
}

bool LightService::shows_color() const {
    const auto& channels = intent().channels;
    return channel_count_ >= 3U &&
           (channel_count_ == 3U || (channels[0] | channels[1] | channels[2]) != 0U ||
               (channels[3] | channels[4]) == 0U);
}

bool LightService::blending() const {
    std::uint8_t blend = 0U;
    (void)settings_->option(105U, blend);
    return blend != 0U && !addressable_ && !independent_ && channel_count_ >= 4U;
}

void LightService::balance(std::array<std::uint16_t, 16>& electrical) const {
    if (addressable_ || independent_)
        return;
    const auto at = [&](const std::size_t channel) -> std::uint16_t& {
        return electrical[channel_indices_[channel]];
    };
    const auto& table = intent().rgbww_table;
    std::size_t scaled = channel_count_;
    // SetOption105 moves the common RGB part to white while white is off; RGBWWTable white
    // then sets the white level and RGBCW splits it by CT.
    if (blending() && at(3) == 0U && (channel_count_ == 4U || at(4) == 0U)) {
        const auto common = std::min({at(0), at(1), at(2)});
        for (std::size_t c = 0U; c < 3U; ++c)
            at(c) = static_cast<std::uint16_t>(at(c) - common);
        const auto white = (static_cast<std::uint32_t>(common) * table[3] + 127U) / 255U;
        if (channel_count_ == 4U) {
            at(3) = static_cast<std::uint16_t>(white);
        } else {
            const auto warm = white * (intent().color_temperature - 153U) / 347U;
            at(4) = static_cast<std::uint16_t>(warm);
            at(3) = static_cast<std::uint16_t>(white - warm);
        }
        scaled = 3U;
    }
    for (std::size_t c = 0U; c < scaled; ++c)
        at(c) = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(at(c)) * table[c] + 127U) / 255U);
}

bool LightService::owns(const std::size_t index) const {
    return index < 16U && (owned_mask_ & (1U << index)) != 0U;
}

std::array<std::uint16_t, 16> LightService::target() const {
    std::array<std::uint16_t, 16> duties{};
    const auto& value = intent();
    for (std::size_t i = 0U; i < channel_count_; ++i) {
        const bool on = (desired_on_ & (1U << endpoint_for_channel(i))) != 0U;
        // Strip patterns carry their own colors. Fade their output level even
        // when the retained Color component is zero.
        const auto component = addressable_ && active_scheme_ >= 5U ? 255U
                               : active_scheme_ != 0U && i < 3U     ? effect_channels_[i]
                                                                    : value.channels[i];
        const auto brightness = separate_ && i >= 3U ? value.white_dimmer : value.dimmer;
        duties[channel_indices_[i]] =
            on ? static_cast<std::uint16_t>(
                     (component * static_cast<std::uint32_t>(brightness) * 65535U + 12750U) /
                     25500U)
               : 0U;
        if (active_scheme_ == 1U && (!separate_ || i < 3U))
            duties[channel_indices_[i]] = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(duties[channel_indices_[i]]) * effect_level_ / 65535U);
    }
    return duties;
}

platform::PwmResult LightService::retarget(const std::uint32_t now, const std::uint16_t selected) {
    if ((transition_mask_ & selected) != 0U) {
        const auto result = sample(now, static_cast<std::uint16_t>(transition_mask_ & selected));
        if (result != platform::PwmResult::success)
            return result;
    }
    const auto next = target();
    std::uint32_t maximum_distance = 0U;
    for (std::size_t i = 0U; i < 16U; ++i) {
        if ((selected & (1U << i)) == 0U)
            continue;
        from_[i] = current_[i];
        target_[i] = next[i];
        const auto distance = static_cast<std::uint32_t>(
            from_[i] > target_[i] ? from_[i] - target_[i] : target_[i] - from_[i]);
        maximum_distance = std::max(maximum_distance, distance);
        duration_[i] = distance;
    }
    for (std::size_t i = 0U; i < 16U; ++i) {
        if ((selected & (1U << i)) == 0U)
            continue;
        const auto distance = independent_ ? duration_[i] : maximum_distance;
        // Speed is 1..40 by command; clamping restored values keeps a full fade within 20 s,
        // so the distance product and the 32-bit interpolation in sample() cannot overflow.
        const auto speed = std::min<std::uint32_t>(intent().speed, kMaximumFadeSpeed);
        // The MCU fades on its own, so it gets the target at once.
        duration_[i] = (intent().fade && active_scheme_ != 1U && !tuya_)
                           ? (distance * speed * 500U + 65534U) / 65535U
                           : 0U;
        started_[i] = now;
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if (duration_[i] != 0U)
            transition_mask_ |= bit;
        else
            transition_mask_ &= static_cast<std::uint16_t>(~bit);
    }
    ++revision_;
    next_sample_ = now;
    return sample(now, selected);
}

platform::PwmResult LightService::sample(const std::uint32_t now, const std::uint16_t selected) {
    auto perceptual = current_;
    for (std::size_t i = 0U; i < 16U; ++i) {
        if ((selected & (1U << i)) == 0U)
            continue;
        const auto elapsed = now - started_[i];
        const auto delta = static_cast<std::int32_t>(target_[i]) - from_[i];
        perceptual[i] = elapsed >= duration_[i] ? target_[i]
                                                : static_cast<std::uint16_t>(
                                                      static_cast<std::int32_t>(from_[i]) +
                                                      delta * static_cast<std::int32_t>(elapsed) /
                                                          static_cast<std::int32_t>(duration_[i]));
    }
    auto electrical = perceptual;
    if (intent().gamma)
        correct(electrical);
    balance(electrical);
    auto result = platform::PwmResult::success;
    std::uint16_t confirmed = 0U;
#if USE_TUYA_MCU
    if (tuya_) {
        tuya::write_light();
        confirmed = selected;
    } else
#endif
#if USE_LED_DRIVER
        if (chip_) {
        std::array<std::uint16_t, 5> values{};
        std::copy_n(electrical.begin(), values.size(), values.begin());
        result = led_driver_.write(values, intent().led_current)
                     ? platform::PwmResult::success
                     : platform::PwmResult::native_failure;
        if (result == platform::PwmResult::success)
            confirmed = selected;
    } else
#endif
#if USE_WS2812
        if (addressable_) {
        result = strip_sample(now, perceptual);
        if (result == platform::PwmResult::success)
            confirmed = selected;
    } else
#endif
        if (independent_) {
        // Each independent endpoint is its own operation. A failed route leaves
        // siblings usable.
        for (std::size_t i = 0U; i < 16U; ++i) {
            const auto bit = static_cast<std::uint16_t>(1U << i);
            if ((selected & bit) == 0U)
                continue;
            const auto applied = outputs_.write(electrical, bit);
            confirmed |= outputs_.confirmed_mask();
            if (applied != platform::PwmResult::success)
                result = applied;
        }
    } else {
        // White blend couples RGB and white, so a change to either group rewrites both.
        const auto written =
            blending() ? static_cast<std::uint16_t>(selected | channel_mask_) : selected;
        result = outputs_.write(electrical, written);
        confirmed = outputs_.confirmed_mask();
    }
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (!addressable_ && !chip_ && !tuya_ && (confirmed & (1U << i)) != 0U &&
            outputs_.status(i) != platform::PwmResult::success)
            confirmed &= static_cast<std::uint16_t>(~(1U << i));
    }
    bool changed = false;
    for (std::size_t i = 0U; i < 16U; ++i) {
        const auto bit = static_cast<std::uint16_t>(1U << i);
        if ((selected & bit) == 0U)
            continue;
        if ((confirmed & bit) != 0U) {
            changed = changed || current_[i] != perceptual[i];
            current_[i] = perceptual[i];
        }
        if ((confirmed & bit) == 0U || now - started_[i] >= duration_[i])
            transition_mask_ &= static_cast<std::uint16_t>(~bit);
    }
    const auto prior = applied_on_;
    if (independent_ || separate_) {
        for (std::uint8_t endpoint = 1U; endpoint <= endpoint_count(); ++endpoint) {
            const auto members = endpoint_channels(endpoint);
            if ((confirmed & members) != members)
                continue;
            const auto bit = static_cast<std::uint8_t>(1U << (endpoint - 1U));
            applied_on_ = static_cast<std::uint8_t>((applied_on_ & ~bit) | (desired_on_ & bit));
        }
    } else if (result == platform::PwmResult::success)
        applied_on_ = desired_on_;
    if (changed || prior != applied_on_ || result != platform::PwmResult::success)
        ++revision_;
    last_result_ = result;
    return result;
}

void LightService::poll(const std::uint32_t now) {
    effect_step(now);
    if (transition_mask_ == 0U || static_cast<std::int32_t>(now - next_sample_) < 0)
        return;
    next_sample_ = now + 20U;
    (void)sample(now, transition_mask_);
}

platform::PwmResult LightService::update(const LightSettings& value, const bool turn_on,
    const std::uint8_t channel, const std::uint16_t effect_start) {
    if (effect_start > 256U || (effect_start < 256U && value.scheme < 2U))
        return platform::PwmResult::invalid_request;
    if (!available() || !value.valid() || channel > channel_count_)
        return platform::PwmResult::invalid_request;
    if ((!addressable_ && value.scheme > 4U) ||
        (value.scheme != 0U && ((independent_ && !addressable_) || channel_count_ < 3U || tuya_)))
        return platform::PwmResult::invalid_request;
    const auto selected =
        independent_ && channel != 0U
            ? static_cast<std::uint16_t>(1U << channel_indices_[channel - 1U])
        : separate_ && channel != 0U
            ? endpoint_channels(static_cast<std::uint8_t>(endpoint_for_channel(channel - 1U) + 1U))
            : channel_mask_;
    // Reject a fault before persisting a new intent. Native write failures
    // retain accepted intent.
    for (std::size_t i = 0U; i < channel_count_; ++i) {
        if ((selected & (1U << channel_indices_[i])) == 0U)
            continue;
        const auto result =
            addressable_ || chip_ || tuya_ ? last_result_ : outputs_.status(channel_indices_[i]);
        if (result != platform::PwmResult::success)
            return result;
    }
    if ((transition_mask_ & selected) != 0U) {
        const auto result = sample(platform::monotonic_milliseconds(),
            static_cast<std::uint16_t>(transition_mask_ & selected));
        if (result != platform::PwmResult::success)
            return result;
    }
    auto next = value;
    // A black RGB ramp is already complete; do not retain a wakeup
    // whose power-off endpoint prevents the cooperative timeline advancing.
    if (next.scheme == 1U &&
        (next.dimmer == 0U ||
            (next.channels[0] == 0U && next.channels[1] == 0U && next.channels[2] == 0U)))
        next.scheme = 0U;
    auto accepted_power = desired_on_;
    if (turn_on) {
        bool use_channel_intent = true;
#if USE_WS2812
        if (addressable_ && value.scheme == 14U) {
            const auto manual = active_scheme_ == 14U && value.strip_pixels == intent().strip_pixels
                                    ? manual_power(value, manual_pixels_.data(), pixel_count())
                                    : 0U;
            for (std::uint8_t endpoint = 1U; endpoint <= endpoint_count(); ++endpoint) {
                if ((endpoint_channels(endpoint) & selected) == 0U)
                    continue;
                const auto bit = static_cast<std::uint8_t>(1U << (endpoint - 1U));
                accepted_power =
                    static_cast<std::uint8_t>((accepted_power & ~bit) | (manual & bit));
            }
            use_channel_intent = false;
        }
#endif
        if (use_channel_intent) {
            if (independent_ && channel != 0U) {
                const auto mask = static_cast<std::uint8_t>(1U << (channel - 1U));
                if (value.channels[channel - 1U] == 0U || value.dimmer == 0U)
                    accepted_power &= static_cast<std::uint8_t>(~mask);
                else
                    accepted_power |= mask;
            } else {
                for (std::uint8_t endpoint = 1U; endpoint <= endpoint_count(); ++endpoint) {
                    const auto members = endpoint_channels(endpoint);
                    if ((members & selected) == 0U)
                        continue;
                    const auto bit = static_cast<std::uint8_t>(1U << (endpoint - 1U));
                    accepted_power &= static_cast<std::uint8_t>(~bit);
                    if (endpoint == 1U && value.scheme >= 2U)
                        accepted_power |= bit;
                    for (std::size_t i = 0U; i < channel_count_; ++i)
                        if (endpoint_for_channel(i) + 1U == endpoint && value.channels[i] != 0U &&
                            (separate_ && i >= 3U ? value.white_dimmer : value.dimmer) != 0U)
                            accepted_power |= bit;
                }
            }
        }
    }
#if USE_WS2812
    const bool strip_count_changed = addressable_ && next.strip_pixels != intent().strip_pixels;
    const bool clear_manual_pixels =
        addressable_ && (strip_count_changed || (next.scheme == 14U && active_scheme_ != 14U));
    if (clear_manual_pixels && next.scheme == 14U)
        accepted_power = 0U;
    // Before saving, so a strip that could not grow keeps a count it can drive after a restart. A
    // strip that failed to open has nothing to grow, and a smaller saved count may be its way back.
    if (strip_count_changed && led_strip::active(strip_)) {
        const auto reserved = led_strip::reserve(strip_, next.strip_pixels);
        if (reserved != led_strip::Result::success)
            return reserved == led_strip::Result::failed
                       ? platform::PwmResult::native_failure
                       : platform::PwmResult::resource_unavailable;
    }
#endif
    next.desired_power = accepted_power;
    if (settings_->set_light_settings(next) == SettingsUpdateResult::failed)
        return platform::PwmResult::native_failure;
#if USE_POWER_PROVENANCE
    /** Source LightPreparePower attributes only accepted linked power changes to SRC_LIGHT. */
    if (power_ && desired_on_ != accepted_power)
        power_->record_source(PowerOperationSource::light);
#endif
#if USE_POWER_TARGETS
    PowerService::TargetUpdate target_update(power_, desired_on_ != accepted_power);
#endif
    desired_on_ = accepted_power;
#if USE_WS2812
    if (addressable_) {
        strip_steps_complete_ = false;
        if (clear_manual_pixels)
            manual_pixels_ = {};
        const auto configured = led_strip::configure(strip_, strip_config(next));
        if (configured != led_strip::Result::success)
            return platform::PwmResult::native_failure;
    }
#endif
    if (active_scheme_ != next.scheme ||
        (next.scheme == 1U && turn_on && (selected & endpoint_channels(1U)) != 0U)) {
        active_scheme_ = next.scheme;
        effect_initial_ = false;
        effect_channels_ = next.channels;
        effect_wheel_ = next.palette_index;
        next_effect_ = platform::monotonic_milliseconds();
        effect_started_ = next_effect_;
        effect_level_ = active_scheme_ == 1U ? 0U : 65535U;
    }
    if (effect_start < 256U)
        start_effect_at(effect_start);
    transition_mask_ &= static_cast<std::uint16_t>(~selected);
    const auto now = platform::monotonic_milliseconds();
    if (turn_on && next.scheme >= 2U && next.scheme < 5U &&
        (selected & endpoint_channels(1U)) != 0U &&
        static_cast<std::int32_t>(now - next_effect_) >= 0) {
        effect_step(now);
        return last_result_;
    }
    return retarget(now, selected);
}

platform::PwmResult LightService::raw_write(const std::size_t index, const std::uint16_t value) {
    if (owns(index))
        return platform::PwmResult::ownership_conflict;
    if (!outputs_.configured(index) || value > intent().range)
        return platform::PwmResult::invalid_request;
    const auto result = outputs_.status(index);
    if (result != platform::PwmResult::success)
        return result;
    auto next = intent();
    next.raw[index] = value;
    if (settings_->set_light_settings(next) == SettingsUpdateResult::failed)
        return platform::PwmResult::native_failure;
    std::array<std::uint16_t, 16> duties{};
    duties[index] = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(value) * 65535U + next.range / 2U) / next.range);
    ++revision_;
    return outputs_.write(duties, static_cast<std::uint16_t>(1U << index));
}

platform::PwmResult LightService::timing(const LightSettings& value, const std::uint16_t selected) {
    if (!value.valid())
        return platform::PwmResult::invalid_request;
    if (timing_pending_ || outputs_.requires_restart()) {
        if (settings_->set_light_settings(value, true) == SettingsUpdateResult::failed)
            return platform::PwmResult::native_failure;
        timing_pending_ = true;
        settings_->require_pwm_restart();
        return platform::PwmResult::success;
    }
    const auto result = outputs_.reconfigure(value, false, selected);
    if (result != platform::PwmResult::success)
        return result;
    if (settings_->set_light_settings(value, true) == SettingsUpdateResult::failed)
        return platform::PwmResult::native_failure;
    auto applied = outputs_.activate_reconfiguration();
    if (applied != platform::PwmResult::success) {
        timing_pending_ = true;
        settings_->require_pwm_restart();
        return applied;
    }
    // Range edits clamp raw intent, then apply its newly normalized duty.
    for (std::size_t i = 0U; i < 16U; ++i) {
        if ((selected & (1U << i)) == 0U || !outputs_.configured(i) || owns(i))
            continue;
        std::array<std::uint16_t, 16> duties{};
        duties[i] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(value.raw[i]) * 65535U + value.range / 2U) / value.range);
        applied = outputs_.write(duties, static_cast<std::uint16_t>(1U << i));
        if (applied != platform::PwmResult::success)
            return applied;
    }
    return platform::PwmResult::success;
}

bool LightService::state(const std::uint8_t instance, bool& on) const {
    if (!available() || instance == 0U || instance > endpoint_count())
        return false;
    on = (applied_on_ & (1U << (instance - 1U))) != 0U;
    return true;
}

#if USE_POWER_TARGETS
std::uint32_t LightService::intent_mask() const {
    std::uint32_t on{};
    for (unsigned index = 1; index <= 32; ++index) {
        const auto endpoint = endpoints_.get(static_cast<std::uint8_t>(index));
        if (endpoint.owner == PowerOwner::light && (desired_on_ & (1U << (endpoint.instance - 1))))
            on |= std::uint32_t{1} << (index - 1);
    }
    return on;
}
#endif

PowerChangeResult LightService::set(
    const std::uint8_t instance, const PowerAction action, PowerOperationSource source) {
    bool on = false;
    if (!state(instance, on) || action == PowerAction::blink || action == PowerAction::blink_stop)
        return PowerChangeResult::unavailable;
#if USE_POWER_PROVENANCE
    if (power_)
        power_->record_source(source);
#else
    (void)source;
#endif
    const auto selected = endpoint_channels(instance);
    for (std::size_t i = 0U; i < channel_count_; ++i) {
        if ((selected & (1U << channel_indices_[i])) != 0U &&
            (addressable_ || chip_ || tuya_
                    ? last_result_
                    : outputs_.status(channel_indices_[i])) != platform::PwmResult::success)
            return PowerChangeResult::failed;
    }
    if ((transition_mask_ & selected) != 0U &&
        sample(platform::monotonic_milliseconds(),
            static_cast<std::uint16_t>(transition_mask_ & selected)) !=
            platform::PwmResult::success)
        return PowerChangeResult::failed;
    if (action == PowerAction::toggle)
        on = (desired_on_ & (1U << (instance - 1U))) == 0U;
    else
        on = action == PowerAction::on;
    const auto mask = static_cast<std::uint8_t>(1U << (instance - 1U));
    const auto prior = desired_on_;
    if (on)
        desired_on_ |= mask;
    else
        desired_on_ &= static_cast<std::uint8_t>(~mask);
    auto next = intent();
    if (on && separate_ && instance == 2U && next.white_dimmer == 0U)
        next.white_dimmer = 100U;
    if (on && (!separate_ || instance == 1U) && next.dimmer == 0U)
        next.dimmer = 100U;
    if (on) {
        if (independent_ && next.channels[instance - 1U] == 0U)
            next.channels[instance - 1U] = 255U;
        if (!independent_) {
            bool black = true;
            for (std::size_t i = 0U; i < channel_count_; ++i)
                if ((selected & (1U << channel_indices_[i])) != 0U)
                    black = black && next.channels[i] == 0U;
            if (black)
                for (std::size_t i = 0U; i < channel_count_; ++i)
                    if ((selected & (1U << channel_indices_[i])) != 0U)
                        next.channels[i] = 255U;
        }
    }
    next.desired_power = desired_on_;
    if (settings_->set_light_settings(next) == SettingsUpdateResult::failed) {
        desired_on_ = prior;
        return PowerChangeResult::failed;
    }
#if USE_POWER_TARGETS
    PowerService::TargetUpdate target_update(power_);
#endif
    transition_mask_ &= static_cast<std::uint16_t>(~selected);
    const auto result = retarget(platform::monotonic_milliseconds(), selected);
    return result != platform::PwmResult::success ? PowerChangeResult::failed
           : prior == desired_on_                 ? PowerChangeResult::unchanged
                                                  : PowerChangeResult::changed;
}

std::uint32_t LightService::state_mask() const {
    std::uint32_t mask = 0U;
    for (std::uint8_t i = 1U; i <= endpoint_count(); ++i) {
        bool on = false;
        if (!state(i, on) || !on)
            continue;
        const auto index = endpoints_.index(PowerOwner::light, i);
        if (index != 0U)
            mask |= 1U << (index - 1U);
    }
    return mask;
}
} // namespace newmota

namespace newmota {
bool LightService::write_state_fields(CommandOutput& output, const bool comma) const {
    if (outputs_.mask() == 0U && !addressable_ && !chip_ && !tuya_)
        return true;
    if (comma && !output.write(","))
        return false;
    if (available()) {
        if (!output.format("\"Dimmer\":%u,\"Color\":\"", static_cast<unsigned>(intent().dimmer)))
            return false;
        const bool decimal = option(17U) != 0U;
        for (std::size_t i = 0U; i < channel_count_; ++i) {
            const auto value =
                (static_cast<unsigned>(intent().channels[i]) *
                        (separate_ && i >= 3U ? intent().white_dimmer : intent().dimmer) +
                    50U) /
                100U;
            if (decimal && i != 0U && !output.write(","))
                return false;
            if (!output.format(decimal ? "%u" : "%02X", value))
                return false;
        }
        if (!output.format(
                "\",\"CT\":%u,\"Fade\":\"%s\",\"Speed\":%u,\"LedTable\":\"%s\","
                "\"Scheme\":%u,\"Dimmer2\":%u,\"Light\":{\"Transition\":%s,\"DesiredPower\":%u,"
                "\"AppliedPower\":%u,\"Result\":%u},",
                static_cast<unsigned>(intent().color_temperature), intent().fade ? "ON" : "OFF",
                static_cast<unsigned>(intent().speed), intent().gamma ? "ON" : "OFF",
                static_cast<unsigned>(intent().scheme),
                static_cast<unsigned>(separate_ ? intent().white_dimmer : intent().dimmer),
                transitioning() ? "true" : "false", static_cast<unsigned>(desired_on_),
                static_cast<unsigned>(applied_on_), static_cast<unsigned>(last_result_)))
            return false;
    }
    if (!output.format("\"PWMStatus\":{\"Configured\":%u,\"LightOwned\":%u,\"Duty\":[",
            static_cast<unsigned>(outputs_.mask()), static_cast<unsigned>(owned_mask_)))
        return false;
    for (std::size_t i = 0U; i < 16U; ++i) {
        if (!output.format("%s%u", i == 0U ? "" : ",", static_cast<unsigned>(outputs_.applied(i))))
            return false;
    }
    if (!output.write("],\"Result\":["))
        return false;
    for (std::size_t i = 0U; i < 16U; ++i) {
        auto result = platform::PwmResult::success;
        if (outputs_.configured(i)) {
            result = outputs_.status(i);
            if (result == platform::PwmResult::success)
                result = outputs_.last_operation(i);
        }
        if (!output.format("%s%u", i == 0U ? "" : ",", static_cast<unsigned>(result)))
            return false;
    }
    return output.write("]}");
}
} // namespace newmota

namespace newmota {
#if USE_WS2812
namespace {
using StripPixel = led_strip::Pixel;

std::uint8_t& channel(StripPixel& pixel, std::size_t index) {
    switch (index) {
    case 0U:
        return pixel.r;
    case 1U:
        return pixel.g;
    case 2U:
        return pixel.b;
    default:
        return pixel.w;
    }
}

StripPixel interpolate(StripPixel a, StripPixel b, std::size_t position, std::size_t span) {
    const auto blend = [=](std::uint8_t first, std::uint8_t last) {
        return static_cast<std::uint8_t>(
            static_cast<int>(first) + (static_cast<int>(last) - first) *
                                          static_cast<std::int32_t>(position) /
                                          static_cast<std::int32_t>(span));
    };
    return {blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b), blend(a.w, b.w)};
}

platform::PwmResult strip_result(led_strip::Result result) {
    switch (result) {
    case led_strip::Result::success:
        return platform::PwmResult::success;
    case led_strip::Result::invalid_argument:
        return platform::PwmResult::invalid_request;
    case led_strip::Result::unsupported:
        return platform::PwmResult::unsupported_route;
    case led_strip::Result::busy:
        return platform::PwmResult::resource_unavailable;
    case led_strip::Result::no_memory:
        return platform::PwmResult::resource_unavailable;
    default:
        return platform::PwmResult::native_failure;
    }
}
} // namespace

led_strip::Config LightService::strip_config(const LightSettings& settings) const {
    constexpr led_strip::Order orders[] = {led_strip::Order::grb, led_strip::Order::grb,
        led_strip::Order::rgb, led_strip::Order::rbg, led_strip::Order::brg, led_strip::Order::bgr,
        led_strip::Order::gbr, led_strip::Order::grb};
    // Channel topology, including power endpoint identities, stays fixed until restart.
    return {strip_pin_, settings.strip_pixels, orders[settings.strip_type & 7U],
        settings.strip_reverse, channel_count_ == 4U, (settings.strip_type & 16U) != 0U, true};
}

std::uint8_t LightService::manual_power(
    const LightSettings& settings, const led_strip::Pixel* pixels, const std::size_t count) const {
    std::uint8_t power{};
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t components[] = {pixels[i].r, pixels[i].g, pixels[i].b, pixels[i].w};
        for (std::size_t c = 0; c < channel_count_; ++c)
            if (components[c] != 0U &&
                (separate_ && c >= 3U ? settings.white_dimmer : settings.dimmer) != 0U)
                power |= static_cast<std::uint8_t>(1U << endpoint_for_channel(c));
    }
    return power;
}

platform::PwmResult LightService::write_pixels(
    std::size_t index, const led_strip::Pixel* colors, std::size_t count, bool repeat) {
    if (!available() || !addressable_ || !colors || !count || index >= pixel_count() ||
        count > pixel_count() - index)
        return platform::PwmResult::invalid_request;
    if (last_result_ != platform::PwmResult::success || !led_strip::active(strip_))
        return platform::PwmResult::native_failure;
    // Stage raw intent without touching the retained manual frame or DMA bytes.
    // Entering manual mode has the same clear-on-entry behavior as Scheme 14.
    if (active_scheme_ == 14U)
        strip_frame_ = manual_pixels_;
    else
        strip_frame_ = {};
    for (std::size_t i = 0; i < count; ++i)
        strip_frame_[index + i] = colors[repeat ? 0U : i];
    auto next = intent();
    next.scheme = 14U;
    next.desired_power = manual_power(next, strip_frame_.data(), pixel_count());
    if (settings_->set_light_settings(next) == SettingsUpdateResult::failed)
        return platform::PwmResult::native_failure;
#if USE_POWER_TARGETS
    PowerService::TargetUpdate target_update(power_, desired_on_ != next.desired_power);
#endif
    manual_pixels_ = strip_frame_;
#if USE_POWER_PROVENANCE
    if (power_ && desired_on_ != next.desired_power)
        power_->record_source(PowerOperationSource::light);
#endif
    desired_on_ = next.desired_power;
    active_scheme_ = 14U;
    strip_steps_complete_ = false;
    transition_mask_ = 0U;
    target_ = target();
    for (std::size_t c = 0; c < channel_count_; ++c)
        duration_[channel_indices_[c]] = 0U;
    ++revision_;
    return sample(platform::monotonic_milliseconds(), channel_mask_);
}

led_strip::Result LightService::strip_edit(
    void* context, std::size_t index, led_strip::Pixel color, std::size_t count) {
    auto& light = *static_cast<LightService*>(context);
    const auto result = light.write_pixels(index, &color, count, true);
    return result == platform::PwmResult::success           ? led_strip::Result::success
           : result == platform::PwmResult::invalid_request ? led_strip::Result::invalid_argument
                                                            : led_strip::Result::failed;
}

void LightService::strip_fault(void* context) {
    auto& light = *static_cast<LightService*>(context);
    light.last_result_ = platform::PwmResult::native_failure;
    light.transition_mask_ = 0U;
    ++light.revision_;
}

platform::PwmResult LightService::strip_sample(
    const std::uint32_t now, const std::array<std::uint16_t, 16>& perceptual) {
    const auto& settings = intent();
    const auto count = pixel_count();
    if (!led_strip::active(strip_))
        return platform::PwmResult::native_failure;
    const auto scheme = settings.scheme;
    if (scheme < 5U) {
        StripPixel uniform{};
        for (std::size_t c = 0; c < channel_count_; ++c)
            channel(uniform, c) = static_cast<std::uint8_t>(perceptual[c] / 257U);
        std::fill_n(strip_frame_.begin(), count, uniform);
    } else if (scheme == 14U) {
        std::copy_n(manual_pixels_.begin(), count, strip_frame_.begin());
    } else if (scheme == 5U) {
        std::fill_n(strip_frame_.begin(), count, StripPixel{});
#if USE_TIME
        if (time_ && time_->synchronized()) {
            const auto civil = TimeService::civil_time(time_->snapshot(now).local_seconds);
            std::uint8_t reverse{};
            (void)settings_->option(16U, reverse);
            const auto hand = [&](std::size_t position, std::size_t color_index) {
                const auto width = color_index == 3U ? settings.strip_width[0]
                                                     : settings.strip_width[color_index + 1U];
                if (!width)
                    return;
                position = (position + settings.strip_rotation) % count;
                if (reverse)
                    position = (count - position) % count;
                const auto range = (width + 1U) / 2U;
                const auto& color = settings.strip_clock_colors[color_index];
                const auto add = [&](std::size_t at, std::size_t numerator) {
                    auto& pixel = strip_frame_[at % count];
                    for (std::size_t c = 0; c < channel_count_; ++c)
                        channel(pixel, c) = static_cast<std::uint8_t>(std::min<std::size_t>(
                            255U, channel(pixel, c) +
                                      static_cast<unsigned>(color[c]) * numerator / range));
                };
                add(position, range);
                for (std::size_t distance = 1; distance < range; ++distance) {
                    add(position + distance, range - distance);
                    add(position + count - distance % count, range - distance);
                }
            };
            hand(static_cast<std::size_t>(civil.second) * count / 60U, 0U);
            hand(static_cast<std::size_t>(civil.minute) * count / 60U, 1U);
            hand(((civil.hour % 12U) * 60U + civil.minute) * count / 720U, 2U);
            for (unsigned marker = 0; marker < 12; ++marker)
                hand(marker * count / 12U, 3U);
        }
#endif
    } else {
        constexpr StripPixel incandescent[] = {{255, 140, 20}, {}};
        constexpr StripPixel rgb[] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
        constexpr StripPixel christmas[] = {{255, 0, 0}, {0, 255, 0}};
        constexpr StripPixel hanukkah[] = {{0, 0, 255}, {255, 255, 255}};
        constexpr StripPixel kwanzaa[] = {{255, 0, 0}, {}, {0, 255, 0}};
        constexpr StripPixel rainbow[] = {{255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {0, 255, 0},
            {0, 0, 255}, {128, 0, 255}, {255, 0, 255}};
        constexpr StripPixel fire[] = {{255, 0, 0}, {255, 102, 0}, {255, 192, 0}};
        const StripPixel stairs[] = {{}, {settings.channels[0], settings.channels[1],
                                             settings.channels[2], settings.channels[3]}};
        const StripPixel* colors[] = {
            incandescent, rgb, christmas, hanukkah, kwanzaa, rainbow, fire, stairs};
        constexpr std::size_t sizes[] = {2, 3, 2, 2, 3, 7, 3, 2};
        const auto which = static_cast<std::size_t>(scheme - 6U);
        const auto* palette = colors[which];
        const auto size = sizes[which];
        const auto elapsed = now - effect_started_;
        if (settings.strip_step_pixels != 0U) {
            // Tasmota steps advance every Speed ticks (one tick is 50 ms).
            const auto position = elapsed / (static_cast<std::uint32_t>(settings.speed) * 50U);
            const auto gradient =
                settings.fade ? std::max(2U, unsigned(settings.strip_width[3])) : size;
            strip_steps_complete_ =
                position >= (count - 1U) / settings.strip_step_pixels + gradient - 1U;
            const bool invert = (settings.strip_rotation & 1U) != 0U;
            for (std::size_t i = 0; i < count; ++i) {
                const auto step = i / settings.strip_step_pixels;
                const auto level =
                    position > step ? std::min<std::size_t>(gradient - 1U, position - step) : 0U;
                const auto value = settings.fade
                                       ? interpolate(palette[invert ? 1 : 0],
                                             palette[invert ? 0 : 1], level, gradient - 1U)
                                       : palette[invert ? size - level - 1U : level];
                strip_frame_[(settings.strip_rotation & 2U) ? count - i - 1U : i] = value;
            }
        } else {
            constexpr std::size_t widths[] = {1, 2, 4, 8, 255};
            constexpr std::size_t repeats[] = {8, 6, 4, 2, 1};
            const auto width = settings.strip_width[0];
            const auto period = (static_cast<std::uint32_t>(settings.speed) * 2U - 1U) * 100U;
            const auto offset = elapsed / period;
            if (settings.fade) {
                const auto range = (count + repeats[width] - 1U) / repeats[width];
                const auto grad_range = (range + size - 2U) / (size - 1U);
                const auto color_at = [&](std::size_t at) {
                    const auto within = at % range;
                    auto first = std::min(size - 2U, within / grad_range);
                    auto last = first + 1U;
                    if ((at / range & 1U) != 0U) {
                        first = size - 1U - first;
                        last = size - 1U - last;
                    }
                    return interpolate(
                        palette[first], palette[last], within % grad_range, grad_range);
                };
                auto previous = color_at(offset);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto next = repeats[width] > 1U ? color_at(i + offset + 1U) : previous;
                    strip_frame_[i] = interpolate(previous, next, elapsed % period, period);
                    previous = next;
                }
            } else {
                const bool bars = widths[width] <= count / size;
                for (std::size_t i = 0; i < count; ++i)
                    strip_frame_[i] =
                        palette[bars ? ((i + offset) / widths[width]) % size : offset % size];
            }
        }
    }
    if (channel_count_ == 4U && scheme >= 5U && scheme < 14U && (separate_ || scheme != 5U))
        for (std::size_t i = 0; i < count; ++i)
            strip_frame_[i].w = settings.channels[3];
    // Frame storage is separate from the DMA buffer; native data cannot be edited
    // until drain. Apply output power/brightness and gamma only at this boundary.
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t c = 0; c < 4U; ++c) {
            std::uint16_t component =
                static_cast<std::uint16_t>(channel(strip_frame_[i], c) * 257U);
            if (scheme >= 5U) {
                component = c < channel_count_ ? static_cast<std::uint16_t>(
                                                     static_cast<std::uint32_t>(component) *
                                                     perceptual[channel_indices_[c]] / 65535U)
                                               : 0U;
            }
            if (settings.gamma)
                component = light_color::gamma16(component);
            channel(strip_frame_[i], c) = static_cast<std::uint8_t>((component + 128U) / 257U);
        }
    }
    // Pixels height describes column height; alternate columns run backwards.
    // Swap in place so rendering needs no per-frame allocation.
    if (settings.strip_alternate && settings.strip_height > 1U) {
        const auto height = std::min<std::size_t>(settings.strip_height, count);
        for (std::size_t base = height; base < count; base += 2U * height)
            std::reverse(
                strip_frame_.begin() + base, strip_frame_.begin() + std::min(base + height, count));
    }
    return strip_result(led_strip::frame(strip_, strip_frame_.data(), count));
}
#endif // USE_WS2812

platform::PwmResult LightService::pixel(std::size_t index, led_strip::Pixel& out) const {
#if USE_WS2812
    if (!addressable_ || index >= pixel_count())
        return platform::PwmResult::invalid_request;
    out = manual_pixels_[index];
    return platform::PwmResult::success;
#else
    (void)index;
    (void)out;
    return platform::PwmResult::unsupported_route;
#endif
}

platform::PwmResult LightService::pixels(
    std::size_t index, const led_strip::Pixel* colors, std::size_t count) {
#if USE_WS2812
    return write_pixels(index, colors, count, false);
#else
    (void)index;
    (void)colors;
    (void)count;
    return platform::PwmResult::unsupported_route;
#endif
}

} // namespace newmota

namespace newmota {
std::uint8_t LightService::option(const std::uint16_t index) const {
    std::uint8_t value{};
    if (settings_)
        (void)settings_->option(index, value);
    return value;
}

platform::PwmResult LightService::pixel_type(const std::uint8_t type) {
#if USE_WS2812
    if (!addressable_ || type > 31U)
        return platform::PwmResult::invalid_request;
    auto next = intent();
    const bool topology_changed = ((next.strip_type ^ type) & 8U) != 0U;
    next.strip_type = type;
    const auto result = update(next, false);
    if (result == platform::PwmResult::success && topology_changed)
        settings_->require_pwm_restart();
    return result;
#else
    (void)type;
    return platform::PwmResult::unsupported_route;
#endif
}
} // namespace newmota
