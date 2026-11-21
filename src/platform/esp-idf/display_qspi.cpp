/*
  display_qspi.cpp - ESP-IDF four-lane display transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_qspi.cpp
 * @brief Synchronous mode-0 QSPI with single-lane command/address phases.
 * Framing follows Tasmota UDisplay QSPIPanel, Copyright (C) 2021 Gerhard Mutz
 * and Theo Arends; GPL-3.0-or-later.
 */
#include "newmota/platform/display/native.h"
#include "spi_host.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include <cstring>

namespace newmota::platform::display::detail {
namespace {
spi_host::Lease lease{};
spi_device_handle_t device{};
std::uint8_t* staging{};
constexpr std::size_t capacity = 4096;

Result result(esp_err_t error) {
    return error == ESP_OK            ? Result::success
           : error == ESP_ERR_NO_MEM  ? Result::no_memory
           : error == ESP_ERR_TIMEOUT ? Result::timeout
                                      : Result::failed;
}
} // namespace

Result native_qspi_preflight(const Config& c) {
    if (c.spi_bus < 1 || c.spi_bus > 2 || (c.spi_bus == 2 && SOC_SPI_PERIPH_NUM < 3))
        return Result::unsupported;
    return Result::success;
}

Result native_qspi_open(const Config& c) {
    staging = static_cast<std::uint8_t*>(
        heap_caps_malloc(capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!staging)
        return Result::no_memory;
    spi_bus_config_t bus{};
    bus.sclk_io_num = c.clock;
    bus.mosi_io_num = c.quad_pins[0];
    bus.miso_io_num = c.quad_pins[1];
    bus.quadwp_io_num = c.quad_pins[2];
    bus.quadhd_io_num = c.quad_pins[3];
    bus.max_transfer_sz = capacity;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    const auto host = c.spi_bus == 1 ? SPI2_HOST : static_cast<spi_host_device_t>(2);
    // Quad mode drives all four data lines, so no other device can share the host.
    switch (spi_host::acquire(host, bus, spi_host::Use::exclusive, lease)) {
    case spi_host::Result::success:
        break;
    case spi_host::Result::no_memory:
        return Result::no_memory;
    case spi_host::Result::busy:
    case spi_host::Result::conflict:
        return Result::busy;
    default:
        return Result::failed;
    }
    spi_device_interface_config_t config{};
    config.command_bits = 8;
    config.address_bits = 24;
    config.clock_speed_hz = c.speed * 1000000;
    config.spics_io_num = c.cs;
    config.flags = SPI_DEVICE_HALFDUPLEX;
    config.queue_size = 1;
    return result(spi_bus_add_device(host, &config, &device));
}

Result native_qspi_write(
    bool pixels, std::uint8_t opcode, const std::uint8_t* bytes, std::size_t length) {
    if (!device || length > capacity || (length && !bytes))
        return Result::failed;
    if (length)
        std::memcpy(staging, bytes, length);
    spi_transaction_t transaction{};
    transaction.cmd = pixels ? 0x32 : 0x02;
    transaction.addr = static_cast<std::uint32_t>(opcode) << 8;
    transaction.flags = pixels ? SPI_TRANS_MODE_QIO : 0;
    transaction.length = length * 8;
    transaction.tx_buffer = length ? staging : nullptr;
    return result(spi_device_polling_transmit(device, &transaction));
}
} // namespace newmota::platform::display::detail
