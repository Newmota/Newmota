/*
  beken_app.c - Newmota-owned Beken application and Wi-Fi transport startup

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file beken_app.c
 * @brief Newmota-owned Beken application and Wi-Fi transport startup.
 *
 * This file replaces the vendor app/app.c application layer. The Beken
 * firmware supplies radio and RTOS primitives plus low-level message
 * producers, while Newmota selects the application threads. The transport
 * bridge lives here because the Beken Wi-Fi libraries call these symbols
 * directly.
 */

#include "include.h"
#include "mem_pub.h"
#include "rwnx_config.h"
#include "app.h"
#include "newmota_features.h"

#if (NX_POWERSAVE)
#include "ps.h"
#endif

#include "sa_ap.h"
#include "sa_station.h"
#include "main_none.h"
#include "sm.h"
#include "uart_pub.h"
#include "rtos_pub.h"
#include "error.h"
#include "param_config.h"
#include "rxl_cntrl.h"
#include "lwip/pbuf.h"
#include "rw_pub.h"
#include "rw_msg_rx.h"
#include "hostapd_intf_pub.h"
#include "wlan_ui_pub.h"
#include "ps_debug_pub.h"
#include "power_save_pub.h"
#include "mcu_ps_pub.h"
#include "rw_msdu.h"
#include "txu_cntrl.h"
#include "power_save.h"
#include "low_voltage_ps.h"
#include "ap_idle_pub.h"
#include "arbitrate.h"
#include "ke_event.h"

#if CFG_TX_BUFING
#include "rw_tx_buffering.h"
#endif

/* These objects are part of the Beken Wi-Fi message ABI. */
static beken_thread_t kmsg_thread_handle;
static beken_thread_t init_thread_handle;
static uint32_t init_stack_size = 2000;
static uint32_t app_stack_size = 4096;

beken_semaphore_t app_sema = NULL;
WIFI_CORE_T g_wifi_core = {0};
volatile int32_t bmsg_rx_count = 0;

extern void net_wlan_initial(void);
extern void wpas_thread_start(void);

void bk_wlan_app_init(void) {
#if (!CFG_SUPPORT_RTT)
    net_wlan_initial();
#endif
    wpas_thread_start();
}

void app_set_sema(void) {
    (void)rtos_set_semaphore(&app_sema);
}

static void kmsg_thread_main(void* arg) {
    (void)arg;
    mr_kmsg_init();
    while (1) {
        const OSStatus status = rtos_get_semaphore(&app_sema, BEKEN_WAIT_FOREVER);
        ASSERT(kNoErr == status);
        rwnx_recv_msg();
        ke_evt_none_core_scheduler();
    }
}

static void wlan_init_thread_main(void* arg) {
    (void)arg;
    GLOBAL_INT_START();
    bk_wlan_app_init();
    rtos_delete_thread(NULL);
}

int bmsg_ps_handler_rf_ps_mode_real_wakeup(void) {
#if CFG_USE_STA_PS
    power_save_rf_dtim_manual_do_wakeup();
#endif
    return 0;
}

static void bmsg_rx_handler(BUS_MSG_T* msg) {
    GLOBAL_INT_DECLARATION();
    GLOBAL_INT_DISABLE();
    if (bmsg_rx_count > 0) {
        bmsg_rx_count -= 1;
    }
    GLOBAL_INT_RESTORE();
    rxl_cntrl_evt((int)msg->arg);
}

static void bmsg_skt_tx_handler(BUS_MSG_T* msg) {
    hapd_intf_ke_rx_handle(msg->arg);
}

static void bmsg_tx_handler(BUS_MSG_T* msg) {
    struct pbuf* packet = (struct pbuf*)msg->arg;
    struct pbuf* coalesced = packet;
    const uint8_t vif_idx = (uint8_t)msg->len;

    if (packet->next) {
        coalesced = pbuf_coalesce(packet, PBUF_RAW);
        if (coalesced == packet) {
            goto done;
        }
    }

    ps_set_data_prevent();
#if CFG_USE_STA_PS
    bmsg_ps_handler_rf_ps_mode_real_wakeup();
    bk_wlan_dtim_rf_ps_mode_do_wakeup();
#endif
    rwm_transfer(vif_idx, coalesced->payload, coalesced->len, 0, 0);

done:
    pbuf_free(coalesced);
}

static void bmsg_tx_raw_cb_handler(BUS_MSG_T* msg) {
    rwm_raw_frame_with_cb((uint8_t*)msg->arg, msg->len, msg->cb, msg->param);
}

int bmsg_tx_raw_cb_sender(uint8_t* buffer, int length, void* callback, void* param) {
    BUS_MSG_T msg = {0};
    void* node = (void*)rwm_raw_frame_prepare_space(buffer, length);
    if (node == NULL) {
        return kGeneralErr;
    }
    msg.type = BMSG_TX_RAW_CB_TYPE;
    msg.arg = (uint32_t)node;
    msg.len = length;
    msg.cb = callback;
    msg.param = param;
    const OSStatus status = rtos_push_to_queue(&g_wifi_core.io_queue, &msg, 1 * SECONDS);
    if (status != kNoErr) {
        rwm_raw_frame_destroy_space(node);
    }
    return status;
}

static void bmsg_tx_raw_handler(BUS_MSG_T* msg) {
    uint8_t* packet = (uint8_t*)msg->arg;
    const uint16_t length = msg->len;
    MSDU_NODE_T* node = rwm_tx_node_alloc(length);
    if (node == NULL) {
        os_free(packet);
        return;
    }

    rwm_tx_msdu_renew(packet, length, node->msdu_ptr);
    UINT8* content = rwm_get_msdu_content_ptr(node);
    struct txdesc* descriptor = tx_txdesc_prepare(AC_VI);
    if (descriptor == NULL || TXDESC_STA_USED == descriptor->status) {
        rwm_node_free(node);
        os_free(packet);
        return;
    }

    descriptor->status = TXDESC_STA_USED;
    descriptor->host.flags = TXU_CNTRL_MGMT;
    descriptor->host.msdu_node = (void*)node;
    descriptor->host.orig_addr = (UINT32)node->msdu_ptr;
    descriptor->host.packet_addr = (UINT32)content;
    descriptor->host.packet_len = length;
    descriptor->host.status_desc_addr = (UINT32)content;
    descriptor->host.tid = 0xff;
    descriptor->umac.payl_len = length;
    descriptor->umac.head_len = 0;
    descriptor->umac.tail_len = 0;
    descriptor->umac.hdr_len_802_2 = 0;
    descriptor->umac.buf_control = &txl_buffer_control_24G;
    descriptor->lmac.agg_desc = NULL;
    descriptor->lmac.hw_desc->cfm.status = 0;

    ps_set_data_prevent();
#if CFG_USE_STA_PS
    bmsg_ps_handler_rf_ps_mode_real_wakeup();
    bk_wlan_dtim_rf_ps_mode_do_wakeup();
#endif
    txl_cntrl_push(descriptor, AC_VI);
    os_free(packet);
}

static void bmsg_ioctl_handler(BUS_MSG_T* msg) {
    ke_msg_send((void*)msg->arg);
}

/* Media is outside Newmota's Beken profile.  Keep the ABI sink so optional
 * vendor key/input objects do not pull the stock media application in. */
static void bmsg_music_handler(BUS_MSG_T* msg) {
    (void)msg;
}

void bmsg_skt_tx_sender(void* arg) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_SKT_TX_TYPE;
    msg.arg = (uint32_t)arg;
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

extern void power_save_wait_timer_real_handler(void);
extern void power_save_wait_timer_start(void);

static void ps_msg_process(UINT8 ps_msg) {
    switch (ps_msg) {
#if CFG_USE_STA_PS
    case PS_BMSG_IOCTL_RF_ENABLE:
        power_save_dtim_enable();
        break;
    case PS_BMSG_IOCTL_RF_USER_WKUP:
        bmsg_ps_handler_rf_ps_mode_real_wakeup();
        break;
    case PS_BMSG_IOCTL_RF_DISABLE:
        bmsg_ps_handler_rf_ps_mode_real_wakeup();
        power_save_dtim_disable();
        break;
    case PS_BMSG_IOCTL_RF_TD_SET:
#if (1 == CFG_LOW_VOLTAGE_PS)
        if (LV_PS_DISABLED)
#endif
        {
            ps_set_td_timer();
        }
        break;
    case PS_BMSG_IOCTL_RF_TD_STOP:
#if (0 == CFG_LOW_VOLTAGE_PS)
        ps_stop_td_timer();
#endif
        break;
#if PS_USE_WAIT_TIMER
    case PS_BMSG_IOCTL_WAIT_TM_HANDLER:
        power_save_wait_timer_real_handler();
        break;
    case PS_BMSG_IOCTL_WAIT_TM_SET:
        power_save_wait_timer_start();
        break;
#endif
#if PS_USE_KEEP_TIMER
    case PS_BMSG_IOCTL_RF_KP_HANDLER:
        power_save_keep_timer_real_handler();
        break;
    case PS_BMSG_IOCTL_RF_KP_SET:
        power_save_keep_timer_set();
        break;
    case PS_BMSG_IOCTL_RF_KP_STOP:
        power_save_keep_timer_stop();
        break;
    case PS_BMSG_IOCTL_RF_PS_TIMER_INIT:
#if (1 == CFG_LOW_VOLTAGE_PS)
        if (LV_PS_ENABLED) {
            power_save_set_keep_timer_time(lv_ps_get_keep_timer_duration());
        } else
#endif
        {
            power_save_set_keep_timer_time(20);
        }
        break;
    case PS_BMSG_IOCTL_RF_PS_TIMER_DEINIT:
        power_save_set_keep_timer_time(0);
        break;
#endif
#endif
#if CFG_USE_MCU_PS
    case PS_BMSG_IOCTL_MCU_ENABLE:
        mcu_ps_init();
        break;
    case PS_BMSG_IOCTL_MCU_DISABLE:
        mcu_ps_exit();
        break;
#endif
#if CFG_USE_AP_IDLE
    case PS_BMSG_IOCTL_AP_PS_RUN:
        ap_bcn_timer_real_handler();
        break;
    case PS_BMSG_IOCTL_AP_PS_STOP:
        stop_global_ap_bcn_timer();
        break;
    case PS_BMSG_IOCTL_AP_PS_START:
        start_global_ap_bcn_timer();
        break;
#endif
#if (1 == CFG_LOW_VOLTAGE_PS)
    case PS_BMSG_IOCTL_ARP_TX:
        if (LV_PS_ENABLED) {
            lv_ps_keepalive_arp_tx();
        }
        break;
#endif
    default:
        break;
    }
}

void bmsg_null_sender(void) {
    BUS_MSG_T msg = {0};
    if (g_wifi_core.io_queue == NULL || !rtos_is_queue_empty(&g_wifi_core.io_queue)) {
        return;
    }
    msg.type = BMSG_NULL_TYPE;
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

void bmsg_rx_sender(void* arg) {
    BUS_MSG_T msg = {0};
    GLOBAL_INT_DECLARATION();
    msg.type = BMSG_RX_TYPE;
    msg.arg = (uint32_t)arg;
    GLOBAL_INT_DISABLE();
    if (bmsg_rx_count >= 2) {
        GLOBAL_INT_RESTORE();
        return;
    }
    bmsg_rx_count += 1;
    GLOBAL_INT_RESTORE();
    if (rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT) != kNoErr) {
        GLOBAL_INT_DISABLE();
        bmsg_rx_count -= 1;
        GLOBAL_INT_RESTORE();
    }
}

int bmsg_tx_sender(struct pbuf* packet, uint32_t vif_idx) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_TX_TYPE;
    msg.arg = (uint32_t)packet;
    msg.len = vif_idx;
    pbuf_ref(packet);
    const OSStatus status = rtos_push_to_queue(&g_wifi_core.io_queue, &msg, 1 * SECONDS);
    if (status != kNoErr) {
        pbuf_free(packet);
    }
    return status;
}

int bmsg_tx_raw_sender(uint8_t* payload, uint16_t length) {
    BUS_MSG_T msg = {0};
    void* node = (void*)rwm_raw_frame_prepare_space(payload, length);
    if (payload != NULL) {
        os_free(payload);
    }
    if (node == NULL) {
        return kGeneralErr;
    }
    msg.type = BMSG_TX_RAW_CB_TYPE;
    msg.arg = (uint32_t)node;
    msg.len = length;
    const OSStatus status = rtos_push_to_queue(&g_wifi_core.io_queue, &msg, 1 * SECONDS);
    if (status != kNoErr) {
        rwm_raw_frame_destroy_space(node);
    }
    return status;
}

void bmsg_ps_sender(uint8_t arg) {
    if (g_wifi_core.io_queue == NULL) {
        return;
    }
    BUS_MSG_T msg = {0};
    msg.type = BMSG_STA_PS_TYPE;
    msg.arg = (uint32_t)arg;
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

#if SUPPORT_LSIG_MONITOR
static void bmsg_rx_lsig_handler(BUS_MSG_T* msg) {
    lsig_input((msg->arg & 0xFFFF0000) >> 16, msg->arg & 0xFF, msg->len);
}

void bmsg_rx_lsig(uint16_t length, uint8_t rssi) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_RX_LSIG;
    msg.arg = (uint32_t)((length << 16) | rssi);
    msg.len = rtos_get_time();
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}
#endif

int bmsg_ioctl_sender(void* arg) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_IOCTL_TYPE;
    msg.arg = (uint32_t)arg;
    return rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

void bmsg_music_sender(void* arg) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_MEDIA_TYPE;
    msg.arg = (uint32_t)arg;
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

#if CFG_TX_BUFING
void bmsg_tx_bufing_sender(uint32_t cid_and_flag) {
    BUS_MSG_T msg = {0};
    msg.type = BMSG_TX_BUFING_TYPE;
    msg.arg = cid_and_flag;
    (void)rtos_push_to_queue(&g_wifi_core.io_queue, &msg, BEKEN_NO_WAIT);
}

static void bmsg_tx_bufing_handler(BUS_MSG_T* msg) {
    rwm_tx_bufing_restore_data(msg->arg);
}
#endif

static void bmsg_ps_handler(BUS_MSG_T* msg) {
    ps_msg_process((UINT8)msg->arg);
}

static void core_thread_main(void* arg) {
    (void)arg;
    BUS_MSG_T msg;
    uint8_t skip_ke = 0;
#if CFG_USE_STA_PS
    uint8_t ps_pending = 0;
#endif

    while (1) {
        const OSStatus status =
            rtos_pop_from_queue(&g_wifi_core.io_queue, &msg, BEKEN_WAIT_FOREVER);
        if (status != kNoErr) {
            continue;
        }
        switch (msg.type) {
#if CFG_USE_STA_PS
        case BMSG_STA_PS_TYPE:
            if (msg.arg == PS_BMSG_IOCTL_RF_DISABLE) {
                bmsg_ps_handler(&msg);
            } else {
                ps_pending = 1;
            }
            break;
#endif
        case BMSG_RX_TYPE:
            bmsg_rx_handler(&msg);
            break;
        case BMSG_TX_TYPE:
            bmsg_tx_handler(&msg);
            break;
        case BMSG_SKT_TX_TYPE:
            bmsg_skt_tx_handler(&msg);
            break;
        case BMSG_IOCTL_TYPE:
            bmsg_ioctl_handler(&msg);
            break;
        case BMSG_MEDIA_TYPE:
            bmsg_music_handler(&msg);
            skip_ke = 1;
            break;
#if CFG_TX_BUFING
        case BMSG_TX_BUFING_TYPE:
            bmsg_tx_bufing_handler(&msg);
            break;
#endif
        case BMSG_TX_RAW_TYPE:
            bmsg_tx_raw_handler(&msg);
            break;
        case BMSG_TX_RAW_CB_TYPE:
            bmsg_tx_raw_cb_handler(&msg);
            break;
#if SUPPORT_LSIG_MONITOR
        case BMSG_RX_LSIG:
            bmsg_rx_lsig_handler(&msg);
            break;
#endif
        default:
            break;
        }
        if (msg.sema != NULL) {
            rtos_set_semaphore(&msg.sema);
        }
        if (!skip_ke) {
            ke_evt_core_scheduler();
        } else {
            skip_ke = 0;
        }
#if CFG_USE_STA_PS
        if (ps_pending) {
            bmsg_ps_handler(&msg);
            ps_pending = 0;
        }
        power_save_rf_sleep_check();
#endif
    }
}

void core_thread_init(void) {
    g_wifi_core.queue_item_count = CORE_QITEM_COUNT;
    g_wifi_core.stack_size = CORE_STACK_SIZE;
    if (rtos_init_queue(&g_wifi_core.io_queue, "core_queue", sizeof(BUS_MSG_T),
            g_wifi_core.queue_item_count) != kNoErr) {
        return;
    }
    if (rtos_create_thread(&g_wifi_core.handle, THD_CORE_PRIORITY, "core_thread",
            (beken_thread_function_t)core_thread_main, (unsigned short)g_wifi_core.stack_size,
            (beken_thread_arg_t)0) != kNoErr) {
        core_thread_uninit();
    }
}

void core_thread_uninit(void) {
    if (g_wifi_core.handle) {
        rtos_delete_thread(&g_wifi_core.handle);
        g_wifi_core.handle = 0;
    }
    if (g_wifi_core.io_queue) {
        rtos_deinit_queue(&g_wifi_core.io_queue);
        g_wifi_core.io_queue = 0;
    }
    g_wifi_core.queue_item_count = 0;
    g_wifi_core.stack_size = 0;
}

void app_pre_start(void) {
    if (rtos_init_semaphore(&app_sema, 1) != kNoErr) {
        return;
    }
    if (rtos_create_thread(&kmsg_thread_handle, THD_APPLICATION_PRIORITY, "kmsgbk",
            (beken_thread_function_t)kmsg_thread_main, (unsigned short)app_stack_size,
            (beken_thread_arg_t)0) != kNoErr) {
        return;
    }
    if (rtos_create_thread(&init_thread_handle, THD_INIT_PRIORITY, "init_thread",
            (beken_thread_function_t)wlan_init_thread_main, (unsigned short)init_stack_size,
            (beken_thread_arg_t)0) != kNoErr) {
        return;
    }
    core_thread_init();
    rf_thread_init();
#if (USE_BLE && CFG_SUPPORT_BLE && (CFG_BLE_VERSION != BLE_VERSION_4_2))
    extern void ble_entry(void);
    ble_entry();
#endif
    power_save_rf_hold_bit_clear(RF_HOLD_BY_STA_BIT);
}

void app_start(void) {
    /* Deliberately no vendor CLI, media, demo, or user application startup. */
    app_pre_start();
}

int bmsg_is_empty(void) {
    return g_wifi_core.io_queue == NULL || rtos_is_queue_empty(&g_wifi_core.io_queue);
}

/* The vendor build enables its manufacturing ATE hooks in the common
 * configuration.  Newmota does not expose that test application, but the
 * radio/driver code still references its small ABI.  Keep normal firmware on
 * the Newmota path and leave the mode flag available to vendor code. */
char ate_mode_state = 0;

void ate_app_init(void) {
    ate_mode_state = 0;
}

uint32_t get_ate_mode_state(void) {
    return 0;
}

void ate_start(void) {
    app_start();
}
