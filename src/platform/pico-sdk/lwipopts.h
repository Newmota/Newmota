/*
  lwipopts.h - lwIP configuration for Newmota on Raspberry Pi Pico W

  Copyright (C) 2026 Christian Baars and Newmota contributors

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

/** @file lwipopts.h
 * @brief LwIP configuration for Newmota on Raspberry Pi Pico W.
 */

#pragma once

#include "newmota_features.h"

#define NO_SYS 1
#define LWIP_DHCP 1
#define LWIP_DNS 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_RAW 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_LIBC_MALLOC 0
#define MEMP_MEM_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 16384
#define MEMP_NUM_UDP_PCB 8
#if USE_MATTER
#define LWIP_IPV6 1
#define LWIP_IPV6_AUTOCONFIG 1
#define SO_REUSE 1
#endif
#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * TCP_SND_BUF + (TCP_MSS - 1)) / TCP_MSS)
#define MEMP_NUM_TCP_SEG TCP_SND_QUEUELEN
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP_DOES_ACD_CHECK 0
