/*
  display_qspi.cpp - Bouffalo DBI four-lane display transport

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
/** @file display_qspi.cpp
 * @brief Bounded mode-0 QSPI through BL616/BL618DG's dedicated DBI controller.
 */
#include "newmota/platform/display/native.h"
#include "newmota/platform/platform.h"
extern "C" {
#include "bflb_clock.h"
#include "bflb_dbi.h"
#include "bflb_gpio.h"
#include "hardware/dbi_reg.h"
}
#include <algorithm>

namespace newmota::platform::display::detail {
namespace {
bflb_device_s* device{};
std::uint32_t control{};

/** @brief Uses normal data mode to preserve exact bytes without pixel conversion.
 * The hardware byte count is eight bits; every frame holds at most 256 bytes.
 * Partial FIFO words are explicitly padded, without unaligned or excess reads.
 */
Result frame(bool pixels, std::uint8_t opcode, const std::uint8_t* bytes, std::size_t length) {
    const auto base = device->reg_base;
    auto config = control;
    if (length)
        config |= DBI_CR_DBI_DAT_EN | ((length - 1) << DBI_CR_DBI_DAT_BC_SHIFT);
    putreg32(control, base + DBI_CONFIG_OFFSET);
    putreg32((2U << DBI_CR_QSPI_ADR_BC_SHIFT) | (pixels ? DBI_CR_QSPI_DAT_4B : 0),
        base + DBI_QSPI_CONFIG_OFFSET);
    // DBI sends the low address byte first: 00, DCS opcode, 00.
    putreg32(static_cast<std::uint32_t>(opcode) << 8, base + DBI_QSPI_ADR_OFFSET);
    putreg32(pixels ? 0x32 : 0x02, base + DBI_CMD_OFFSET);
    putreg32(DBI_TX_FIFO_CLR, base + DBI_FIFO_CONFIG_0_OFFSET);
    putreg32(DBI_CR_DBI_END_CLR | DBI_CR_DBI_END_MASK | DBI_CR_DBI_TXF_MASK | DBI_CR_DBI_FER_MASK,
        base + DBI_INT_STS_OFFSET);
    putreg32(config | DBI_CR_DBI_EN, base + DBI_CONFIG_OFFSET);
    const auto start = monotonic_milliseconds();
    std::size_t at{};
    while (true) {
        if (at < length && (getreg32(base + DBI_FIFO_CONFIG_1_OFFSET) & DBI_TX_FIFO_CNT_MASK)) {
            std::uint32_t word{};
            for (unsigned shift = 0; shift < 32 && at < length; shift += 8)
                word |= static_cast<std::uint32_t>(bytes[at++]) << shift;
            putreg32(word, base + DBI_FIFO_WDATA_OFFSET);
        }
        const auto status = getreg32(base + DBI_INT_STS_OFFSET);
        if (status & DBI_END_INT) {
            putreg32(control, base + DBI_CONFIG_OFFSET);
            return at == length ? Result::success : Result::failed;
        }
        if (monotonic_milliseconds() - start >= 100U) {
            putreg32(control, base + DBI_CONFIG_OFFSET);
            putreg32(DBI_TX_FIFO_CLR, base + DBI_FIFO_CONFIG_0_OFFSET);
            return Result::timeout;
        }
    }
}
} // namespace

Result native_qspi_preflight(const Config& c) {
#if defined(BL616)
    constexpr int clock = 10, select = 11, data = 12;
#elif defined(BL618DG)
    // Provisional signal order inferred from the SDK GPIO0-5 QSPI group.
    // The BL618DG per-signal mux order still requires vendor/hardware confirmation.
    constexpr int clock = 0, select = 1, data = 2;
#else
#error "QSPI requires BL616 or BL618DG"
#endif
    if (c.spi_bus != 1 || c.clock != clock || c.cs != select)
        return Result::unsupported;
    for (unsigned i = 0; i < 4; ++i)
        if (c.quad_pins[i] != data + static_cast<int>(i))
            return Result::unsupported;
    return Result::success;
}

Result native_qspi_open(const Config& c) {
    device = bflb_device_get_by_name("dbi");
    auto* gpio = bflb_device_get_by_name("gpio");
    if (!device || !gpio)
        return Result::unsupported;
    if (getreg32(device->reg_base + DBI_CONFIG_OFFSET) & (DBI_CR_DBI_EN | DBI_STS_DBI_BUS_BUSY))
        return Result::busy;
    const auto clock = bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_DBI, device->idx);
    const auto requested = static_cast<std::uint32_t>(c.speed) * 1000000U;
    const auto divisor = std::max<std::uint32_t>(2U, (clock + requested - 1U) / requested);
    if (divisor > 512)
        return Result::unsupported;
    const auto low = (divisor + 1) / 2 - 1, high = divisor / 2 - 1;
    putreg32((low << DBI_CR_DBI_PRD_S_SHIFT) | (high << DBI_CR_DBI_PRD_I_SHIFT) |
                 (low << DBI_CR_DBI_PRD_D_PH_0_SHIFT) | (high << DBI_CR_DBI_PRD_D_PH_1_SHIFT),
        device->reg_base + DBI_PRD_OFFSET);
    control = (DBI_MODE_EX_QSPI << DBI_CR_DBI_SEL_SHIFT) | DBI_CR_DBI_CMD_EN | DBI_CR_DBI_DAT_WR |
              DBI_CR_DBI_SCL_PH | DBI_CR_DBI_CS_STRETCH;
    putreg32(control, device->reg_base + DBI_CONFIG_OFFSET);
    const auto flags = GPIO_FUNC_DBI_QSPI | GPIO_ALTERNATE | GPIO_FLOAT | GPIO_DRV_1;
    bflb_gpio_init(gpio, c.clock, flags);
    bflb_gpio_init(gpio, c.cs, flags);
    for (auto pad : c.quad_pins)
        bflb_gpio_init(gpio, pad, flags);
    return Result::success;
}

Result native_qspi_write(
    bool pixels, std::uint8_t opcode, const std::uint8_t* bytes, std::size_t length) {
    if (!device || length > 4096 || (length && !bytes) || (!pixels && length > 256))
        return Result::failed;
    do {
        const auto count = std::min(length, std::size_t{256});
        const auto result = frame(pixels, opcode, bytes, count);
        if (result != Result::success)
            return result;
        length -= count;
        if (count)
            bytes += count;
        opcode = 0x3c;
    } while (length);
    return Result::success;
}
} // namespace newmota::platform::display::detail
