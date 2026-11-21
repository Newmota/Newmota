/*
  spi_host.cpp - ESP-IDF SPI controller owner for Newmota

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file spi_host.cpp
 * @brief Reference-counted SPI bus initialization per GPSPI host.
 */

#include "spi_host.h"

#include "soc/soc_caps.h"

#include <array>
#include <cstddef>

namespace newmota::platform::spi_host {
namespace {

/** @brief Bus state of one host; SPI1 belongs to the flash and is never leased. */
struct HostState final {
    spi_bus_config_t bus{};
    std::uint8_t users{};
    bool exclusive{};
};

std::array<HostState, SOC_SPI_PERIPH_NUM> hosts{};

/** @brief Returns the state of a leasable GPSPI host, or nullptr. */
[[nodiscard]] HostState* state(const spi_host_device_t host) {
    const auto index = static_cast<std::size_t>(host);
    if (host < SPI2_HOST || index >= hosts.size()) {
        return nullptr;
    }
    return &hosts[index];
}

/** @brief Compares the signals and mode flags that make two users share one wire. */
[[nodiscard]] bool same_wiring(const spi_bus_config_t& left, const spi_bus_config_t& right) {
    return left.sclk_io_num == right.sclk_io_num && left.mosi_io_num == right.mosi_io_num &&
           left.miso_io_num == right.miso_io_num && left.quadwp_io_num == right.quadwp_io_num &&
           left.quadhd_io_num == right.quadhd_io_num && left.flags == right.flags;
}

[[nodiscard]] Result native(const esp_err_t error) {
    switch (error) {
    case ESP_OK:
        return Result::success;
    case ESP_ERR_NO_MEM:
        return Result::no_memory;
    case ESP_ERR_INVALID_STATE:
        return Result::busy; // Initialized outside the owner.
    case ESP_ERR_NOT_SUPPORTED:
        return Result::unsupported;
    default:
        return Result::failed;
    }
}

} // namespace

Result acquire(
    const spi_host_device_t host, const spi_bus_config_t& bus, const Use use, Lease& lease) {
    auto* const current = state(host);
    if (current == nullptr) {
        return Result::unsupported;
    }
    if (lease.held_) {
        return Result::failed;
    }
    const bool exclusive = use == Use::exclusive;
    if (!exclusive && bus.max_transfer_sz > kSharedTransferBytes) {
        return Result::conflict;
    }
    if (current->users != 0U) {
        if (exclusive || current->exclusive) {
            return Result::busy;
        }
        if (!same_wiring(current->bus, bus)) {
            return Result::conflict;
        }
        if (current->users == UINT8_MAX) {
            return Result::busy;
        }
    } else {
        spi_bus_config_t configuration = bus;
        if (!exclusive) {
            configuration.max_transfer_sz = kSharedTransferBytes;
        }
        const auto result = native(spi_bus_initialize(host, &configuration, SPI_DMA_CH_AUTO));
        if (result != Result::success) {
            return result;
        }
        current->bus = configuration;
        current->exclusive = exclusive;
    }
    ++current->users;
    lease.host_ = host;
    lease.held_ = true;
    return Result::success;
}

Result release(Lease& lease) {
    auto* const current = lease.held_ ? state(lease.host_) : nullptr;
    if (current == nullptr || current->users == 0U) {
        return Result::failed;
    }
    if (current->users == 1U && spi_bus_free(lease.host_) != ESP_OK) {
        return Result::failed; // A device is still attached; the lease stays for a retry.
    }
    if (--current->users == 0U) {
        *current = {};
    }
    lease = {};
    return Result::success;
}

} // namespace newmota::platform::spi_host
