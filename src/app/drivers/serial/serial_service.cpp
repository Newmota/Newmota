/*
  serial_service.cpp - Shared serial port ownership and polling service

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file serial_service.cpp
 * @brief Owns shared native UART ports and advances nonblocking serial transfers.
 */

#include "drivers/serial/serial_service.h"
#include "core/hardware/runtime_pin_claims.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include <array>
#include <limits>

namespace newmota::serial {
namespace native = platform::serial;

namespace {
struct State {
    std::uint32_t generation{};
    native::Port port{};
    Config config{};
    PinId tx_en{}, rx_en{};
    bool rx_active{};
    bool active{}, faulted{}, transmitting{}, closing{}, burst_open{};
    bool suspended{}, break_low{}, break_reserved{}, break_cleanup_uncertain{};
    std::uint32_t break_at{}, break_idle{};
};

std::array<State, kPortCount> ports{};
std::uint32_t generation{1};

constexpr runtime_pins::Owner pin_owner(const std::uint32_t instance) {
    return {runtime_pins::OwnerKind::serial, instance};
}

/** @brief Resolves a current generation to its healthy retained port state. */
State* retained_state(Handle h) {
    if (h.slot >= ports.size())
        return nullptr;
    auto& s = ports[h.slot];
    return s.active && s.generation == h.generation && !s.faulted && !s.closing ? &s : nullptr;
}

State* state(Handle h) {
    auto* value = retained_state(h);
    return value && !value->suspended ? value : nullptr;
}

bool controller_suspended() {
    for (const auto& value : ports)
        if (value.active && (value.suspended || value.break_reserved))
            return true;
    return false;
}

/** @brief Lowers RS-485 enable only after FIFO and final stop-bit drain. */
Result finish(State& s) {
    if (s.transmitting && !s.burst_open && native::tx_idle(s.port)) {
        if (s.tx_en.valid() &&
            platform::gpio_write(s.tx_en, false) != platform::GpioResult::success) {
            s.faulted = true;
            return Result::failed;
        }
        if (s.rx_en.valid() &&
            platform::gpio_write(s.rx_en, s.rx_active) != platform::GpioResult::success) {
            s.faulted = true;
            return Result::failed;
        }
        s.transmitting = false;
    }
    return Result::success;
}
} // namespace

/** @brief Validates and opens an exclusively owned native UART port. */
enum class Claim : std::uint8_t { plain, gps, tuya };

static Result open_port(const Config& c, Handle& h, const Claim claim) {
    h = {};
    if (controller_suspended())
        return Result::busy;
    if ((!c.rx.valid() && !c.tx.valid()) || (c.rx.valid() && c.rx.value() == c.tx.value()) ||
        !c.baud || c.baud > 4000000 || c.data_bits < 5 || c.data_bits > 8 || c.stop_bits < 1 ||
        c.stop_bits > 2 || unsigned(c.parity) > 2 || !c.rx_capacity ||
        c.rx_capacity > native::kMaximumRxCapacity)
        return Result::invalid_argument;
    std::size_t index{};
    while (index < ports.size() && (ports[index].active || ports[index].faulted))
        ++index;
    // Berry handles store generations in a positive signed 32-bit integer.
    if (index == ports.size() ||
        generation >= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return Result::busy;
    const auto token = ++generation;
    PinId pins[2]{};
    std::size_t count{};
    if (c.rx.valid())
        pins[count++] = c.rx;
    if (c.tx.valid())
        pins[count++] = c.tx;
    bool claimed = false;
    switch (claim) {
    case Claim::gps:
        claimed = runtime_pins::claim_gps_serial(pin_owner(token), c.rx, c.tx, c.baud);
        break;
#if USE_TUYA_MCU
    case Claim::tuya:
        claimed = runtime_pins::claim_tuya_serial(pin_owner(token), c.rx, c.tx);
        break;
#endif
    default:
        claimed = runtime_pins::claim(pin_owner(token), pins, count);
        break;
    }
    if (!claimed)
        return Result::busy;
    native::Port port{};
    const auto result = native::open(c, port);
    if (result != Result::success) {
        if (result == Result::failed)
            ports[index].faulted = true;
        else
            runtime_pins::release(pin_owner(token));
        return result;
    }
    ports[index] = {};
    ports[index].generation = token;
    ports[index].port = port;
    ports[index].config = c;
    ports[index].active = true;
    h = {token, static_cast<std::uint8_t>(index)};
    return Result::success;
}

Result open(const Config& c, Handle& h) {
    return open_port(c, h, Claim::plain);
}

#if USE_TUYA_MCU
Result open_tuya(const Config& c, Handle& h) {
    if (!c.rx.valid() || !c.tx.valid())
        return Result::invalid_argument;
    return open_port(c, h, Claim::tuya);
}
#endif

Result open_gps(const Config& c, Handle& h) {
    if (!c.rx.valid() || !c.tx.valid() || c.data_bits != 8 || c.stop_bits != 1 ||
        c.parity != native::Parity::none || c.inverted)
        return Result::invalid_argument;
    return open_port(c, h, Claim::gps);
}

Result reconfigure(Handle handle, const Config& configuration) {
    if (controller_suspended())
        return Result::busy;
    auto* s = state(handle);
    if (!s)
        return Result::invalid_argument;
    if (configuration.rx.value() != s->config.rx.value() ||
        configuration.tx.value() != s->config.tx.value() || !configuration.baud ||
        configuration.baud > 4000000 || configuration.data_bits < 5 ||
        configuration.data_bits > 8 || configuration.stop_bits < 1 || configuration.stop_bits > 2 ||
        unsigned(configuration.parity) > 2 || !configuration.rx_capacity ||
        configuration.rx_capacity > native::kMaximumRxCapacity)
        return Result::invalid_argument;
    if (s->burst_open || s->transmitting || !native::tx_idle(s->port))
        return Result::busy;
    if (native::close(s->port) != Result::success) {
        s->faulted = true;
        return Result::failed;
    }
    native::Port replacement{};
    const auto result = native::open(configuration, replacement);
    if (result == Result::success) {
        s->port = replacement;
        s->config = configuration;
        return Result::success;
    }
    if (result == Result::failed || native::open(s->config, replacement) != Result::success) {
        s->faulted = true;
        return Result::failed;
    }
    s->port = replacement;
    return result;
}

Result request_break(Handle handle, std::uint32_t low_ms, std::uint32_t idle_ms) {
    if (break_pending(handle))
        return Result::busy;
    auto* value = state(handle);
    if (!value || !value->config.tx.valid() || !low_ms || low_ms > 1000 || !idle_ms ||
        idle_ms > 1000)
        return Result::invalid_argument;
    if (value->tx_en.valid() || value->rx_en.valid())
        return Result::unsupported;
    if (controller_suspended() || value->burst_open || !native::tx_idle(value->port))
        return Result::busy;
    value->break_reserved = true;
    if (finish(*value) != Result::success || native::close(value->port) != Result::success) {
        value->faulted = true;
        return Result::failed;
    }
    value->suspended = true;
    if (platform::gpio_configure_output(value->config.tx, false) != platform::GpioResult::success) {
        value->faulted = true;
        return Result::failed;
    }
    value->break_low = true;
    value->break_at = platform::monotonic_milliseconds() + low_ms;
    value->break_idle = idle_ms;
    return Result::success;
}

bool break_pending(Handle handle) {
    const auto* value = retained_state(handle);
    return value && value->suspended;
}

/** @brief Defers collection cleanup until IRQ/native TX completion. */
Result request_close(Handle h) {
    if (h.slot >= ports.size())
        return Result::invalid_argument;
    auto& s = ports[h.slot];
    if (!s.active || s.generation != h.generation)
        return Result::success;
    s.burst_open = false;
    s.closing = true;
    return Result::success;
}

/** @brief Rejects closed, quarantined, and stale-generation handles. */
bool active(Handle h) {
    return retained_state(h) != nullptr;
}

bool any_active() {
    for (const auto& s : ports)
        if (s.active)
            return true;
    return false;
}

/** @brief Drains or safely stops native ownership before releasing pad claims. */
Result close(Handle h) {
    if (h.slot >= ports.size())
        return Result::invalid_argument;
    auto& s = ports[h.slot];
    if (!s.active || s.generation != h.generation)
        return Result::success;
    if (s.break_cleanup_uncertain)
        return Result::failed; // A restore did not identify safe native teardown: retain claims.
    if (s.burst_open)
        return Result::busy;
    if (s.suspended) {
        for (const auto pin : {s.config.rx, s.config.tx})
            if (pin.valid() && platform::gpio_configure_input(pin, platform::GpioPull::none) !=
                                   platform::GpioResult::success) {
                s.faulted = true;
                return Result::failed;
            }
    } else {
        const auto deadline = platform::monotonic_milliseconds() + 25;
        while (!native::tx_idle(s.port)) {
            if (platform::monotonic_milliseconds() - deadline < 0x80000000U)
                return Result::timeout;
            platform::sleep_for_milliseconds(1);
        }
        // Close is also allowed for quarantined states to attempt safe native cleanup.
        const auto result = native::close(s.port);
        if (result != Result::success) {
            s.faulted = true;
            return result;
        }
    }
    if (s.tx_en.valid() && platform::gpio_write(s.tx_en, false) != platform::GpioResult::success) {
        s.faulted = true;
        return Result::failed;
    }
    if (s.tx_en.valid() && platform::gpio_configure_input(s.tx_en, platform::GpioPull::none) !=
                               platform::GpioResult::success) {
        s.faulted = true;
        return Result::failed;
    }
    if (s.rx_en.valid() &&
        (platform::gpio_write(s.rx_en, !s.rx_active) != platform::GpioResult::success ||
            platform::gpio_configure_input(s.rx_en, platform::GpioPull::none) !=
                platform::GpioResult::success)) {
        s.faulted = true;
        return Result::failed;
    }
    runtime_pins::release(pin_owner(s.generation));
    s = {};
    return Result::success;
}

/** @brief Returns the currently buffered RX byte count without waiting. */
std::size_t available(Handle h) {
    auto* s = state(h);
    return s ? native::available(s->port) : 0;
}

/** @brief Peeks or consumes currently buffered bytes without waiting for input. */
Transfer read(Handle h, std::uint8_t* data, std::size_t size, bool peek) {
    auto* s = state(h);
    if (!s || (!data && size))
        return {Result::invalid_argument, 0};
    return native::read(s->port, data, size, peek);
}

/** @brief Opens one explicit frame so inter-iteration FIFO drains do not toggle DE. */
Result begin_tx(Handle handle) {
    auto* s = state(handle);
    if (!s || !s->config.tx.valid())
        return Result::invalid_argument;
    if (finish(*s) != Result::success)
        return Result::failed;
    if (s->burst_open || s->transmitting || !native::tx_idle(s->port))
        return Result::busy;
    if (s->rx_en.valid() &&
        platform::gpio_write(s->rx_en, !s->rx_active) != platform::GpioResult::success) {
        s->faulted = true;
        return Result::failed;
    }
    if (s->tx_en.valid() && platform::gpio_write(s->tx_en, true) != platform::GpioResult::success) {
        s->faulted = true;
        return Result::failed;
    }
    s->burst_open = true;
    s->transmitting = true;
    return Result::success;
}

/** @brief Releases the explicit frame hold while retaining DE until wire completion. */
Result end_tx(Handle handle) {
    auto* s = state(handle);
    if (!s || !s->burst_open)
        return Result::invalid_argument;
    s->burst_open = false;
    return finish(*s);
}

/** @brief Accepts a bounded FIFO burst and reports the accepted byte count. */
Transfer write_some(Handle h, const std::uint8_t* data, std::size_t size) {
    auto* s = state(h);
    if (!s || !s->config.tx.valid() || (!data && size))
        return {Result::invalid_argument, 0};
    if (!size)
        return {};
    if (s->rx_en.valid() && !s->transmitting &&
        platform::gpio_write(s->rx_en, !s->rx_active) != platform::GpioResult::success) {
        s->faulted = true;
        return {Result::failed, 0};
    }
    if (s->tx_en.valid() && !s->transmitting &&
        platform::gpio_write(s->tx_en, true) != platform::GpioResult::success) {
        s->faulted = true;
        return {Result::failed, 0};
    }
    s->transmitting = true;
    const auto result = native::write_some(s->port, data, size);
    if (finish(*s) != Result::success)
        return {Result::failed, result.count};
    return result;
}

/** @brief Claims, configures, or releases an idle RS-485 enable pad. */
Result config_tx_en(Handle h, PinId pin) {
    auto* s = state(h);
    if (!s || !s->config.tx.valid())
        return Result::invalid_argument;
    if (!native::tx_idle(s->port) || s->transmitting)
        return Result::busy;
    if (s->tx_en.value() == pin.value())
        return Result::success;
    if (!pin.valid()) {
        if (platform::gpio_configure_input(s->tx_en, platform::GpioPull::none) !=
            platform::GpioResult::success) {
            s->faulted = true;
            return Result::failed;
        }
        runtime_pins::release(pin_owner(s->generation), s->tx_en);
        s->tx_en = {};
        return Result::success;
    }
    // Replacing DE adds cleanup ambiguity; close/reopen before selecting another pad.
    if (s->tx_en.valid())
        return Result::busy;
    if (pin.value() == s->config.rx.value() || pin.value() == s->config.tx.value() ||
        pin.value() == s->rx_en.value())
        return Result::invalid_argument;
    if (!(platform::gpio_capabilities(pin) & capability_mask(PinCapability::output)))
        return Result::unsupported;
    if (!runtime_pins::claim(pin_owner(s->generation), &pin, 1))
        return Result::busy;
    const auto result = platform::gpio_configure_output(pin, false);
    if (result != platform::GpioResult::success) {
        if (result == platform::GpioResult::failed) {
            s->tx_en = pin;
            s->faulted = true;
        } else
            runtime_pins::release(pin_owner(s->generation), pin);
        return Result::failed;
    }
    s->tx_en = pin;
    return Result::success;
}

/** @brief Owns receiver-enable independently of DE; failure retains uncertain pad claims. */
Result config_rx_en(Handle handle, PinId pin, bool active_level) {
    auto* s = state(handle);
    if (!s || !s->config.rx.valid())
        return Result::invalid_argument;
    if (s->transmitting || s->burst_open || !native::tx_idle(s->port))
        return Result::busy;
    if (s->rx_en.value() == pin.value() && (!pin.valid() || s->rx_active == active_level))
        return Result::success;
    if (s->rx_en.valid())
        return Result::busy; // Close before changing RE; never abandon an uncertain output.
    if (!pin.valid())
        return Result::success;
    if (pin.value() == s->config.rx.value() || pin.value() == s->config.tx.value() ||
        pin.value() == s->tx_en.value())
        return Result::invalid_argument;
    if (!(platform::gpio_capabilities(pin) & capability_mask(PinCapability::output)))
        return Result::unsupported;
    if (!runtime_pins::claim(pin_owner(s->generation), &pin, 1))
        return Result::busy;
    const auto result = platform::gpio_configure_output(pin, active_level);
    if (result != platform::GpioResult::success) {
        if (result == platform::GpioResult::failed) {
            s->rx_en = pin;
            s->rx_active = active_level;
            s->faulted = true;
        } else
            runtime_pins::release(pin_owner(s->generation), pin);
        return Result::failed;
    }
    s->rx_en = pin;
    s->rx_active = active_level;
    return Result::success;
}

/** @brief Observes native drain without touching the explicit burst hold or DE output. */
bool transmitter_empty(Handle handle) {
    auto* s = state(handle);
    return s && native::tx_idle(s->port);
}

/** @brief Requires transmitter drain through the final stop bit. */
bool tx_idle(Handle h) {
    auto* s = state(h);
    return s && !s->burst_open && finish(*s) == Result::success && native::tx_idle(s->port);
}

/** @brief Waits for wire drain under a wrapping millisecond deadline. */
Result drain(Handle h, std::uint32_t deadline) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    if (s->burst_open)
        return Result::busy;
    while (!native::tx_idle(s->port)) {
        if (platform::monotonic_milliseconds() - deadline < 0x80000000U)
            return Result::timeout;
        platform::sleep_for_milliseconds(1);
    }
    return finish(*s);
}

/** @brief Discards queued RX bytes without changing port configuration. */
Result discard_rx(Handle h) {
    auto* s = state(h);
    if (!s)
        return Result::invalid_argument;
    native::discard_rx(s->port);
    return Result::success;
}

/** @brief Returns cumulative dropped-byte and native-error counters. */
Status status(Handle h) {
    auto* s = state(h);
    return s ? native::status(s->port) : Status{};
}

/** @brief Completes pending RS-485 enable transitions from the application loop. */
void poll() {
    for (std::size_t i = 0; i < ports.size(); ++i) {
        auto& s = ports[i];
        if (!s.active || s.faulted)
            continue;
        if (s.suspended) {
            if (s.closing) {
                (void)close({s.generation, static_cast<std::uint8_t>(i)});
                continue;
            }
            if (platform::monotonic_milliseconds() - s.break_at >= 0x80000000U)
                continue;
            if (s.break_low) {
                if (platform::gpio_write(s.config.tx, true) != platform::GpioResult::success) {
                    s.faulted = true;
                    continue;
                }
                s.break_low = false;
                s.break_at = platform::monotonic_milliseconds() + s.break_idle;
            } else {
                native::Port restored{};
                const auto result = native::open(s.config, restored);
                if (result != Result::success) {
                    // A failed native open may have touched hardware: never release its pads.
                    if (result == Result::failed)
                        s.break_cleanup_uncertain = true;
                    s.faulted = true;
                    continue;
                }
                s.port = restored;
                s.suspended = false;
                s.break_reserved = false;
            }
            continue;
        }
        if (s.closing && native::tx_idle(s.port))
            (void)close({s.generation, static_cast<std::uint8_t>(i)});
        else
            (void)finish(s);
    }
}
} // namespace newmota::serial
