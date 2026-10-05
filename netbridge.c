/*
 * ACSI_NET: network interface for STinG over ACSI (see ACSI_NET-ontwerp.md).
 *
 * The Atari shares the Pico's MAC address and has its own IP address. When
 * the Atari enables the bridge (NET_CTRL), a filter in front of lwIP's
 * netif input hands frames for the Atari's IP (IPv4 and ARP) to a receive
 * ring; everything else still goes to lwIP (TNFS, NTP, DHCP). Frames from
 * the Atari go through a transmit ring and are sent with
 * cyw43_send_ethernet(). Without NET_CTRL enable the filter is not even
 * installed: the network behaves exactly as before.
 *
 * Threads: core1 serves the ACSI commands and only touches its end of the
 * rings, it never waits for core0. On core0 the filter runs in the cyw43
 * async context (from the Wi-Fi driver), and so does a "when pending"
 * worker that empties the transmit ring and installs or removes the filter.
 * core0's main loop (TNFS) may be busy for seconds without stopping either.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "acsi.h"

#if BOARD_HAS_WIFI
#include "pico/cyw43_arch.h"
#include "pico/async_context.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#endif

#define NET_PROTO_VERSION   1
#define NET_CAP_CTRL        0x0001          /* NET_CTRL stores settings   */
#define NET_CAP_FRAMES      0x0002          /* NET_TX / NET_RX work        */
#define NET_CAP_TEST        0x8000          /* NET_TEST short read test    */

#define FRAME_MAX   1514                    /* Ethernet frame without FCS */
#define HDR         8                       /* NET_TX / NET_RX header     */
#define SLOT        1536                    /* 3 sectors: HDR + FRAME_MAX */
#define RX_SLOTS    8                       /* powers of two              */
#define TX_SLOTS    4

bool net_link_up(void);                     /* net.c */

/* written by core0, read by core1 */
static volatile bool     pub_link;
static volatile uint32_t pub_ip, pub_mask, pub_gw;
static uint8_t           pub_mac[6];

/* written by core1 (NET_CTRL), read by core0 */
static volatile bool     atari_enabled;
static volatile uint32_t atari_ip, atari_mask;
static volatile uint32_t ctrl_count, ctrl_last;

/* receive ring: core0 (filter) fills, core1 (NET_RX) empties */
static uint8_t           rx_buf[RX_SLOTS][SLOT];
static volatile uint16_t rx_len[RX_SLOTS];
static volatile uint32_t rx_head, rx_tail;

/* transmit ring: core1 (NET_TX) fills, core0 (worker) empties */
static uint8_t           tx_buf[TX_SLOTS][SLOT];
static volatile uint16_t tx_len[TX_SLOTS];
static volatile uint32_t tx_head, tx_tail;

/* statistics */
static volatile uint32_t st_rx, st_rx_drop, st_rx_big, st_rx_poll;
static volatile uint32_t st_tx, st_tx_busy, st_tx_bad, st_tx_pico, st_tx_err;

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return get16(p) << 16 | get16(p + 2); }

/* timeline of the last frames, for finding where time goes (console W):
   one ring per core, so each has a single writer */
enum { TR_RX_IN, TR_RX_OUT, TR_TX_IN, TR_TX_OUT };
typedef struct { uint32_t t; uint32_t val; uint16_t len; uint8_t type, flags; } trace_t;
#define TR_N 256
static trace_t tr0[TR_N], tr1[TR_N];        /* core0: Wi-Fi side, core1: ACSI side */
static volatile uint32_t tr0_n, tr1_n;

/* TCP: sequence number (frames to the Atari) or acknowledgement (frames
   from it), low 32 bits, and the flags; other frames: 0 */
static void trace(trace_t *ring, volatile uint32_t *n, uint8_t type, const uint8_t *f, uint32_t len)
{
    trace_t *e = &ring[*n % TR_N];
    e->t = time_us_32();
    e->type = type;
    e->len = (uint16_t)len;
    e->val = 0;
    e->flags = 0;
    if (len >= 54 && f[12] == 0x08 && f[13] == 0x00 && f[23] == 6) {
        uint32_t ihl = (f[14] & 15) * 4;
        const uint8_t *t = f + 14 + ihl;
        if (14 + ihl + 20 <= len) {
            bool rx = type == TR_RX_IN || type == TR_RX_OUT;
            const uint8_t *v = t + (rx ? 4 : 8);
            e->val = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
            e->flags = t[13];
        }
    }
    __dmb();
    (*n)++;
}

/* lwIP keeps addresses in network order: as a number, a.b.c.d = 0xaabbccdd */
static uint32_t ip_num(uint32_t lwip) { return __builtin_bswap32(lwip); }

/* ---------------- core0: filter, transmit worker ---------------- */
#if BOARD_HAS_WIFI

static netif_input_fn lwip_input;           /* lwIP's own input, while filtered */
static bool filter_on, worker_added;

/* destination of a frame for the Atari: its IP in IPv4 or as ARP target */
static bool for_atari(const uint8_t *f, uint32_t len)
{
    uint32_t type = get16(f + 12);
    if (type == 0x0800 && len >= 34) return get32(f + 30) == atari_ip;
    if (type == 0x0806 && len >= 42) return get32(f + 38) == atari_ip;
    return false;
}

static err_t bridge_input(struct pbuf *p, struct netif *inp)
{
    uint8_t h[42];
    uint32_t len = p->tot_len;
    if (!atari_enabled || len < 14 || pbuf_copy_partial(p, h, len < 42 ? len : 42, 0) < 14 ||
        !for_atari(h, len))
        return lwip_input(p, inp);
    if (len > FRAME_MAX) st_rx_big++;
    else if (rx_head - rx_tail >= RX_SLOTS) st_rx_drop++;      /* full: drop the newest */
    else {
        uint32_t s = rx_head % RX_SLOTS;
        pbuf_copy_partial(p, rx_buf[s] + HDR, (u16_t)len, 0);
        rx_len[s] = (uint16_t)len;
        trace(tr0, &tr0_n, TR_RX_IN, rx_buf[s] + HDR, len);
        __dmb();
        rx_head++;
        st_rx++;
    }
    pbuf_free(p);
    return ERR_OK;
}

/* async context (cyw43 lock held): send what the Atari queued, and follow
   NET_CTRL / reset: filter and power mode only while the bridge is on */
static void bridge_work(async_context_t *ctx, async_when_pending_worker_t *w)
{
    (void)ctx; (void)w;
    struct netif *n = netif_default;
    bool on = atari_enabled && n;
    if (on && !filter_on) {
        lwip_input = n->input;
        n->input = bridge_input;
        cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
        filter_on = true;
    } else if (!on && filter_on) {
        if (n) n->input = lwip_input;
        cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM);
        filter_on = false;
    }
    while (tx_tail != tx_head) {
        uint32_t s = tx_tail % TX_SLOTS;
        if (on && pub_link) {
            trace(tr0, &tr0_n, TR_TX_OUT, tx_buf[s] + HDR, tx_len[s]);
            if (cyw43_send_ethernet(&cyw43_state, CYW43_ITF_STA, tx_len[s], tx_buf[s] + HDR, false) == 0)
                st_tx++;
            else
                st_tx_err++;
        }
        __dmb();
        tx_tail++;
    }
}

static async_when_pending_worker_t worker = { .do_work = bridge_work };

static void kick(void)                      /* any core */
{
    if (worker_added) async_context_set_work_pending(cyw43_arch_async_context(), &worker);
}

#else
static void kick(void) { }
#endif

void net_bridge_publish(void)               /* core0 main loop */
{
#if BOARD_HAS_WIFI
    struct netif *n = netif_default;
    if (n) {
        pub_ip = ip_num(ip4_addr_get_u32(netif_ip4_addr(n)));
        pub_mask = ip_num(ip4_addr_get_u32(netif_ip4_netmask(n)));
        pub_gw = ip_num(ip4_addr_get_u32(netif_ip4_gw(n)));
        memcpy(pub_mac, n->hwaddr, 6);
        if (!worker_added) {                /* the cyw43 context exists now */
            async_context_add_when_pending_worker(cyw43_arch_async_context(), &worker);
            worker_added = true;
            kick();
        }
    }
    pub_link = net_link_up();
#endif
}

/* ---------------- core1: ACSI commands ---------------- */

/* NET_INFO (vendor sub 0x20), 512 bytes, big-endian */
void net_info(uint8_t *b)
{
    memset(b, 0, 512);
    memcpy(b, "ATN", 3);
    put16(b + 4, NET_PROTO_VERSION);
    put16(b + 6, NET_CAP_CTRL | (BOARD_HAS_WIFI ? NET_CAP_FRAMES : 0) | NET_CAP_TEST);
    b[8] = (pub_link ? 0x01 : 0) | (atari_enabled ? 0x02 : 0);
    memcpy(b + 10, pub_mac, 6);
    put32(b + 16, pub_ip);
    put32(b + 20, pub_mask);
    put32(b + 24, pub_gw);
    put32(b + 28, atari_ip);
    put32(b + 32, atari_mask);
    put16(b + 36, 1500);                    /* MTU                */
    b[38] = SLOT / 512;                     /* max sectors NET_RX */
    b[39] = SLOT / 512;                     /* max sectors NET_TX */
    b[40] = RX_SLOTS;
    b[41] = TX_SLOTS;
    put32(b + 44, st_rx);
    put32(b + 48, st_tx);
    put32(b + 52, st_rx_drop + st_rx_big);
    put32(b + 56, st_tx_busy + st_tx_bad + st_tx_err);
    put32(b + 60, ctrl_count);
    strncpy((char *)b + 64, FW_VERSION_STR, 31);
    put32(b + 96, ctrl_last);
}

/* NET_CTRL (vendor sub 0x21), 512 bytes from the Atari:
   "ATN", 0, version.w, command.w (0 off, 1 on), ip.l, mask.l
   Returns 0 when stored, else an error code (also kept for NET_INFO). */
uint32_t net_ctrl(const uint8_t *b)
{
    uint32_t err = 0;
    uint32_t cmd = get16(b + 6), ip = get32(b + 8), mask = get32(b + 12);
    ctrl_count++;
    if (memcmp(b, "ATN", 4) || get16(b + 4) != NET_PROTO_VERSION) err = 1;  /* not for us  */
    else if (cmd == 0) {
        atari_enabled = false;
        rx_tail = rx_head;                                                  /* nothing left */
    }
    else if (cmd != 1) err = 2;                                              /* unknown     */
    else if (!ip || ip == 0xffffffffu || !mask || (~mask & (~mask + 1))) err = 3; /* bad IP/mask */
    else if (pub_link && ip == pub_ip) err = 4;                              /* Pico's IP   */
    else {
        if (!atari_enabled || ip != atari_ip) rx_tail = rx_head;            /* old frames  */
        atari_ip = ip;
        atari_mask = mask;
        atari_enabled = true;
    }
    ctrl_last = err;
    kick();
    return err;
}

/* the Atari was reset: STinG is gone, so is everything queued for it */
void net_bridge_reset(void)
{
    atari_enabled = false;
    rx_tail = rx_head;
    kick();                                 /* worker: filter off, drop the TX ring */
}

/* NET_RX (vendor sub 0x23): the next frame for the Atari, in place.
   Returns the buffer to send and its size in sectors (header + frame);
   with nothing queued it fills 'empty' (one sector, frame_len 0). */
uint8_t *net_rx_next(uint8_t *empty, uint32_t max_sectors, uint32_t *sectors)
{
    st_rx_poll++;
    while (rx_tail != rx_head) {
        uint32_t s = rx_tail % RX_SLOTS, len = rx_len[s];
        uint32_t n = (HDR + len + 511) / 512;
        if (n > max_sectors) {              /* the Atari cannot take it */
            st_rx_big++;
            rx_tail++;
            continue;
        }
        uint8_t *b = rx_buf[s];
        put16(b, len);
        put16(b + 2, rx_head - rx_tail - 1);
        put16(b + 4, st_rx_drop + st_rx_big);
        put16(b + 6, 0);
        *sectors = n;
        return b;
    }
    memset(empty, 0, 512);
    put16(empty + 4, st_rx_drop + st_rx_big);
    *sectors = 1;
    return empty;
}

void net_rx_sent(uint8_t *b)                /* that frame reached the Atari */
{
    if (rx_tail != rx_head && b == rx_buf[rx_tail % RX_SLOTS]) {
        trace(tr1, &tr1_n, TR_RX_OUT, b + HDR, rx_len[rx_tail % RX_SLOTS]);
        __dmb();
        rx_tail++;
    }
}

/* NET_TX (vendor sub 0x22): a free slot to receive into, NULL if full */
uint8_t *net_tx_slot(void)
{
    if (tx_head - tx_tail >= TX_SLOTS) {
        st_tx_busy++;
        return NULL;
    }
    return tx_buf[tx_head % TX_SLOTS];
}

/* the slot holds 'sectors' sectors from the Atari: check and queue it.
   0 queued (or dropped on purpose: for the Pico itself), else an error */
uint32_t net_tx_commit(uint8_t *b, uint32_t sectors)
{
    uint32_t len = get16(b);
    const uint8_t *f = b + HDR;
    if (!atari_enabled || len < 14 || len > FRAME_MAX || HDR + len > sectors * 512 ||
        memcmp(f + 6, pub_mac, 6)) {        /* source must be our (shared) MAC */
        st_tx_bad++;
        return 1;
    }
    uint32_t type = get16(f + 12);
    if ((type == 0x0800 && len >= 34 && get32(f + 30) == pub_ip) ||
        (type == 0x0806 && len >= 42 && get32(f + 38) == pub_ip)) {
        st_tx_pico++;                       /* would not come back over the air */
        return 0;
    }
    tx_len[tx_head % TX_SLOTS] = (uint16_t)len;
    trace(tr1, &tr1_n, TR_TX_IN, f, len);
    __dmb();
    tx_head++;
    kick();
    return 0;
}

/* NET_TEST (vendor sub 0x2f): n sectors of a known pattern, for the short
   DMA read test of NETTEST. Byte i: "ATT", n, then (i ^ i >> 8 ^ 0xa5). */
void net_test_pattern(uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n * 512; i++) b[i] = (uint8_t)(i ^ i >> 8 ^ 0xa5);
    memcpy(b, "ATT", 3);
    b[3] = (uint8_t)n;
}

/* ---------------- console ---------------- */

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
    printf("Atari network (ACSI_NET)\n");
    printf("  state    : %s%s\n", atari_enabled ? "bridge on" : "off",
#if BOARD_HAS_WIFI
           filter_on ? ", filter installed, Wi-Fi performance mode" : "");
#else
           "");
#endif
    printf("  Atari IP : %s / %s\n", a, m);
    printf("  NET_CTRL : %lu calls, last result %lu\n",
           (unsigned long)ctrl_count, (unsigned long)ctrl_last);
    printf("  RX       : %lu frames to the Atari, %lu dropped (ring full), %lu too big, "
           "%lu queued, %lu polls\n", (unsigned long)st_rx, (unsigned long)st_rx_drop,
           (unsigned long)st_rx_big, (unsigned long)(rx_head - rx_tail), (unsigned long)st_rx_poll);
    printf("  TX       : %lu frames sent, %lu busy (ring full), %lu refused, %lu for the Pico, "
           "%lu send errors\n", (unsigned long)st_tx, (unsigned long)st_tx_busy,
           (unsigned long)st_tx_bad, (unsigned long)st_tx_pico, (unsigned long)st_tx_err);
}

/* console W: the last frames of both rings, merged by time. Columns: time
   since the first event (ms), gap to the previous one (ms), event, length,
   TCP seq (to the Atari) or ack (from it), polls. */
void net_bridge_trace(void)
{
    static const char *name[] = { "wifi->pico ", "pico->atari", "atari->pico", "pico->wifi " };
    uint32_t n0 = tr0_n, n1 = tr1_n;
    uint32_t i0 = n0 > TR_N ? n0 - TR_N : 0, i1 = n1 > TR_N ? n1 - TR_N : 0;
    uint32_t t0 = 0, prev = 0;
    bool first = true;
    printf("frame timeline (last %d per side), polls so far %lu\n", TR_N, (unsigned long)st_rx_poll);
    while (i0 < n0 || i1 < n1) {
        const trace_t *a = i0 < n0 ? &tr0[i0 % TR_N] : NULL, *b = i1 < n1 ? &tr1[i1 % TR_N] : NULL;
        const trace_t *e = !b || (a && (int32_t)(a->t - b->t) <= 0) ? a : b;
        if (e == a) i0++; else i1++;
        if (first) { t0 = prev = e->t; first = false; }
        printf("%8.3f %+7.3f %s %4u", (e->t - t0) / 1000.0, (e->t - prev) / 1000.0, name[e->type], e->len);
        if (e->flags) printf("  %s %08lx %02x", e->type <= TR_RX_OUT ? "seq" : "ack", (unsigned long)e->val, e->flags);
        printf("\n");
        prev = e->t;
    }
}
