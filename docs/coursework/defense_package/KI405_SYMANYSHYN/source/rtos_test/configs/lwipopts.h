/*
 * Copyright (c) 2001-2003 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt
 *
 */
#pragma once

#include <whd_types.h>

/* ---------- Memory options ---------- */
#define MEM_ALIGNMENT                   (4)
#define MEM_LIBC_MALLOC                 (1)

/* ---------- Protocol support ---------- */
#define LWIP_RAW                        (1)
#define LWIP_IPV4                       (1)
#define LWIP_IPV6                       (1)
#define LWIP_ICMP                       (1)
#define LWIP_TCP                        (1)
#define LWIP_UDP                        (1)
#define LWIP_IGMP                       (1)
#define LWIP_DNS                        (1)

/* ---------- ARP ---------- */
#define ETHARP_SUPPORT_STATIC_ENTRIES   (1)

/* ---------- DHCP ---------- */
#define LWIP_DHCP                       (1)
#define DHCP_DOES_ARP_CHECK             (0)

/* ---------- Socket options ---------- */
#define LWIP_SO_SNDTIMEO                (1)
#define LWIP_SO_RCVTIMEO                (1)
#define SO_REUSE                        (1)
#define LWIP_TCP_KEEPALIVE              (1)
#define LWIP_SOCKET                     (1)
#define LWIP_NETCONN                    (1)
#define LWIP_SO_RCVBUF                  (128)

/* ---------- Buffer sizes ---------- */
#define PBUF_LINK_HLEN                  (WHD_PHYSICAL_HEADER)
#define TCP_MSS                         (WHD_PAYLOAD_MTU)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((6 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

/* ---------- Checksum options ---------- */
#define LWIP_CHECKSUM_CTRL_PER_NETIF    (1)
#define CHECKSUM_GEN_IP                 (1)
#define CHECKSUM_GEN_UDP                (1)
#define CHECKSUM_GEN_TCP                (1)
#define CHECKSUM_GEN_ICMP               (1)
#define CHECKSUM_GEN_ICMP6              (1)
#define CHECKSUM_CHECK_IP               (1)
#define CHECKSUM_CHECK_UDP              (1)
#define CHECKSUM_CHECK_TCP              (1)
#define CHECKSUM_CHECK_ICMP             (1)
#define CHECKSUM_CHECK_ICMP6            (1)
#define LWIP_CHECKSUM_ON_COPY           (1)
#define LWIP_CHKSUM_ALGORITHM           (3)

/* ---------- Thread / OS options ---------- */
#define SYS_LIGHTWEIGHT_PROT            (1)
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT  (1)
#define DEFAULT_TCP_RECVMBOX_SIZE       (12)
#define TCPIP_MBOX_SIZE                 (16)
#define TCPIP_THREAD_STACKSIZE          (4 * 1024)
#define TCPIP_THREAD_PRIO               (4)
#define DEFAULT_RAW_RECVMBOX_SIZE       (12)
#define DEFAULT_UDP_RECVMBOX_SIZE       (12)
#define DEFAULT_ACCEPTMBOX_SIZE         (8)

/* ---------- MEMP options ---------- */
#define MEMP_NUM_UDP_PCB                (8)
#define MEMP_NUM_TCP_PCB                (8)
#define MEMP_NUM_TCP_PCB_LISTEN         (1)
#define MEMP_NUM_TCP_SEG                (27)
#define MEMP_NUM_SYS_TIMEOUT            (14)
#define PBUF_POOL_SIZE                  (5)
#define MEMP_NUM_NETBUF                 (8)
#define MEMP_NUM_NETCONN                (16)

/* ---------- Stats ---------- */
#ifdef DEBUG
#define LWIP_STATS                      (1)
#else
#define LWIP_STATS                      (0)
#endif

/* ---------- Core locking ---------- */
#define LWIP_TCPIP_CORE_LOCKING         (1)
#define LWIP_TCPIP_CORE_LOCKING_INPUT   (1)
#define LWIP_FREERTOS_CHECK_CORE_LOCKING (1)
#define LWIP_ASSERT_CORE_LOCKED()       sys_check_core_locking()

/* ---------- Netif options ---------- */
#define LWIP_NETIF_API                  (1)
#define LWIP_NETIF_TX_SINGLE_PBUF       (1)
#define LWIP_NETIF_STATUS_CALLBACK      (1)
#define LWIP_NETIF_LINK_CALLBACK        (1)
#define LWIP_NETIF_REMOVE_CALLBACK      (1)

/* ---------- Misc ---------- */
#define LWIP_PROVIDE_ERRNO              (1)
#define LWIP_RAND                       rand

/*
 * Use the timeval from GCC newlib, not the one from lwIP.
 * Prevents 'redefinition of struct timeval' errors.
 */
#if defined(__GNUC__) && !defined(__ARMCC_VERSION)
#define LWIP_TIMEVAL_PRIVATE            (0)
#endif

/* ---------- SNTP (wall-clock timestamps) ---------- */
#define SNTP_SERVER_DNS                 (1)
#define SNTP_MAX_SERVERS                (2)
#define SNTP_UPDATE_DELAY               (3600000)   /* re-sync every hour */
#define SNTP_STARTUP_DELAY              (0)         /* no random start delay */

/* Called by lwip/sntp.c with Unix-epoch seconds + microseconds */
extern void wallclock_sntp_set_time(uint32_t sec, uint32_t us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) wallclock_sntp_set_time((sec), (us))

extern void sys_check_core_locking(void);
