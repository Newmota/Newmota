/*
  spi_service.cpp - Portable GPIO SPI read write duplex and retained resource cleanup

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file spi_service.cpp
 * @brief Portable GPIO SPI read write duplex and retained resource cleanup.
 */

#include "drivers/spi/spi_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include <array>

namespace newmota::device_spi {
namespace {
struct BusState {
    BusConfiguration config{};
    std::uint32_t generation{};
    bool occupied{}, faulted{}, busy{};
};

struct Lease {
    std::uint32_t generation{};
    std::uint8_t bus{};
    bool occupied{};
};

struct DeviceState {
    DeviceConfiguration config{};
    Bus lease{};
    std::uint32_t generation{};
    bool occupied{}, faulted{};
};

std::array<BusState, kMaximumBuses> buses{};
std::array<Lease, kMaximumLeases> leases{};
std::array<DeviceState, kMaximumDevices> devices{};
std::uint32_t generation{};

/** @brief A single generation domain scopes both bus-wire and CS claims without collisions. */
runtime_pins::Owner owner(std::uint32_t id) {
    return {runtime_pins::OwnerKind::device_spi, id};
}

Lease* lease(Bus handle) {
    if (handle.slot >= leases.size())
        return nullptr;
    auto& entry = leases[handle.slot];
    return entry.occupied && entry.generation == handle.generation ? &entry : nullptr;
}

DeviceState* device(Device handle) {
    if (handle.slot >= devices.size())
        return nullptr;
    auto& entry = devices[handle.slot];
    return entry.occupied && entry.generation == handle.generation ? &entry : nullptr;
}

bool same(const BusConfiguration& a, const BusConfiguration& b) {
    return a.clock.value() == b.clock.value() && a.mosi.value() == b.mosi.value() &&
           a.miso.value() == b.miso.value();
}

bool output(PinId pin) {
    return platform::gpio_available(pin) &&
           (platform::gpio_capabilities(pin) & capability_mask(PinCapability::output));
}

bool input(PinId pin) {
    return platform::gpio_available(pin) &&
           (platform::gpio_capabilities(pin) & capability_mask(PinCapability::input));
}

bool write_pin(PinId pin, bool high) {
    return platform::gpio_write(pin, high) == platform::GpioResult::success;
}

/** @brief Confirms all pads are inputs before releasing any bus claim. */
bool release_bus(BusState& bus) {
    bool cleaned = true;
    const PinId pins[]{bus.config.clock, bus.config.mosi, bus.config.miso};
    for (const auto pin : pins)
        if (pin.valid() && platform::gpio_configure_input(pin, platform::GpioPull::none) !=
                               platform::GpioResult::success)
            cleaned = false;
    if (!cleaned) {
        bus.faulted = true;
        return false;
    }
    runtime_pins::release(owner(bus.generation));
    bus = {};
    return true;
}
} // namespace

Result open(const BusConfiguration& config, Bus& handle) {
    handle = {};
    if (!config.clock.valid() || (!config.mosi.valid() && !config.miso.valid()) ||
        config.clock.value() == config.mosi.value() ||
        config.clock.value() == config.miso.value() ||
        (config.mosi.valid() && config.mosi.value() == config.miso.value()))
        return Result::invalid_argument;
    if (!output(config.clock) || (config.mosi.valid() && !output(config.mosi)) ||
        (config.miso.valid() && !input(config.miso)))
        return Result::unsupported;
    std::size_t selected = buses.size(), free = buses.size(), lease_slot = leases.size();
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (!buses[i].occupied && free == buses.size())
            free = i;
        if (buses[i].occupied && same(buses[i].config, config)) {
            if (buses[i].faulted || buses[i].busy)
                return Result::busy;
            selected = i;
        }
    }
    for (std::size_t i = 0; i < leases.size(); ++i)
        if (!leases[i].occupied) {
            lease_slot = i;
            break;
        }
    if (lease_slot == leases.size() || generation > UINT32_MAX - 2)
        return Result::busy;
    if (selected == buses.size()) {
        if (free == buses.size())
            return Result::busy;
        selected = free;
        const auto token = ++generation;
        std::array<PinId, 3> pins{};
        std::size_t count{};
        pins[count++] = config.clock;
        if (config.mosi.valid())
            pins[count++] = config.mosi;
        if (config.miso.valid())
            pins[count++] = config.miso;
        if (!runtime_pins::claim(owner(token), pins.data(), count))
            return Result::busy;
        buses[selected] = {config, token, true, false, false};
        const auto lease_id = ++generation;
        leases[lease_slot] = {lease_id, std::uint8_t(selected), true};
        handle = {lease_id, std::uint8_t(lease_slot)};
        const bool configured =
            platform::gpio_configure_output(config.clock, false) == platform::GpioResult::success &&
            (!config.mosi.valid() || platform::gpio_configure_output(config.mosi, false) ==
                                         platform::GpioResult::success) &&
            (!config.miso.valid() ||
                platform::gpio_configure_input(config.miso, platform::GpioPull::none) ==
                    platform::GpioResult::success);
        if (!configured) {
            buses[selected].faulted = true;
            return Result::failed;
        }
    } else {
        const auto token = ++generation;
        leases[lease_slot] = {token, std::uint8_t(selected), true};
        handle = {token, std::uint8_t(lease_slot)};
    }
    return Result::success;
}

Result close(Bus handle) {
    auto* entry = lease(handle);
    if (!entry)
        return Result::success;
    auto& bus = buses[entry->bus];
    if (bus.busy)
        return Result::busy;
    for (const auto& target : devices)
        if (target.occupied && target.lease.slot == handle.slot &&
            target.lease.generation == handle.generation)
            return Result::busy;
    bool shared{};
    for (const auto& other : leases)
        if (&other != entry && other.occupied && other.bus == entry->bus)
            shared = true;
    if (!shared && !release_bus(bus))
        return Result::failed;
    *entry = {};
    return Result::success;
}

Result attach(Bus handle, const DeviceConfiguration& config, Device& output_handle) {
    output_handle = {};
    auto* selected = lease(handle);
    if (!selected || !config.select.valid() || config.mode > 3 || config.frequency_hz < 1000 ||
        config.frequency_hz > 250000 || config.setup_us > 100 || config.hold_us > 100)
        return Result::invalid_argument;
    auto& bus = buses[selected->bus];
    if (bus.faulted || bus.busy)
        return Result::busy;
    if (!output(config.select))
        return Result::unsupported;
    std::size_t slot{};
    while (slot < devices.size() && devices[slot].occupied)
        ++slot;
    if (slot == devices.size() || generation == UINT32_MAX)
        return Result::busy;
    const auto token = ++generation;
    if (!runtime_pins::claim(owner(token), &config.select, 1))
        return Result::busy;
    auto& target = devices[slot];
    target = {config, handle, token, true, false};
    output_handle = {token, std::uint8_t(slot)};
    if (platform::gpio_configure_output(config.select, !config.select_active_high) !=
        platform::GpioResult::success) {
        target.faulted = true;
        return Result::failed;
    }
    return Result::success;
}

Result close(Device handle) {
    auto* target = device(handle);
    if (!target)
        return Result::success;
    const auto* selected = lease(target->lease);
    if (selected && buses[selected->bus].busy)
        return Result::busy;
    if (!write_pin(target->config.select, !target->config.select_active_high) ||
        platform::gpio_configure_input(target->config.select, platform::GpioPull::none) !=
            platform::GpioResult::success) {
        target->faulted = true;
        return Result::failed;
    }
    runtime_pins::release(owner(target->generation));
    *target = {};
    return Result::success;
}

bool active(Device handle) {
    const auto* target = device(handle);
    const auto* selected = target ? lease(target->lease) : nullptr;
    return target && !target->faulted && selected && !buses[selected->bus].faulted;
}

Result transfer(Device handle, const std::uint8_t* tx, std::uint8_t* rx, std::size_t size,
    std::uint32_t timeout_ms) {
    auto* target = device(handle);
    auto* selected = target ? lease(target->lease) : nullptr;
    if (!target || !selected || size > kMaximumTransfer || !timeout_ms || timeout_ms > 100)
        return Result::invalid_argument;
    auto& bus = buses[selected->bus];
    if (bus.busy)
        return Result::busy;
    if (target->faulted || bus.faulted)
        return Result::failed;
    if ((tx && !bus.config.mosi.valid()) || (rx && !bus.config.miso.valid()))
        return Result::invalid_argument;
    if (!size)
        return Result::success;
    bus.busy = true;
    const auto& config = target->config;
    const bool idle = (config.mode & 2) != 0, trailing = (config.mode & 1) != 0;
    const auto half_period = (500000U + config.frequency_hz - 1) / config.frequency_hz;
    const auto deadline = platform::monotonic_milliseconds() + timeout_ms;
    Result result = Result::success;
    if (!write_pin(bus.config.clock, idle) || !write_pin(config.select, config.select_active_high))
        result = Result::failed;
    if (result == Result::success)
        platform::delay_microseconds(config.setup_us);
    for (std::size_t i = 0; i < size && result == Result::success; ++i) {
        const auto sent = tx ? tx[i] : std::uint8_t(255);
        std::uint8_t received{};
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (platform::monotonic_milliseconds() - deadline < 0x80000000U) {
                result = Result::timeout;
                break;
            }
            const auto shift = config.lsb_first ? bit : 7 - bit;
            bool sampled{};
            if (!trailing && bus.config.mosi.valid() &&
                !write_pin(bus.config.mosi, ((sent >> shift) & 1) != 0)) {
                result = Result::failed;
                break;
            }
            platform::delay_microseconds(half_period);
            if (!write_pin(bus.config.clock, !idle)) {
                result = Result::failed;
                break;
            }
            if (trailing && bus.config.mosi.valid() &&
                !write_pin(bus.config.mosi, ((sent >> shift) & 1) != 0)) {
                result = Result::failed;
                break;
            }
            if (!trailing && rx &&
                platform::gpio_read(bus.config.miso, sampled) != platform::GpioResult::success) {
                result = Result::failed;
                break;
            }
            platform::delay_microseconds(half_period);
            if (!write_pin(bus.config.clock, idle)) {
                result = Result::failed;
                break;
            }
            if (trailing && rx &&
                platform::gpio_read(bus.config.miso, sampled) != platform::GpioResult::success) {
                result = Result::failed;
                break;
            }
            if (sampled)
                received |= std::uint8_t(1U << shift);
        }
        if (result == Result::success && rx)
            rx[i] = received;
    }
    if (result == Result::success && platform::monotonic_milliseconds() - deadline < 0x80000000U)
        result = Result::timeout;
    // On every exit, stop clocking and deassert CS. Never release claims on uncertainty.
    const bool stopped = write_pin(bus.config.clock, idle);
    platform::delay_microseconds(config.hold_us);
    const bool deselected = write_pin(config.select, !config.select_active_high);
    if (!stopped || !deselected || result == Result::failed) {
        bus.faulted = true;
        target->faulted = true;
        result = Result::failed;
    }
    bus.busy = false;
    return result;
}
} // namespace newmota::device_spi
