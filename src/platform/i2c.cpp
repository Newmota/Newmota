/*
  i2c.cpp - owned I2C and optional driver support for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "newmota/platform/i2c.h"
#include "newmota/platform/gpio.h"
#include "newmota/platform/platform.h"
#include <array>

namespace newmota::platform::i2c {
namespace {
struct Bus {
    PinId scl;
    PinId sda;
    GpioPull release{GpioPull::none};
    bool ready{};
    bool busy{};
};

std::array<Bus, kBusCount> buses{};

// GPIO direction changes implement open drain on every native GPIO adapter.
// Delays establish a maximum clock rate; scheduling may extend low/high phases.
class Transaction {
  public:
    Bus& bus;
    std::uint32_t started, timeout;
    Result result{Result::success};

    /** @brief Changes one pad to released input or driven low output. */
    bool level(PinId pin, bool high) {
        const auto status =
            high ? gpio_configure_input(pin, bus.release) : gpio_configure_output(pin, false);
        if (status != GpioResult::success)
            result = Result::failed;
        delay_microseconds(5);
        return status == GpioResult::success;
    }

    /** @brief Checks the complete transaction deadline. */
    bool expired() {
        if (monotonic_milliseconds() - started >= timeout)
            result = Result::timeout;
        return result != Result::success;
    }

    /** @brief Releases SCL and waits for bounded clock stretching. */
    bool clock() {
        if (!level(bus.scl, true))
            return false;
        bool high = false;
        do {
            if (gpio_read(bus.scl, high) != GpioResult::success) {
                result = Result::failed;
                return false;
            }
            if (expired())
                return false;
            if (!high)
                delay_microseconds(5);
        } while (!high);
        return true;
    }

    /** @brief Generates START or repeated START. */
    bool start() {
        return level(bus.sda, true) && clock() && level(bus.sda, false) && level(bus.scl, false);
    }

    /** @brief Attempts STOP cleanup while preserving an earlier error. */
    void stop() {
        const auto previous = result;
        // Attempt cleanup even when the operation's deadline has expired.
        result = Result::success;
        (void)level(bus.scl, false);
        (void)level(bus.sda, false);
        (void)clock();
        (void)level(bus.sda, true);
        if (previous != Result::success)
            result = previous;
    }

    /** @brief Shifts one byte and samples the device acknowledgement. */
    bool send(std::uint8_t value) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (!level(bus.sda, (value & 0x80U) != 0) || !clock() || !level(bus.scl, false))
                return false;
            value <<= 1;
        }
        bool ack = true;
        if (!level(bus.sda, true) || !clock())
            return false;
        if (gpio_read(bus.sda, ack) != GpioResult::success) {
            result = Result::failed;
            return false;
        }
        if (!level(bus.scl, false))
            return false;
        if (ack)
            result = Result::nack;
        return !ack;
    }

    /** @brief Samples one byte and acknowledges all but the final byte. */
    bool receive(std::uint8_t& value, bool last) {
        value = 0;
        if (!level(bus.sda, true))
            return false;
        for (unsigned bit = 0; bit < 8; ++bit) {
            bool high = false;
            if (!clock())
                return false;
            if (gpio_read(bus.sda, high) != GpioResult::success) {
                result = Result::failed;
                return false;
            }
            value = static_cast<std::uint8_t>((value << 1) | high);
            if (!level(bus.scl, false))
                return false;
        }
        return level(bus.sda, last) && clock() && level(bus.scl, false) && level(bus.sda, true);
    }
};
} // namespace

/** @brief Reports whether a zero-based bus index exists. */
bool supported(const std::uint8_t bus) {
    return bus < buses.size();
}

/** @brief Reports whether runtime pin configuration activated a bus. */
bool available(const std::uint8_t bus) {
    return supported(bus) && buses[bus].ready;
}

/** @brief Initializes one retained bus with released SDA and SCL. */
Result configure(
    const std::uint8_t index, const PinId scl, const PinId sda, const bool internal_pull_up) {
    if (!supported(index) || !scl.valid() || !sda.valid() || scl.value() == sda.value())
        return Result::invalid_argument;
    auto& bus = buses[index];
    if (bus.busy)
        return Result::busy;
    bus.ready = false;
    if (!gpio_available(scl) || !gpio_available(sda))
        return Result::unavailable;
    const auto release = internal_pull_up ? GpioPull::up : GpioPull::none;
    if (gpio_configure_input(scl, release) != GpioResult::success ||
        gpio_configure_input(sda, release) != GpioResult::success)
        return Result::failed;
    bus.release = release;
    bus.scl = scl;
    bus.sda = sda;
    bus.ready = true;
    return Result::success;
}

/** @brief Runs one bounded atomic probe, write, read or combined transfer. */
Result transfer(const std::uint8_t index, const std::uint8_t address, const std::uint8_t* write,
    const std::size_t write_size, std::uint8_t* read, const std::size_t read_size,
    const std::uint32_t timeout_ms) {
    if (!supported(index) || address < 8 || address > 0x77 || write_size > kMaxTransfer ||
        read_size > kMaxTransfer || (write_size && !write) || (read_size && !read) ||
        timeout_ms == 0 || timeout_ms > 1000)
        return Result::invalid_argument;
    auto& bus = buses[index];
    if (!bus.ready)
        return Result::unavailable;
    if (bus.busy)
        return Result::busy;
    bus.busy = true;
    Transaction operation{bus, monotonic_milliseconds(), timeout_ms};
    bool idle = false;
    bool okay = gpio_read(bus.sda, idle) == GpioResult::success;
    if (!okay)
        operation.result = Result::failed;
    else if (!idle) {
        okay = false;
        operation.result = Result::busy;
    }
    if (okay)
        okay = operation.start();
    if (okay && (write_size || !read_size)) {
        okay = operation.send(static_cast<std::uint8_t>(address << 1));
        for (std::size_t i = 0; okay && i < write_size; ++i)
            okay = operation.send(write[i]);
    }
    if (okay && read_size) {
        if (write_size)
            okay = operation.start();
        if (okay)
            okay = operation.send(static_cast<std::uint8_t>((address << 1) | 1));
        for (std::size_t i = 0; okay && i < read_size; ++i)
            okay = operation.receive(read[i], i + 1 == read_size);
    }
    operation.stop();
    bus.busy = false;
    return operation.result;
}
} // namespace newmota::platform::i2c
