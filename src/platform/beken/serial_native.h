/*
  serial_native.h - Beken native UART adapter declarations

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Reports whether UART @p uart (1 or 2) carries the SDK log output. */
int newmota_beken_serial_reserved(int uart);
/** @brief Moves SDK log output to the other UART, without pads, before the app takes @p uart. */
void newmota_beken_serial_move_log(int uart);
/** @brief Opens UART @p uart (1 or 2) with its own capture ISR and per-pad mux selection. */
void newmota_beken_serial_start(int uart, uint32_t baud, unsigned bits, unsigned parity,
    unsigned stop, int rx, int tx, void (*handler)(void));
/** @brief Disables the application UART and restores its inactive SDK ISR registration. */
void newmota_beken_serial_stop(void);
/** @brief Acknowledges IRQ status and returns portable error/completion bits. */
uint32_t newmota_beken_serial_interrupt(void);
/** @brief Reads a hardware FIFO byte without waiting. */
int newmota_beken_serial_read(void);
/** @brief Fills available FIFO space in a bounded bulk operation. */
size_t newmota_beken_serial_write(const uint8_t*, size_t);
/** @brief Changes only the application UART's interrupt mask in the interrupt controller. */
void newmota_beken_serial_irq(int enable);
/** @brief Reports FIFO emptiness separately from the stop-bit completion ISR. */
int newmota_beken_serial_fifo_empty(void);
#ifdef __cplusplus
}
#endif
