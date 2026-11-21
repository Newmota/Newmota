/*
  energy_configuration.cpp - Typed energy resources and persistent connection identities

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file energy_configuration.cpp
 * @brief Typed energy resources and persistent connection identities.
 */

#include "services/energy/energy_service.h"
#include "support/text.h"
#include "core/hardware/hardware.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/log.h"
#include "services/settings/settings.h"
#if USE_MODBUS_RTU
#include "drivers/modbus/rtu_service.h"
#endif
#if USE_TUYA_MCU_ENERGY
#include "drivers/tuya/tuya_mcu.h"
#endif
#include <array>
#include <algorithm>
#include <string_view>

namespace newmota::energy {
/** @brief Resolves one provider without probing ambiguous I2C addresses or replacing accounts. */
void Service::select_hardware(const Hardware& hardware, const Settings& settings) {
    if (storage_fault_)
        return;
    runtime_pins::EnergyClaimScope claims;

    // Stable unshifted identities from config/hardware/gpio-functions.toml.
    struct Selector {
        std::uint16_t base, driver;
    };

    constexpr Selector selectors[]{
        {97, 2},   // GPIO_CSE7766_RX
        {254, 14}, // GPIO_BL0939_RX
        {170, 41}, // GPIO_BL0940_RX
        {255, 42}, // GPIO_BL0942_RX
        {229, 19}, // GPIO_CSE7761_RX
        {354, 23}, // GPIO_BL0906_RX
        {47, 3},   // GPIO_PZEM004_RX
        {48, 5},   // GPIO_PZEM016_RX
        {49, 6},   // GPIO_PZEM017_RX
        {87, 4},   // GPIO_MCP39F5_RX
        {370, 25}, // GPIO_V9240_RX
        {61, 8},   // GPIO_SDM120_RX
        {63, 10},  // GPIO_SDM630_RX
        {123, 9},  // GPIO_DDS2382_RX
        {125, 11}, // GPIO_DDSU666_RX
        {140, 13}, // GPIO_LE01MR_RX
        {180, 16}, // GPIO_IEM3000_RX
        {186, 17}, // GPIO_WE517_RX
        {221, 18}, // GPIO_SDM72_RX
        {266, 20}, // GPIO_SDM230_RX
        {298, 28}, // GPIO_NRG_MBS_RX
        {84, 1},   // GPIO_HLW_CF
        {85, 34},  // GPIO_HJL_CF
        {108, 35}, // GPIO_ADE7953_IRQ
        {272, 22}, // GPIO_ADE7880_IRQ
        {352, 33}, // GPIO_ADC_VOLTAGE
        {353, 33}, // GPIO_ADC_CURRENT
        {194, 30}, // GPIO_OPTION_A, left as a binding only on an invalid pin
    };
    Configuration selected{};
    bool conflict{};
    const newmota::PinBinding* marker{};
    for (std::size_t i = 0; i < hardware.binding_count(); ++i) {
        const auto* binding = hardware.binding(i);
        const auto base = binding->function >> 5;
        const auto variant = binding->function & 31;
        for (const auto& selector : selectors) {
            if (base != selector.base)
                continue;
            auto id = selector.driver;
            if (base == 194) {
                if (variant == 1)
                    id = 30; // Source Option A2: Dummy.
                else if (variant == 8)
                    id = 27; // Source Option A9: Berry.
                else
                    continue;
            } else if (base == 229)
                id = variant ? 40 : 19;
            else if (base == 108) {
                if (!variant || variant > 5) {
                    AddLog(
                        LogLevel::error, "NRG: excluded ADE7953 model; select a supported variant");
                    return;
                }
                id = 34 + variant;
            }
            if (selected.identity.driver &&
                (selected.identity.driver != id || (id != 33 && id != 22)))
                conflict = true;
            selected.identity.driver = id;
            if (!binding->active()) {
                AddLog(LogLevel::error,
                    "NRG: GPIO%u energy function %u unavailable (driver excluded or invalid route)",
                    unsigned(binding->pin.value()), unsigned(binding->function));
                return;
            }
            marker = binding;
        }
    }
    // Source Option A2 selects Dummy and Option A9 Berry; valid markers resolve to flags.
    constexpr Selector options[]{{1, 30}, {8, 27}};
    for (const auto& option : options) {
        if (!(hardware.option_a() & (std::uint32_t{1} << option.base)))
            continue;
        if (selected.identity.driver)
            conflict = true;
        selected.identity.driver = option.driver;
    }
#if USE_TUYA_MCU_ENERGY
    // As Tasmota's Xnrg32: the running TuyaMCU driver measures once its power is mapped.
    if (tuya::energy_mapped()) {
        if (selected.identity.driver)
            conflict = true;
        selected.identity.driver = 32;
    }
#endif
    auto& retained = provider_;
    if (conflict) {
        AddLog(LogLevel::error, "NRG: multiple energy providers in template; no provider opened");
        return;
    }
    if (!selected.identity.driver) {
        if (retained.configuration.identity.driver)
            AddLog(LogLevel::info,
                "NRG: retained meter offline; select its GPIO functions in Template and Restart");
        return;
    }
    const auto* descriptor = driver(selected.identity.driver);
    if (!descriptor) {
        AddLog(LogLevel::error, "NRG: template requires energy driver %u; enable it in the build",
            unsigned(selected.identity.driver));
        return;
    }
    const auto id = selected.identity.driver;
    // Driver-specific setup below names only drivers this build compiles; the rest folds away.
    selected.channels = built_one_of<33>(id)   ? 0
                        : built_one_of<27>(id) ? 1
                                               : descriptor->maximum_channels;
    if (built_one_of<30>(id)) {
        std::uint8_t phases{};
        (void)settings.option(48, phases);
        if (!phases)
            for (std::uint8_t phase = 1; phase <= 32; ++phase)
                if (hardware.find_function(PinRole::relay, phase))
                    ++phases;
        selected.channels = std::min(phases, descriptor->maximum_channels);
        if (!selected.channels) {
            AddLog(
                LogLevel::error, "NRG: Dummy needs configured Power outputs or SetOption48 phases");
            return;
        }
    }
    selected.options = descriptor->default_options;
    if (built_one_of<14>(id))
        selected.identity.address = 5;
    if (built_one_of<42>(id)) {
        selected.identity.address = 8;
        selected.options.flags =
            marker->function & 7; // Source baud (bits0..1), signed power (bit2).
    }
    if (built_one_of<3>(id))
        selected.identity.address = 1;
    if (built_one_of<35, 36, 37, 38, 39>(id) && marker) {
        selected.identity.bind(PinFunction::interrupt, marker->pin);
    }
    const bool rtu = built_one_of<5, 6, 8, 9, 10, 11, 13, 16, 17, 18, 20, 28>(id);
    std::array<PinId, PhysicalIdentity::kMaximumPins> authorized{};
    std::size_t count{};
    PinId rx{}, tx{}, de{};
    for (std::size_t i = 0; i < hardware.active_count(); ++i) {
        const auto* binding = hardware.function_binding(i);
        PinFunction role = PinFunction::none;
        switch (binding->role) {
        case PinRole::energy_rx:
            role = PinFunction::uart_rx;
            rx = binding->pin;
            break;
        case PinRole::energy_tx:
            role = PinFunction::uart_tx;
            tx = binding->pin;
            break;
        case PinRole::energy_cf:
            role = PinFunction::cf;
            break;
        case PinRole::energy_cf1:
            role = PinFunction::cf1;
            break;
        case PinRole::energy_sel:
            if (rtu)
                de = binding->pin;
            else {
                role = PinFunction::selector;
                selected.options.flags = (binding->modifiers & 1) ? 1 : 0;
            }
            break;
        case PinRole::energy_reset:
            role = PinFunction::reset;
            break;
        case PinRole::energy_irq:
            if (built_one_of<22>(id))
                role = binding->instance == 1 ? PinFunction::interrupt : PinFunction::interrupt2;
            break;
        case PinRole::energy_cs:
            role = binding->instance == 1 ? PinFunction::spi_cs : PinFunction::spi_cs2;
            break;
        case PinRole::energy_voltage:
            role = static_cast<PinFunction>(
                unsigned(PinFunction::adc_voltage1) + binding->instance - 1);
            selected.channels = std::max<std::uint8_t>(selected.channels, binding->instance);
            break;
        case PinRole::energy_current:
            role = static_cast<PinFunction>(
                unsigned(PinFunction::adc_current1) + binding->instance - 1);
            selected.channels = std::max<std::uint8_t>(selected.channels, binding->instance);
            break;
        default:
            if (built_one_of<37, 38, 39>(id)) {
                switch (binding->function >> 5) {
                case 23:
                    role = PinFunction::spi_clock;
                    break;
                case 22:
                    role = PinFunction::spi_mosi;
                    break;
                case 21:
                    role = PinFunction::spi_miso;
                    break;
                default:
                    break;
                }
            }
            break;
        }
        if (role != PinFunction::none || (rtu && binding->pin.value() == de.value()) ||
            (built_one_of<35, 36, 37, 38, 39>(id) && marker && binding == marker)) {
            if (count == authorized.size()) {
                AddLog(LogLevel::error, "NRG: too many energy GPIO bindings");
                return;
            }
            authorized[count++] = binding->pin;
        }
        if (role != PinFunction::none && !rtu && !selected.identity.bind(role, binding->pin))
            return;
    }
    if (built_one_of<22, 35, 36>(id)) {
        const auto* scl = hardware.find_function(PinRole::i2c_scl, 1);
        const auto* second = hardware.find_function(PinRole::i2c_scl, 2);
        const auto bus = scl ? 1 : 2;
        const auto* sda = hardware.find_function(PinRole::i2c_sda, bus);
        if ((!scl && !second) || !sda || (scl && second)) {
            AddLog(
                LogLevel::error, "NRG: explicit metering marker requires one configured I2C bus");
            return;
        }
        selected.identity.bus = bus;
        selected.identity.address = 0x38;
    }
    if (rtu) {
        selected.identity.bus =
            retained.configuration.identity.driver == id ? retained.configuration.identity.bus : 1;
        selected.identity.address = 1;
    }
    if (retained.configuration.identity.driver && retained.configuration.identity.driver != id)
        AddLog(LogLevel::info, "NRG: template names another meter; totals kept, calibration reset");
    if (retained.configuration.identity.driver == id) {
        // Addresses and channel counts may have been changed by driver-specific commands.
        selected.identity.address = retained.configuration.identity.address;
        // The same driver on other pins keeps its settings; only the pins follow the template.
        const auto identity = selected.identity;
        selected = retained.configuration;
        selected.identity = identity;
        // GPIO-defined hardware variants remain authoritative after explicit Restart.
        if (built_one_of<42>(id))
            selected.options.flags = marker->function & 7;
        if (built_one_of<1, 34>(id)) {
            const auto* sel = hardware.find_function(PinRole::energy_sel, 1);
            selected.options.flags = sel && (sel->modifiers & 1) ? 1 : 0;
        }
        if (retained.configuration.options.flags != selected.options.flags)
            dirty_ = true;
        retained.configuration.options = selected.options;
    }
    if (rtu) {
#if USE_MODBUS_RTU
        modbus::BusConfiguration bus{};
        bus.uart.rx = rx;
        bus.uart.tx = tx;
        bus.transmit_enable = de;
        bus.uart.baud = built_one_of<8, 13>(id) ? 2400 : built_one_of<16>(id) ? 19200 : 9600;
        if (built_one_of<17>(id))
            bus.uart.parity = platform::serial::Parity::even;
        if (built_one_of<6>(id))
            bus.uart.stop_bits = 2;
        modbus::BusConfiguration existing{};
        const auto bus_index = static_cast<std::uint8_t>(selected.identity.bus - 1);
        if (modbus::configuration(bus_index, existing)) {
            if (existing.uart.rx.value() != rx.value() || existing.uart.tx.value() != tx.value() ||
                existing.transmit_enable.value() != de.value()) {
                AddLog(LogLevel::error,
                    "NRG: template differs from retained Modbus route; settings preserved");
                return;
            }
            bus = existing; // Preserve explicitly configured real-device framing.
        }
        if (!runtime_pins::authorize_energy(authorized.data(), count) ||
            modbus::configure_bus(bus_index, bus) != modbus::Result::success) {
            AddLog(LogLevel::error,
                "NRG: template Modbus route unavailable; retained settings preserved");
            return;
        }
#else
        return;
#endif
    }
#if USE_MODBUS_ENERGY
    // As in Tasmota, the Modbus meter reads its register map from /modbus.json.
    if (id == 28 && !retained.configuration.identity.driver)
        (void)load_modbus_profile_file();
#endif
    if (!runtime_pins::authorize_energy(authorized.data(), count) ||
        !configure(selected, retained.configuration.identity.driver != id)) {
        AddLog(LogLevel::error,
            "NRG: template energy provider could not open; check pins, UART and enabled driver");
        return;
    }
#if USE_MODBUS_ENERGY
    if (id == 28 && retained.configuration.identity.driver)
        (void)load_modbus_profile_file();
#endif
    AddLog(LogLevel::info, "NRG: template selected %s (%u channels)", descriptor->name,
        unsigned(selected.channels));
}

} // namespace newmota::energy
