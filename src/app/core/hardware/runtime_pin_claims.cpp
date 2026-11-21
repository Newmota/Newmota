/*
  runtime_pin_claims.cpp - Generation-safe runtime GPIO pin claims

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file runtime_pin_claims.cpp
 * @brief Tracks exclusive GPIO ownership with generation-safe runtime identities.
 */
#include "core/hardware/runtime_pin_claims.h"
#if USE_TUYA_MCU
#include "gpio_ids.h"
#endif
#include "newmota/log.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"

namespace newmota::runtime_pins {
namespace {
const Hardware* hardware{};
std::uint32_t energy_generation{};
std::array<PinId, 8> energy_pins{};
std::size_t energy_pin_count{};
bool energy_claim_access{};

/** @brief Allows delegated transport owners to acquire only the selected resolved roles. */
bool energy_authorized(Owner token, PinId pin) {
    if (!energy_claim_access && owner(pin) != token)
        return false;
    bool selected{};
    for (std::size_t i = 0; i < energy_pin_count; ++i)
        selected |= energy_pins[i].value() == pin.value();
    const auto* binding = hardware ? hardware->find_pin(pin) : nullptr;
    if (!selected || !binding)
        return false;
    switch (token.kind) {
    case OwnerKind::serial:
        return binding->role == PinRole::energy_rx || binding->role == PinRole::energy_tx ||
               binding->role == PinRole::energy_sel;
    case OwnerKind::edge_capture:
        return binding->role == PinRole::energy_cf || binding->role == PinRole::energy_cf1 ||
               binding->role == PinRole::energy_irq;
    case OwnerKind::energy:
        return binding->role == PinRole::energy_sel || binding->role == PinRole::energy_reset ||
               binding->role == PinRole::energy_irq;
    case OwnerKind::device_spi:
        return binding->role == PinRole::none || binding->role == PinRole::energy_cs;
    case OwnerKind::adc:
        return binding->role == PinRole::energy_voltage || binding->role == PinRole::energy_current;
    default:
        return false;
    }
}

/** @brief Lets Berry's GPIO owner take an active Buzzer template pin, which no native driver
 * owns.
 */
bool script_authorized(Owner token, PinId pin) {
    const auto* binding = hardware ? hardware->find_pin(pin) : nullptr;
    return token == kGpio && binding && binding->role == PinRole::buzzer && binding->active();
}

struct Claim {
    PinId pin{};
    Owner owner{};
};

std::array<Claim, kMaximumHardwarePins> claims{};

/** @brief Records one claim; a console pin stops the console first, so the configuration wins. */
void record(Claim& slot, PinId pin, Owner token) {
    if (platform::gpio_console_pin(pin) && platform::release_console())
        AddLog(LogLevel::info, "CMD: serial console off, GPIO%u claimed",
            static_cast<unsigned int>(pin.value()));
    slot = {pin, token};
}
} // namespace

EnergyClaimScope::EnergyClaimScope() : previous_(energy_claim_access) {
    energy_claim_access = true;
}

EnergyClaimScope::~EnergyClaimScope() {
    energy_claim_access = previous_;
}

Owner allocate_energy_owner() {
    if (energy_generation == UINT32_MAX)
        return {};
    return {OwnerKind::energy, ++energy_generation};
}

bool authorize_energy(const PinId* pins, std::size_t count) {
    if (count > energy_pins.size() || (count && !pins))
        return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!hardware || !hardware->find_pin(pins[i]))
            return false;
    energy_pin_count = count;
    for (std::size_t i = 0; i < count; ++i)
        energy_pins[i] = pins[i];
    return true;
}

/** @brief Sets the resolved hardware reservation view without clearing retained claims. */
void initialize(const Hardware* view) {
    hardware = view;
}

/** @brief Reserves all configured bindings, including failed or inactive assignments. */
bool configured(PinId pin) {
    if (!hardware)
        return true;
    for (std::size_t i = 0; i < hardware->binding_count(); ++i)
        if (hardware->binding(i)->pin.value() == pin.value())
            return true;
    return false;
}

/** @brief Finds the retained runtime owner of a physical pad. */
Owner owner(PinId pin) {
    for (const auto& c : claims)
        if (c.owner && c.pin.value() == pin.value())
            return c.owner;
    return {};
}

/** @brief Preflights every requested pad before atomically acquiring the complete set. */
bool claim(Owner token, const PinId* pins, std::size_t count) {
    if (!token || !pins || !count || count > claims.size())
        return false;
    std::size_t free{}, needed{};
    for (const auto& c : claims)
        if (!c.owner)
            ++free;
    for (std::size_t i = 0; i < count; ++i) {
        if (!pins[i].valid() || !platform::gpio_available(pins[i]) ||
            (configured(pins[i]) && !energy_authorized(token, pins[i]) &&
                !script_authorized(token, pins[i])))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (pins[i].value() == pins[j].value())
                return false;
        const auto current = owner(pins[i]);
        if (current && current != token)
            return false;
        if (!current)
            ++needed;
    }
    if (needed > free)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (owner(pins[i]))
            continue;
        for (auto& c : claims)
            if (!c.owner) {
                record(c, pins[i], token);
                break;
            }
    }
    return true;
}

/** @brief Claims descriptor-selected pins while allowing its authorized bindings. */
bool claim_display(Owner token, const PinId* pins, std::size_t count, const PinId* authorized,
    std::size_t authorized_count) {
    if (!token || token.kind != OwnerKind::display || !pins || !count || count > claims.size() ||
        authorized_count > count || (authorized_count != 0U && !authorized))
        return false;

    for (std::size_t i = 0; i < authorized_count; ++i) {
        if (!authorized[i].valid())
            return false;
        bool requested{};
        for (std::size_t j = 0; j < count; ++j)
            requested |= pins[j].value() == authorized[i].value();
        if (!requested)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (authorized[j].value() == authorized[i].value())
                return false;
        std::size_t bindings{};
        for (std::size_t j = 0; hardware && j < hardware->binding_count(); ++j)
            bindings += hardware->binding(j)->pin.value() == authorized[i].value();
        if (bindings != 1U || !hardware->find_pin(authorized[i]))
            return false;
    }

    std::size_t free{}, needed{};
    for (const auto& c : claims)
        if (!c.owner)
            ++free;
    for (std::size_t i = 0; i < count; ++i) {
        if (!pins[i].valid() || !platform::gpio_available(pins[i]))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (pins[i].value() == pins[j].value())
                return false;
        bool authorized_pin{};
        for (std::size_t j = 0; j < authorized_count; ++j)
            authorized_pin |= pins[i].value() == authorized[j].value();
        if (configured(pins[i]) != authorized_pin)
            return false;
        const auto current = owner(pins[i]);
        if (current && current != token)
            return false;
        if (!current)
            ++needed;
    }
    if (needed > free)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (owner(pins[i]))
            continue;
        for (auto& c : claims)
            if (!c.owner) {
                record(c, pins[i], token);
                break;
            }
    }
    return true;
}

bool claim_i2s(Owner token, const PinId* pins, const PinRole* roles, std::size_t count) {
    if (!token || token.kind != OwnerKind::audio || !hardware || !pins || !roles || !count ||
        count > claims.size())
        return false;
    std::size_t free{}, needed{};
    for (const auto& c : claims)
        if (!c.owner)
            ++free;
    for (std::size_t i = 0; i < count; ++i) {
        if (!pins[i].valid() || !platform::gpio_available(pins[i]) ||
            roles[i] < PinRole::i2s_dout || roles[i] > PinRole::i2s_mclk)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (pins[j].value() == pins[i].value())
                return false;
        if (configured(pins[i])) {
            const auto* binding = hardware->find_pin(pins[i]);
            if (!binding || !binding->active() || binding->role != roles[i])
                return false;
            std::size_t matches{};
            for (std::size_t j = 0; j < hardware->binding_count(); ++j)
                matches += hardware->binding(j)->pin.value() == pins[i].value();
            if (matches != 1)
                return false;
        }
        const auto current = owner(pins[i]);
        if (current && current != token)
            return false;
        needed += !current;
    }
    if (needed > free)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (owner(pins[i]))
            continue;
        for (auto& c : claims)
            if (!c.owner) {
                record(c, pins[i], token);
                break;
            }
    }
    return true;
}

bool claim_gps_serial(Owner token, PinId rx, PinId tx, std::uint32_t baud) {
    if (!token || token.kind != OwnerKind::serial || !hardware || !rx.valid() || !tx.valid() ||
        rx.value() == tx.value())
        return false;
    const PinId pins[]{rx, tx};
    const PinRole roles[]{PinRole::gps_rx, PinRole::gps_tx};
    std::size_t free{};
    for (const auto& c : claims)
        free += !c.owner;
    if (free < 2)
        return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* b = hardware->find_pin(pins[i]);
        if (!b || !b->active() || b->role != roles[i] || owner(pins[i]) ||
            !platform::gpio_available(pins[i]))
            return false;
        if (i == 0 && (b->instance < 1 || b->instance > 3 || baud != (9600U << (b->instance - 1))))
            return false;
        std::size_t matches{};
        for (std::size_t j = 0; j < hardware->binding_count(); ++j)
            matches += hardware->binding(j)->pin.value() == pins[i].value();
        if (matches != 1)
            return false;
    }
    for (const auto pin : pins)
        for (auto& c : claims)
            if (!c.owner) {
                record(c, pin, token);
                break;
            }
    return true;
}

#if USE_TUYA_MCU
bool claim_tuya_serial(Owner token, PinId rx, PinId tx) {
    namespace gpio = generated::gpio;
    if (!token || token.kind != OwnerKind::serial || !hardware || !rx.valid() || !tx.valid() ||
        rx.value() == tx.value())
        return false;
    const PinId pins[]{rx, tx};
    const std::uint16_t bases[]{gpio::kGpioTuyaRxBase, gpio::kGpioTuyaTxBase};
    std::size_t free{};
    for (const auto& c : claims)
        free += !c.owner;
    if (free < 2)
        return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* b = hardware->find_pin(pins[i]);
        if (!b || !b->active() || (b->function >> gpio::kEncodingShift) != bases[i] ||
            owner(pins[i]) || !platform::gpio_available(pins[i]))
            return false;
        std::size_t matches{};
        for (std::size_t j = 0; j < hardware->binding_count(); ++j)
            matches += hardware->binding(j)->pin.value() == pins[i].value();
        if (matches != 1)
            return false;
    }
    for (const auto pin : pins)
        for (auto& c : claims)
            if (!c.owner) {
                record(c, pin, token);
                break;
            }
    return true;
}
#endif

bool claim_led_strip(Owner token, PinId pin) {
    if (!token || token.kind != OwnerKind::led_strip)
        return false;
    if (!configured(pin))
        return claim(token, &pin, 1);
    if (!hardware || !platform::gpio_available(pin))
        return false;
    const auto* binding = hardware->find_pin(pin);
    if (!binding || binding->role != PinRole::led_strip || !binding->active())
        return false;
    std::size_t count{};
    for (std::size_t i = 0; i < hardware->binding_count(); ++i)
        if (hardware->binding(i)->pin.value() == pin.value())
            ++count;
    if (count != 1 || owner(pin))
        return false;
    for (auto& c : claims)
        if (!c.owner) {
            record(c, pin, token);
            return true;
        }
    return false;
}

/** @brief Releases only claims belonging to the supplied owner. */
void release(Owner token) {
    for (auto& c : claims)
        if (c.owner == token)
            c = {};
}

/** @brief Releases only claims belonging to the supplied owner. */
void release(Owner token, PinId pin) {
    for (auto& c : claims)
        if (c.owner == token && c.pin.value() == pin.value())
            c = {};
}
} // namespace newmota::runtime_pins
