/*
 * ACSI_NET: network interface for STinG over ACSI (see ACSI_NET-ontwerp.md).
 *
 * Phase 1: detection and configuration only. NET_INFO reports what the
 * adapter is and what it knows about the network, NET_CTRL stores the
 * Atari's IP settings. Nothing is filtered or bridged yet: without these
 * commands nothing changes.
 *
 * core0 publishes a snapshot of the Wi-Fi state (net_bridge_publish), core1
 * builds the replies from it, so core1 never touches lwIP.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "acsi.h"

#if BOARD_HAS_WIFI
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#endif

#define NET_PROTO_VERSION   1
#define NET_CAP_CTRL        0x0001          /* NET_CTRL stores settings */
#define NET_CAP_TEST        0x8000          /* NET_TEST short read test  */

bool net_link_up(void);                     /* net.c */

/* written by core0, read by core1 */
static volatile bool     pub_link;
static volatile uint32_t pub_ip, pub_mask, pub_gw;
static uint8_t           pub_mac[6];

/* written by core1 (NET_CTRL), read by core0 for the console */
static volatile bool     atari_enabled;
static volatile uint32_t atari_ip, atari_mask;
static volatile uint32_t ctrl_count, ctrl_last;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

/* lwIP keeps addresses in network order: as a number, a.b.c.d = 0xaabbccdd */
static uint32_t ip_num(uint32_t lwip) { return __builtin_bswap32(lwip); }

void net_bridge_publish(void)               /* core0 */
{
#if BOARD_HAS_WIFI
    struct netif *n = netif_default;
    bool link = net_link_up();
    if (n) {
        pub_ip = ip_num(ip4_addr_get_u32(netif_ip4_addr(n)));
        pub_mask = ip_num(ip4_addr_get_u32(netif_ip4_netmask(n)));
        pub_gw = ip_num(ip4_addr_get_u32(netif_ip4_gw(n)));
        memcpy(pub_mac, n->hwaddr, 6);
    }
    pub_link = link;
#endif
}

/* NET_INFO (vendor sub 0x20), 512 bytes, big-endian */
void net_info(uint8_t *b)                   /* core1 */
{
    memset(b, 0, 512);
    memcpy(b, "ATN", 3);
    put16(b + 4, NET_PROTO_VERSION);
    put16(b + 6, NET_CAP_CTRL | NET_CAP_TEST);
    b[8] = (pub_link ? 0x01 : 0) | (atari_enabled ? 0x02 : 0);
    memcpy(b + 10, pub_mac, 6);
    put32(b + 16, pub_ip);
    put32(b + 20, pub_mask);
    put32(b + 24, pub_gw);
    put32(b + 28, atari_ip);
    put32(b + 32, atari_mask);
    put16(b + 36, 1500);                    /* MTU                       */
    b[38] = 3;                              /* max sectors NET_RX        */
    b[39] = 3;                              /* max sectors NET_TX        */
    /* 40..59: ring sizes and frame counters, 0 until the bridge exists */
    put32(b + 60, ctrl_count);
    strncpy((char *)b + 64, FW_VERSION_STR, 31);
    put32(b + 96, ctrl_last);
}

/* NET_CTRL (vendor sub 0x21), 512 bytes from the Atari:
   "ATN", 0, version.w, command.w (0 off, 1 on), ip.l, mask.l
   Returns 0 when stored, else an error code (also kept for NET_INFO). */
uint32_t net_ctrl(const uint8_t *b)         /* core1 */
{
    uint32_t err = 0;
    uint32_t cmd = get16(b + 6), ip = get32(b + 8), mask = get32(b + 12);
    ctrl_count++;
    if (memcmp(b, "ATN", 4) || get16(b + 4) != NET_PROTO_VERSION) err = 1;  /* not for us  */
    else if (cmd == 0) atari_enabled = false;
    else if (cmd != 1) err = 2;                                              /* unknown     */
    else if (!ip || ip == 0xffffffffu || !mask || (~mask & (~mask + 1))) err = 3; /* bad IP/mask */
    else if (pub_link && ip == pub_ip) err = 4;                              /* Pico's IP   */
    else {
        atari_ip = ip;
        atari_mask = mask;
        atari_enabled = true;
    }
    ctrl_last = err;
    return err;
}

/* the Atari was reset: its network settings are gone with STinG */
void net_bridge_reset(void)                 /* core1 */
{
    atari_enabled = false;
}

/* NET_TEST (vendor sub 0x2f): n sectors of a known pattern, for the short
   DMA read test of NETTEST. Byte i: "ATT", n, then (i ^ i >> 8 ^ 0xa5). */
void net_test_pattern(uint8_t *b, uint32_t n)   /* core1 */
{
    for (uint32_t i = 0; i < n * 512; i++) b[i] = (uint8_t)(i ^ i >> 8 ^ 0xa5);
    memcpy(b, "ATT", 3);
    b[3] = (uint8_t)n;
}

static void ip_str(char *s, uint32_t ip)
{
    sprintf(s, "%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24), (unsigned long)(ip >> 16 & 255),
            (unsigned long)(ip >> 8 & 255), (unsigned long)(ip & 255));
}

void net_bridge_console(void)               /* core0: 'w' */
{
    char a[16], m[16];
    ip_str(a, atari_ip);
    ip_str(m, atari_mask);
    printf("Atari network (ACSI_NET, phase 1: settings only)\n");
    printf("  state      : %s\n", atari_enabled ? "enabled by the Atari" : "off");
    printf("  Atari IP   : %s / %s\n", a, m);
    printf("  NET_CTRL   : %lu calls, last result %lu\n",
           (unsigned long)ctrl_count, (unsigned long)ctrl_last);
}
