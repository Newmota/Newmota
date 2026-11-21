/*
  adc_native.c - Beken calibrated ADC conversion bridge

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file adc_native.c
 * @brief Beken calibrated ADC conversion bridge.
 */

#include "adc_native.h"
#include "include.h"
#include "saradc_pub.h"
#include "drv_model_pub.h"
#include "rtos_pub.h"
static saradc_desc_t descriptor;
static UINT16 data[2];
static DD_HANDLE handle = DD_HANDLE_UNVALID;

int newmota_beken_adc_channel(unsigned pin) {
#if CFG_SOC_NAME == SOC_BK7231N
    switch (pin) {
    case 26:
        return 1;
    case 24:
        return 2;
    case 23:
        return 3;
    case 28:
        return 4;
    case 22:
        return 5;
    case 21:
        return 6;
    default:
        return -1;
    }
#elif CFG_SOC_NAME == SOC_BK7238
    /* BK7238 external channels follow its pinned GPIO second-function map. */
    switch (pin) {
    case 26:
        return 1;
    case 24:
        return 2;
    case 20:
        return 3;
    case 28:
        return 4;
    case 1:
        return 5;
    case 10:
        return 6;
    default:
        return -1;
    }
#else
    return -1;
#endif
}

int newmota_beken_adc_start(unsigned pin) {
    const int channel = newmota_beken_adc_channel(pin);
    UINT32 status = 0;
    if (channel < 0)
        return -1;
    const UINT32 saved = rtos_disable_int();
    if (handle != DD_HANDLE_UNVALID || saradc_check_busy()) {
        rtos_enable_int(saved);
        return 1;
    }
    saradc_config_param_init(&descriptor);
    descriptor.channel = channel;
    descriptor.data_buff_size = 2;
    descriptor.pData = data;
    descriptor.p_Int_Handler = 0;
    handle = ddev_open(SARADC_DEV_NAME, &status, (UINT32)&descriptor);
    rtos_enable_int(saved);
    return handle == DD_HANDLE_UNVALID ? -1 : 0;
}

int newmota_beken_adc_cancel(void) {
    const UINT32 saved = rtos_disable_int();
    if (handle != DD_HANDLE_UNVALID) {
        if (ddev_close(handle) != DRV_SUCCESS) {
            rtos_enable_int(saved);
            return -1;
        }
        handle = DD_HANDLE_UNVALID;
    }
    rtos_enable_int(saved);
    return 0;
}

int newmota_beken_adc_finish(uint32_t* raw, uint32_t* millivolts) {
    const UINT32 saved = rtos_disable_int();
    if (handle == DD_HANDLE_UNVALID) {
        rtos_enable_int(saved);
        return -1;
    }
    if (!descriptor.all_done) {
        rtos_enable_int(saved);
        return 1;
    }
    UINT32 converted = data[1];
#if CFG_SOC_NAME == SOC_BK7231N
    converted = saradc_format_data(converted);
#else
    const UINT32 accuracy = saradc_check_accuracy();
    if (accuracy)
        converted >>= accuracy - 1;
#endif
    const float volts = saradc_calculate(converted);
    *raw = converted;
    *millivolts = volts > 0 ? volts * 1000 : 0;
    rtos_enable_int(saved);
    return newmota_beken_adc_cancel();
}
