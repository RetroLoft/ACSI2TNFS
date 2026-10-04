/*
 * ACSI_NET.STX - STinG port driver for the network function of ACSI2TNFS
 * (see ACSI_NET-ontwerp.md). Compiled with -mshort: STinG passes 16-bit ints.
 *
 * Phase 3 (skeleton): the module finds the adapter, adds the port
 * "ACSI2TNFS" to STinG, and switches the Pico's frame bridge with the
 * port (NET_CTRL with the port's IP address and mask). The receive routine
 * already polls NET_RX from the STinG thread - the part that has to live
 * with disk traffic - but drops the frames; datagrams STinG wants to send
 * are dropped too. ARP and IP follow in phase 4.
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
#define VERSION       "00.03"
#define MOD_DATE      (((2026 - 1980) << 9) | (10 << 5) | 4)

#define MAX_FRAMES_PER_POLL 4

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

static uint8 mac[6];
static unsigned char dmabuf[3 * 512];       /* ST-RAM (program BSS) */

/* counters, for the console of the Pico and later NETSTAT */
static uint32 rx_frames, rx_busy, rx_errors, tx_dropped;

/* ---------------- small helpers, no C library (-mshort) ---------------- */

static void *xmemcpy(void *d, const void *s, long n)
{
    char *dp = d;
    const char *sp = s;
    while (n-- > 0) *dp++ = *sp++;
    return d;
}

static int xstrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static void put32(unsigned char *p, uint32 v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

static void say(const char *s)
{
    while (*s) {
        if (*s == '\n') Bconout(2, '\r');
        Bconout(2, *s++);
    }
}

/* ---------------- process context (load, CPX) ---------------- */

static long sv_result;
static unsigned char sv_sub;
static int sv_write;

static long sv_find(void) { return sv_result = acsi_find(); }

static long sv_cmd(void)
{
    return sv_result = acsi_net(sv_sub, 0, dmabuf, 1, sv_write, 1);
}

static long cmd(unsigned char sub, int write)
{
    sv_sub = sub;
    sv_write = write;
    Supexec(sv_cmd);
    return sv_result;
}

static int16 cdecl set_state(PORT *port, int16 state)
{
    int i;
    if (port != &my_port) return FALSE;
    for (i = 0; i < 512; i++) dmabuf[i] = 0;
    xmemcpy(dmabuf, "ATN", 4);
    dmabuf[5] = 1;                          /* protocol version 1   */
    dmabuf[7] = state ? 1 : 0;              /* 1 bridge on, 0 off   */
    put32(dmabuf + 8, port->ip_addr);
    put32(dmabuf + 12, port->sub_mask);
    if (cmd(NET_CTRL, 1) != ST_OK)
        return state ? FALSE : TRUE;        /* refused: stay off    */
    return TRUE;
}

static int16 cdecl cntrl(PORT *port, uint32 arg, int16 code)
{
    if (port != &my_port) return E_PARAMETER;
    switch (code) {
    case CTL_ETHER_GET_MAC:
        xmemcpy((void *)arg, mac, 6);
        return E_NORMAL;
    default:
        return E_FNAVAIL;
    }
}

/* ---------------- STinG thread (supervisor, timer interrupt) ---------------- */

static void cdecl send_dgrams(PORT *port)
{
    if (port != &my_port || !my_port.active) return;
    while (my_port.send) {                  /* phase 3: not sent yet */
        IP_DGRAM *next = my_port.send->next;
        IP_discard(my_port.send, TRUE);
        my_port.send = next;
        my_port.stat_dropped++;
        tx_dropped++;
    }
}

static void cdecl receive_dgrams(PORT *port)
{
    int n;
    if (port != &my_port || !my_port.active) return;
    for (n = 0; n < MAX_FRAMES_PER_POLL; n++) {
        long r = acsi_net(NET_RX, 3, dmabuf, 3, 0, 0);
        unsigned len;
        if (r == ST_LOCKED) { rx_busy++; return; }   /* disk busy: next time */
        if (r != ST_OK) { rx_errors++; return; }
        len = get16(dmabuf);
        if (!len) return;
        rx_frames++;
        my_port.stat_rcv_data += len;       /* phase 3: counted, not used */
        if (!get16(dmabuf + 2)) return;     /* nothing more queued */
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
    if (cmd(NET_INFO, 0) != ST_OK || dmabuf[0] != 'A' || dmabuf[1] != 'T' || dmabuf[2] != 'N')
        quit("ACSI2TNFS firmware without network function, not installed\n");
    if (!(get16(dmabuf + 6) & 0x0002))
        quit("ACSI2TNFS cannot pass frames (no Wi-Fi?), not installed\n");
    xmemcpy(mac, dmabuf + 10, 6);

    query_chains((void **)&ports, (void **)&drv, NULL);
    while (ports->next) ports = ports->next;
    ports->next = &my_port;
    while (drv->next) drv = drv->next;
    my_driver.basepage = bp;
    drv->next = &my_driver;

    say(DRIVER_NAME " " VERSION ": port " PORT_NAME " installed\n");
    Ptermres(size, 0);
}
