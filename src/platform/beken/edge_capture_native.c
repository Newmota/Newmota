/*
  edge_capture_native.c - Beken native per-pin edge interrupt registration

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file edge_capture_native.c
 * @brief Beken native per-pin edge interrupt registration.
 */

#include "edge_capture_native.h"
#include "include.h"
#include "arm_arch.h"
#include "gpio.h"
#include "gpio_pub.h"
#include "rtos_pub.h"
#include "sys_ctrl.h"

/* Clear one pending edge where gpio_isr reads it: BK7252N keeps GPIO interrupt
 * status in the system controller, and the 40-pin parts use a second word for
 * GPIO32..39. */
static void clear_pending(unsigned pin) {
#if (CFG_SOC_NAME == SOC_BK7252N)
    if (pin < 32U)
        REG_WRITE(REG_SYS_CTRL_GPIO_INTSTA, 1U << pin);
    else
        REG_WRITE(REG_SYS_CTRL_GPIO_INTSTA2, 1U << (pin - 32U));
#elif (CFG_SOC_NAME == SOC_BK7221U) || (CFG_SOC_NAME == SOC_BK7231U)
    if (pin < 32U)
        REG_WRITE(REG_GPIO_INTSTA, 1U << pin);
    else
        REG_WRITE(REG_GPIO_INTSTA2, 1U << (pin - 32U));
#else
    REG_WRITE(REG_GPIO_INTSTA, 1U << pin);
#endif
}

uint32_t newmota_beken_edge_lock(void) {
    return rtos_disable_int();
}

void newmota_beken_edge_unlock(uint32_t state) {
    rtos_enable_int(state);
}

uint64_t newmota_beken_edge_time(void) {
    return rtos_get_time_us();
}

void newmota_beken_edge_start(unsigned pin, int rising, void (*handler)(unsigned char)) {
    gpio_int_disable(pin);
    clear_pending(pin);
    gpio_int_enable(pin, rising ? GPIO_INT_LEVEL_RISING : GPIO_INT_LEVEL_FALLING, handler);
}

void newmota_beken_edge_stop(unsigned pin) {
    gpio_int_disable(pin);
    /* The SDK owns its callback array; register NULL while interrupts remain masked. */
    gpio_int_enable(pin, GPIO_INT_LEVEL_FALLING, 0);
    gpio_int_disable(pin);
    clear_pending(pin);
}
