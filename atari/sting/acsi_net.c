/*
 * ACSI_NET.STX - STinG port driver for the network function of ACSI2TNFS
 * (see ACSI_NET-ontwerp.md). Compiled with -mshort: STinG passes 16-bit ints.
 *
 * The adapter's Pico bridges Ethernet frames to Wi-Fi; the Atari uses the
 * Pico's MAC address and its own IP address (the port's). This module does
 * what an Ethernet driver for STinG does: ARP (cache, requests, answers),
 * IP datagrams to and from frames. Frames go over ACSI with the vendor
 * commands NET_TX and NET_RX (acsinet.c).
 *
 * The structure and the ARP handling follow USB_NET.STX (usbsting) by Roger
 * Burrows and Christian Zietz, itself based on SCSILINK code.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version. See LICENSE.
 */
#include <osbind.h>
#include <mint/basepage.h>

#define cdecl
#define NULL ((void *)0)
typedef BASEPAGE BASPAG;                    /* Pure C name used by the headers */
#include "transprt.h"
#include "port.h"
#include "acsinet.h"

#define DRIVER_NAME   "ACSI_NET.STX"
#define PORT_NAME     "ACSI2TNFS"
#define VERSION       "00.04"
#define MOD_DATE      (((2026 - 1980) << 9) | (10 << 5) | 4)

#define MAX_FRAMES_PER_POLL 4               /* receive: frames per STinG call */
#define MAX_SENDS_PER_POLL  4               /* send: datagrams per STinG call */
#define ARP_RETRY_MS        1000L           /* ARP request again after 1 s     */

#define ETH_ALEN    6
#define ETH_HLEN    14
#define ETH_MIN     60
#define FRAME_MAX   1514
#define HDR         8                       /* NET_TX / NET_RX header           */
#define TYPE_IP     0x0800
#define TYPE_ARP    0x0806

typedef struct {
    uint16 hw_space, proto_space;
    uint8  hw_len, proto_len;
    uint16 op;
    uint8  src_ether[ETH_ALEN];
    uint32 src_ip;
    uint8  dst_ether[ETH_ALEN];
    uint32 dst_ip;
} ARP;                                      /* 28 bytes, 68000 alignment ok */

#define ARP_REQ 1
#define ARP_ANS 2

TPL *tpl;
STX *stx;

static int16 cdecl set_state(PORT *port, int16 state);
static int16 cdecl cntrl(PORT *port, uint32 arg, int16 code);
static void cdecl send_dgrams(PORT *port);
static void cdecl receive_dgrams(PORT *port);

static DRIVER my_driver = {
    set_state, cntrl, send_dgrams, receive_dgrams,
    PORT_NAME, VERSION, MOD_DATE, "ACSI2TNFS project", NULL, NULL
};

static PORT my_port = {
    PORT_NAME, L_SER_BUS, FALSE, 0L, 0xffffffffUL, 0xffffffffUL,
    1500, 1500, 0L, NULL, 0L, NULL, 0, &my_driver, NULL
};

static uint8 mac[ETH_ALEN];
static uint16 txw[3 * 256], rxw[3 * 256];   /* DMA buffers: ST-RAM, even */
#define txbuf ((uint8 *)txw)
#define rxbuf ((uint8 *)rxw)

static IP_DGRAM *arpwait;                   /* datagrams waiting for ARP */
static uint32 arp_asked_ip;                 /* last ARP request ...      */
static int32  arp_asked_ms;                 /* ... and when              */

/* counters (the Pico counts too: console 'w') */
static uint32 st_rx_frames, st_rx_locked, st_rx_err, st_rx_bad;
static uint32 st_tx_frames, st_tx_busy, st_tx_err, st_arp_req, st_arp_ans;

/* ---------------- small helpers, no C library (-mshort) ---------------- */

void *memcpy(void *d, const void *s, unsigned long n)
{
    char *dp = d;
    const char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memset(void *d, int c, unsigned long n)
{
    char *dp = d;
    while (n--) *dp++ = (char)c;
    return d;
}

static int same(const void *a, const void *b, int n)
{
    const char *x = a, *y = b;
    while (n--) if (*x++ != *y++) return 0;
    return 1;
}

static int xstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static void put16(uint8 *p, uint16 v) { p[0] = (uint8)(v >> 8); p[1] = (uint8)v; }

static void put32(uint8 *p, uint32 v)
{
    put16(p, (uint16)(v >> 16));
    put16(p + 2, (uint16)v);
}

static uint16 get16(const uint8 *p) { return (uint16)(p[0] << 8 | p[1]); }

static void say(const char *s)
{
    while (*s) {
        if (*s == '\n') Bconout(2, '\r');
        Bconout(2, *s++);
    }
}

/* ---------------- ARP cache ---------------- */

#define ARP_NUM 16

static struct { uint32 ip; uint8 ether[ETH_ALEN]; int16 used; } arp_tab[ARP_NUM];
static int16 arp_next;

static uint8 *arp_lookup(uint32 ip)
{
    int16 i;
    for (i = 0; i < ARP_NUM; i++)
        if (arp_tab[i].used && arp_tab[i].ip == ip) return arp_tab[i].ether;
    return NULL;
}

static void arp_store(uint32 ip, const uint8 *ether)
{
    uint8 *e = arp_lookup(ip);
    if (!e) {                               /* oldest entry makes room */
        arp_tab[arp_next].ip = ip;
        arp_tab[arp_next].used = 1;
        e = arp_tab[arp_next].ether;
        arp_next = (int16)((arp_next + 1) % ARP_NUM);
    }
    memcpy(e, ether, ETH_ALEN);
}

/* ---------------- frames over ACSI (STinG thread) ---------------- */

/* frame of len bytes at txbuf + HDR: ST_OK, ST_BUSY / ST_LOCKED (try again
   next time) or an error */
static long send_frame(uint16 len)
{
    int sectors;
    long r;
    if (len < ETH_MIN) {
        memset(txbuf + HDR + len, 0, ETH_MIN - len);
        len = ETH_MIN;
    }
    put16(txbuf, len);
    put16(txbuf + 2, 0);
    put16(txbuf + 4, 0);
    put16(txbuf + 6, 0);
    sectors = (HDR + len + 511) / 512;
    r = acsi_net(NET_TX, (unsigned char)sectors, txbuf, sectors, 1, 0);
    if (r == ST_OK) st_tx_frames++;
    else if (r == ST_BUSY || r == ST_LOCKED) st_tx_busy++;
    else st_tx_err++;
    return r;
}

static long send_arp(uint16 op, const uint8 *dst_ether, uint32 dst_ip)
{
    ARP *a = (ARP *)(txbuf + HDR + ETH_HLEN);
    memcpy(txbuf + HDR, op == ARP_REQ ? (const uint8 *)"\377\377\377\377\377\377" : dst_ether, ETH_ALEN);
    memcpy(txbuf + HDR + 6, mac, ETH_ALEN);
    put16(txbuf + HDR + 12, TYPE_ARP);
    a->hw_space = 1;
    a->proto_space = TYPE_IP;
    a->hw_len = ETH_ALEN;
    a->proto_len = 4;
    a->op = op;
    memcpy(a->src_ether, mac, ETH_ALEN);
    a->src_ip = my_port.ip_addr;
    if (op == ARP_REQ) memset(a->dst_ether, 0, ETH_ALEN);
    else memcpy(a->dst_ether, dst_ether, ETH_ALEN);
    a->dst_ip = dst_ip;
    if (op == ARP_REQ) st_arp_req++; else st_arp_ans++;
    return send_frame(ETH_HLEN + sizeof(ARP));
}

/* one datagram: 1 sent (or dropped on purpose), 0 not now (keep it),
   -1 cannot be sent (discard), -2 waiting for ARP (put on arpwait) */
static int16 output(IP_DGRAM *dg)
{
    uint32 net = my_port.ip_addr & my_port.sub_mask, hop;
    uint8 *ether, *p;
    uint16 len = (uint16)(ETH_HLEN + sizeof(IP_HDR) + dg->opt_length + dg->pkt_length);
    long r;

    if (len > FRAME_MAX) return -1;
    if ((dg->hdr.ip_dest & ~my_port.sub_mask) == ~my_port.sub_mask)
        ether = (uint8 *)"\377\377\377\377\377\377";           /* subnet broadcast */
    else {
        if ((dg->hdr.ip_dest & my_port.sub_mask) == net) hop = dg->hdr.ip_dest;
        else if ((dg->ip_gateway & my_port.sub_mask) == net) hop = dg->ip_gateway;
        else return -1;                                         /* no route on this net */
        ether = arp_lookup(hop);
        if (!ether) {
            int32 now = TIMER_now();
            if (hop != arp_asked_ip || now - arp_asked_ms > ARP_RETRY_MS) {
                if (send_arp(ARP_REQ, NULL, hop) == ST_OK) {
                    arp_asked_ip = hop;
                    arp_asked_ms = now;
                }
            }
            return -2;
        }
    }
    p = txbuf + HDR;
    memcpy(p, ether, ETH_ALEN);
    memcpy(p + 6, mac, ETH_ALEN);
    put16(p + 12, TYPE_IP);
    memcpy(p + ETH_HLEN, &dg->hdr, sizeof(IP_HDR));
    memcpy(p + ETH_HLEN + sizeof(IP_HDR), dg->options, dg->opt_length);
    memcpy(p + ETH_HLEN + sizeof(IP_HDR) + dg->opt_length, dg->pkt_data, dg->pkt_length);
    r = send_frame(len);
    if (r == ST_BUSY || r == ST_LOCKED) return 0;
    if (r != ST_OK) return -1;
    my_port.stat_sd_data += len - ETH_HLEN;
    return 1;
}

static void queue_tail(IP_DGRAM **q, IP_DGRAM *dg)
{
    dg->next = NULL;
    while (*q) q = &(*q)->next;
    *q = dg;
}

/* send from 'queue': stop at the first datagram that has to wait */
static void flush(IP_DGRAM **queue)
{
    int n;
    for (n = 0; n < MAX_SENDS_PER_POLL && *queue; n++) {
        IP_DGRAM *dg = *queue, *next = dg->next;
        int16 r;
        if (check_dgram_ttl(dg) != E_NORMAL) {      /* expired: STinG discarded it */
            *queue = next;
            continue;
        }
        r = output(dg);
        if (r == 0) return;                         /* adapter busy: next time */
        *queue = dg->next;
        if (r == 1) IP_discard(dg, TRUE);
        else if (r == -2) queue_tail(&arpwait, dg);
        else { IP_discard(dg, TRUE); my_port.stat_dropped++; }
    }
}

static void cdecl send_dgrams(PORT *port)
{
    if (port != &my_port || !my_port.active) return;
    if (arpwait) {                                  /* an answer may have come */
        IP_DGRAM *w = arpwait;
        arpwait = NULL;
        while (w) {                                 /* back in front of the queue */
            IP_DGRAM *next = w->next;
            IP_DGRAM **q = &my_port.send;
            w->next = *q;
            *q = w;
            w = next;
        }
    }
    flush(&my_port.send);
}

static void input_arp(const ARP *a)
{
    if (a->hw_space != 1 || a->proto_space != TYPE_IP || a->hw_len != ETH_ALEN || a->proto_len != 4)
        { st_rx_bad++; return; }
    if (a->src_ip) arp_store(a->src_ip, a->src_ether);
    if (a->op == ARP_REQ && a->dst_ip == my_port.ip_addr)
        send_arp(ARP_ANS, a->src_ether, a->src_ip);
}

static void input_ip(const uint8 *ip, uint16 len)
{
    const IP_HDR *h = (const IP_HDR *)ip;
    IP_DGRAM *dg;
    int16 hlen = (int16)(h->hd_len * 4);

    if (len < sizeof(IP_HDR) || h->version != 4 || hlen < (int16)sizeof(IP_HDR) ||
        h->length > len || hlen > (int16)h->length) { st_rx_bad++; return; }
    dg = KRmalloc(sizeof(IP_DGRAM));
    if (!dg) { my_port.stat_dropped++; return; }
    memcpy(&dg->hdr, ip, sizeof(IP_HDR));
    dg->opt_length = (int16)(hlen - sizeof(IP_HDR));
    dg->pkt_length = (int16)(h->length - hlen);
    dg->options = KRmalloc(dg->opt_length ? dg->opt_length : 1);
    dg->pkt_data = KRmalloc(dg->pkt_length ? dg->pkt_length : 1);
    if (!dg->options || !dg->pkt_data) {
        IP_discard(dg, TRUE);
        my_port.stat_dropped++;
        return;
    }
    memcpy(dg->options, ip + sizeof(IP_HDR), dg->opt_length);
    memcpy(dg->pkt_data, ip + hlen, dg->pkt_length);
    dg->recvd = &my_port;
    dg->next = NULL;
    set_dgram_ttl(dg);
    queue_tail(&my_port.receive, dg);
    my_port.stat_rcv_data += h->length;
}

static void cdecl receive_dgrams(PORT *port)
{
    int n;
    if (port != &my_port || !my_port.active) return;
    for (n = 0; n < MAX_FRAMES_PER_POLL; n++) {
        long r = acsi_net(NET_RX, 3, rxbuf, 3, 0, 0);
        uint16 len, type;
        if (r == ST_LOCKED) { st_rx_locked++; return; }     /* disk busy: next time */
        if (r != ST_OK) { st_rx_err++; return; }
        len = get16(rxbuf);
        if (!len) return;
        st_rx_frames++;
        type = get16(rxbuf + HDR + 12);
        if (len < ETH_HLEN + 20) st_rx_bad++;
        else if (type == TYPE_ARP) input_arp((const ARP *)(rxbuf + HDR + ETH_HLEN));
        else if (type == TYPE_IP) input_ip(rxbuf + HDR + ETH_HLEN, (uint16)(len - ETH_HLEN));
        if (!get16(rxbuf + 2)) return;                      /* nothing more queued */
    }
}

/* ---------------- process context (load, CPX) ---------------- */

static long sv_result;
static unsigned char sv_sub;
static int sv_write;

static long sv_find(void) { return sv_result = acsi_find(); }

static long sv_cmd(void)
{
    return sv_result = acsi_net(sv_sub, 0, txbuf, 1, sv_write, 1);
}

static long cmd(unsigned char sub, int write)
{
    sv_sub = sub;
    sv_write = write;
    Supexec(sv_cmd);
    return sv_result;
}

static void drop_all(IP_DGRAM **q)
{
    while (*q) {
        IP_DGRAM *next = (*q)->next;
        IP_discard(*q, TRUE);
        *q = next;
    }
}

static int16 cdecl set_state(PORT *port, int16 state)
{
    if (port != &my_port) return FALSE;
    memset(txbuf, 0, 512);
    memcpy(txbuf, "ATN", 4);
    txbuf[5] = 1;                           /* protocol version 1   */
    txbuf[7] = state ? 1 : 0;               /* 1 bridge on, 0 off   */
    put32(txbuf + 8, port->ip_addr);
    put32(txbuf + 12, port->sub_mask);
    if (cmd(NET_CTRL, 1) != ST_OK && state)
        return FALSE;                       /* refused: stay off    */
    if (!state) {
        drop_all(&arpwait);
        drop_all(&my_port.send);
        drop_all(&my_port.receive);
    }
    memset(arp_tab, 0, sizeof arp_tab);
    arp_asked_ip = 0;
    return TRUE;
}

static int16 cdecl cntrl(PORT *port, uint32 arg, int16 code)
{
    if (port != &my_port) return E_PARAMETER;
    switch (code) {
    case CTL_ETHER_GET_MAC:
        memcpy((void *)arg, mac, ETH_ALEN);
        return E_NORMAL;
    default:
        return E_FNAVAIL;
    }
}

/* ---------------- start ---------------- */

static long get_sting_cookie(void)
{
    long *p;
    for (p = *(long **)0x5a0L; p && *p; p += 2)
        if (*p == 0x5354694bL)              /* 'STiK' */
            return p[1];
    return 0;
}

static void quit(const char *s)
{
    say(DRIVER_NAME ": ");
    say(s);
    Pterm(-1);
}

void _init(BASEPAGE *bp)
{
    DRV_LIST *sting;
    PORT *ports;
    DRIVER *drv;
    long size = (long)bp->p_bbase + bp->p_blen - (long)bp;

    bp->p_cmdlin[1 + (unsigned char)bp->p_cmdlin[0]] = '\0';
    if (xstrcmp(bp->p_cmdlin + 1, "STinG_Load") != 0)
        quit("STinG module, started by STinG only\n");
    sting = (DRV_LIST *)Supexec(get_sting_cookie);
    if (!sting || xstrcmp(sting->magic, MAGIC) != 0)
        quit("STinG not found\n");
    tpl = (TPL *)(*sting->get_dftab)(TRANSPORT_DRIVER);
    stx = (STX *)(*sting->get_dftab)(MODULE_DRIVER);
    if (!tpl || !stx)
        quit("STinG module tables not found\n");

    Supexec(sv_find);
    if (sv_result < 0)
        quit("no ACSI2TNFS found on ACSI id 0-7, not installed\n");
    if (cmd(NET_INFO, 0) != ST_OK || !same(txbuf, "ATN", 3))
        quit("ACSI2TNFS firmware without network function, not installed\n");
    if (!(get16(txbuf + 6) & 0x0002))
        quit("ACSI2TNFS cannot pass frames (no Wi-Fi?), not installed\n");
    memcpy(mac, txbuf + 10, ETH_ALEN);

    query_chains((void **)&ports, (void **)&drv, NULL);
    while (ports->next) ports = ports->next;
    ports->next = &my_port;
    while (drv->next) drv = drv->next;
    my_driver.basepage = bp;
    drv->next = &my_driver;

    say(DRIVER_NAME " " VERSION ": port " PORT_NAME " installed\n");
    Ptermres(size, 0);
}
