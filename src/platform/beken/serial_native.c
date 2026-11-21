/*
  serial_native.c - Beken native UART interrupt and transmit hooks

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#include "serial_native.h"
#include "arm_arch.h"
#include "uart.h"
#include "gpio.h"
#include "gpio_pub.h"
#include "icu_pub.h"
#include "intc_pub.h"
#include "drv_model_pub.h"

/** The UART the application owns: UART1 on GPIO10/11 or UART2 on GPIO0/1. */
static int app_port = UART2_PORT;

static UINT32 reg_config(void) {
    return app_port == UART1_PORT ? REG_UART1_CONFIG : REG_UART2_CONFIG;
}

static UINT32 reg_intr_enable(void) {
    return app_port == UART1_PORT ? REG_UART1_INTR_ENABLE : REG_UART2_INTR_ENABLE;
}

static UINT32 reg_intr_status(void) {
    return app_port == UART1_PORT ? REG_UART1_INTR_STATUS : REG_UART2_INTR_STATUS;
}

/** @brief Reports whether a UART carries the SDK log output. */
int newmota_beken_serial_reserved(int uart) {
    return uart_print_port == (uart == 1 ? UART1_PORT : UART2_PORT);
}

/** @brief Moves SDK log output off a UART the application takes, as Tasmota's ClaimSerial.
 * The other UART's transmitter runs without pads, so bk_printf neither reaches the
 * application's wire nor waits on a full FIFO.
 */
void newmota_beken_serial_move_log(int uart) {
    const int other = uart == 1 ? UART2_PORT : UART1_PORT;
    bk_uart_config_t config = {0};
    config.baud_rate = 115200;
    config.data_width = BK_DATA_WIDTH_8BIT;
    config.stop_bits = BK_STOP_BITS_1;
    config.parity = BK_PARITY_NO;
    uart_hw_set_change(other, &config);
    const UINT32 reg = other == UART1_PORT ? REG_UART1_CONFIG : REG_UART2_CONFIG;
    REG_WRITE(reg, REG_READ(reg) | UART_TX_ENABLE);
    uart_print_port = other;
}

/** @brief Changes only the application UART's interrupt mask in the interrupt controller. */
void newmota_beken_serial_irq(int enable) {
    UINT32 bits = app_port == UART1_PORT ? IRQ_UART1_BIT : IRQ_UART2_BIT;
    sddev_control(ICU_DEV_NAME, enable ? CMD_ICU_INT_ENABLE : CMD_ICU_INT_DISABLE, &bits);
}

/** @brief Opens one UART with its own capture ISR and per-pad mux selection. */
void newmota_beken_serial_start(int uart, uint32_t baud, unsigned bits, unsigned parity,
    unsigned stop, int rx, int tx, void (*handler)(void)) {
    newmota_beken_serial_irq(0);
    app_port = uart == 1 ? UART1_PORT : UART2_PORT;
    bk_uart_config_t config = {0};
    config.baud_rate = baud;
    config.data_width = (uart_data_width_t)(bits - 5);
    config.stop_bits = stop == 1 ? BK_STOP_BITS_1 : BK_STOP_BITS_2;
    config.parity = parity == 0 ? BK_PARITY_NO : parity == 1 ? BK_PARITY_EVEN : BK_PARITY_ODD;
    uart_hw_set_change(app_port, &config);
    // The pinned gpio_enable_second_function uses PERIAL_MODE_1 (zero) on GPIO0/1 for UART2
    // and GPIO10/11 for UART1, two mux bits per pad. Select one pad at a time so a one-way
    // port never touches an unclaimed pad.
    const unsigned tx_pad = app_port == UART1_PORT ? 11U : 0U;
    const unsigned rx_pad = app_port == UART1_PORT ? 10U : 1U;
    UINT32 mux = REG_READ(REG_GPIO_FUNC_CFG);
    if (tx) {
        mux &= ~(3U << (tx_pad * 2U));
        gpio_config(tx_pad, GMODE_SECOND_FUNC_PULL_UP);
    }
    if (rx) {
        mux &= ~(3U << (rx_pad * 2U));
        gpio_config(rx_pad, GMODE_SECOND_FUNC_PULL_UP);
    }
    REG_WRITE(REG_GPIO_FUNC_CFG, mux);
    UINT32 enable = REG_READ(reg_config()) & ~(UART_RX_ENABLE | UART_TX_ENABLE);
    if (rx)
        enable |= UART_RX_ENABLE;
    if (tx)
        enable |= UART_TX_ENABLE;
    REG_WRITE(reg_config(), enable);
    REG_WRITE(reg_intr_status(), REG_READ(reg_intr_status()));
    REG_WRITE(reg_intr_enable(),
        UART_TX_STOP_END_EN |
            (rx ? RX_FIFO_NEED_READ_EN | UART_RX_STOP_END_EN | UART_RX_PARITY_ERR_EN |
                        UART_RX_STOP_ERR_EN | RX_FIFO_OVER_FLOW_EN
                : 0));
    if (app_port == UART1_PORT)
        intc_service_register(IRQ_UART1, PRI_IRQ_UART1, handler);
    else
        intc_service_register(IRQ_UART2, PRI_IRQ_UART2, handler);
}

/** @brief Disables the application UART and restores its inactive SDK ISR registration. */
void newmota_beken_serial_stop(void) {
    newmota_beken_serial_irq(0);
    REG_WRITE(reg_intr_enable(), 0);
    REG_WRITE(reg_config(), 0);
    if (app_port == UART1_PORT)
        intc_service_register(IRQ_UART1, PRI_IRQ_UART1, uart1_isr);
    else
        intc_service_register(IRQ_UART2, PRI_IRQ_UART2, uart2_isr);
}

/** @brief Acknowledges IRQ status and returns portable error/completion bits. */
uint32_t newmota_beken_serial_interrupt(void) {
    const UINT32 status = REG_READ(reg_intr_status());
    REG_WRITE(reg_intr_status(), status);
    return ((status & (RX_FIFO_OVER_FLOW_STA | UART_RX_PARITY_ERR_STA | UART_RX_STOP_ERR_STA))
                   ? 1U
                   : 0U) |
           ((status & UART_TX_STOP_END_STA) ? 2U : 0U);
}

/** @brief Reads a hardware FIFO byte without waiting. */
int newmota_beken_serial_read(void) {
    return uart_read_byte(app_port);
}

/** @brief Fills available FIFO space in a bounded bulk operation. */
size_t newmota_beken_serial_write(const uint8_t* data, size_t size) {
    size_t n = 0;
    REG_WRITE(reg_intr_status(), UART_TX_STOP_END_STA);
    while (n < size && n < 128 && !uart_is_tx_fifo_full(app_port)) {
        uart_write_byte(app_port, (char)data[n]);
        ++n;
    }
    return n;
}

/** @brief Reports FIFO emptiness separately from the stop-bit completion ISR. */
int newmota_beken_serial_fifo_empty(void) {
    return uart_is_tx_fifo_empty(app_port);
}
