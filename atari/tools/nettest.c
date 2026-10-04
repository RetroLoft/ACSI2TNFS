/*
 * NETTEST.TTP - test tool for ACSI_NET phase 1 (see ACSI_NET-ontwerp.md)
 *
 *   NETTEST            find the adapter, show NET_INFO, run the short DMA
 *                      read test
 *   NETTEST I          NET_INFO only
 *   NETTEST S [n]      short DMA read test only, n rounds (default 300)
 *   NETTEST C a.b.c.d m.m.m.m   NET_CTRL: store the Atari IP and mask
 *   NETTEST D          NET_CTRL: off
 *   NETTEST U          send an unknown network sub (0x2e): must be refused
 *
 * ACSI is driven directly, as in atari/acsi.inc, in supervisor mode and
 * with flock taken with TAS (as a STinG driver will have to).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <osbind.h>

#define DMADATA   ((volatile unsigned short *)0xffff8604L)
#define DMACTRL   ((volatile unsigned short *)0xffff8606L)
#define DMASTAT   ((volatile unsigned short *)0xffff8606L)
#define DMAHI     ((volatile unsigned char *)0xffff8609L)
#define DMAMID    ((volatile unsigned char *)0xffff860bL)
#define DMALO     ((volatile unsigned char *)0xffff860dL)
#define GPIP      ((volatile unsigned char *)0xfffffa01L)
#define FLOCK     ((volatile short *)0x43eL)
#define HZ200     (*(volatile unsigned long *)0x4baL)

#define NET_INFO  0x20
#define NET_CTRL  0x21
#define NET_TEST  0x2f

#define NL "\r\n"

/* DMA buffer: 3 sectors + guard, even address, ST-RAM (program BSS) */
static unsigned char dmabuf[3 * 512 + 64] __attribute__((aligned(16)));

/* parameters for the supervisor call */
static unsigned char cdb[6];
static unsigned char *a_buf;
static int a_count, a_write;
static long a_result;
static unsigned short a_dmastat;

static int take_flock(void)
{
    unsigned long until = HZ200 + 400;          /* 2 s */
    for (;;) {
        char got;
        __asm__ volatile ("tas 0x43e.w\n\tseq %0" : "=d"(got) : : "cc", "memory");
        if (got) return 1;
        if (HZ200 > until) return 0;
    }
}

static int wait_irq(unsigned long ticks)
{
    unsigned long until = HZ200 + ticks;
    while (*GPIP & 0x20)
        if (HZ200 > until) return 0;
    return 1;
}

/* one ACSI command, as acsi.inc: status byte, -1 timeout, -2 bus busy */
static long acsi_exec(void)
{
    unsigned long a = (unsigned long)a_buf;
    unsigned short dir = a_write ? 0x100 : 0x000;
    long r = -1;
    int i;

    if (!take_flock()) return a_result = -2;
    *DMALO = (unsigned char)a;
    *DMAMID = (unsigned char)(a >> 8);
    *DMAHI = (unsigned char)(a >> 16);
    *DMACTRL = (0x090 | dir) ^ 0x100;           /* toggle R/W: reset DMA, flush FIFO */
    *DMACTRL = 0x090 | dir;                     /* sector count register */
    *DMADATA = (unsigned short)a_count;
    *DMACTRL = 0x088 | dir;                     /* A1 = 0: first byte */
    for (i = 0; i < 5; i++) {
        *(volatile unsigned long *)DMADATA = (unsigned long)cdb[i] << 16 | (0x08a | dir);
        if (!wait_irq(20)) goto out;            /* 100 ms */
    }
    *(volatile unsigned long *)DMADATA = (unsigned long)cdb[5] << 16 | dir;  /* start DMA */
    if (!wait_irq(200)) goto out;               /* 1 s */
    a_dmastat = *DMASTAT;
    *DMACTRL = 0x08a | dir;
    r = *DMADATA & 0xff;
out:
    *DMACTRL = 0x080;                           /* back to the FDC, as TOS leaves it */
    *FLOCK = 0;
    return a_result = r;
}

static long get_hz(void) { return (long)HZ200; }

static int acsi_id = -1;

static long cmd(unsigned char sub, unsigned char arg, void *buf, int count, int write)
{
    cdb[0] = (unsigned char)(0x11 | acsi_id << 5);
    cdb[1] = 'A'; cdb[2] = 'T'; cdb[3] = sub; cdb[4] = arg; cdb[5] = 0;
    a_buf = buf; a_count = count; a_write = write;
    Supexec(acsi_exec);
    return a_result;
}

static long read_sector0(void *buf)
{
    cdb[0] = (unsigned char)(0x08 | acsi_id << 5);
    cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 1; cdb[5] = 0;
    a_buf = buf; a_count = 1; a_write = 0;
    Supexec(acsi_exec);
    return a_result;
}

static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | p[2] << 8 | p[3];
}

static unsigned get16(const unsigned char *p) { return p[0] << 8 | p[1]; }

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void print_ip(const char *label, unsigned long ip)
{
    printf("%s%lu.%lu.%lu.%lu" NL, label, ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

static unsigned long parse_ip(const char *s)
{
    unsigned long ip = 0;
    char *e;
    int i;
    for (i = 0; i < 4; i++) {
        ip = ip << 8 | (strtoul(s, &e, 10) & 255);
        s = *e == '.' ? e + 1 : e;
    }
    return ip;
}

/* find ACSI2TNFS: vendor sub 8 answers "ATL" on every firmware */
static int find_adapter(void)
{
    int id;
    for (id = 0; id < 8; id++) {
        acsi_id = id;
        memset(dmabuf, 0, 512);
        if (cmd(8, 0, dmabuf, 1, 0) == 0 && !memcmp(dmabuf, "ATL", 3)) return id;
    }
    acsi_id = -1;
    return -1;
}

static int net_info(int quiet)
{
    long r;
    memset(dmabuf, 0, 512);
    r = cmd(NET_INFO, 0, dmabuf, 1, 0);
    if (r != 0 || memcmp(dmabuf, "ATN", 3)) {
        printf("Network function not supported by this firmware" NL
               "(NET_INFO: status %ld)" NL, r);
        return 0;
    }
    if (quiet) return 1;
    printf("NET_INFO: protocol %u, capabilities $%04x" NL, get16(dmabuf + 4), get16(dmabuf + 6));
    printf("  firmware  : %.31s" NL, dmabuf + 64);
    printf("  Wi-Fi     : %s" NL, dmabuf[8] & 1 ? "connected" : "not connected");
    printf("  MAC       : %02x:%02x:%02x:%02x:%02x:%02x" NL, dmabuf[10], dmabuf[11],
           dmabuf[12], dmabuf[13], dmabuf[14], dmabuf[15]);
    print_ip("  Pico IP   : ", get32(dmabuf + 16));
    print_ip("  netmask   : ", get32(dmabuf + 20));
    print_ip("  gateway   : ", get32(dmabuf + 24));
    printf("  Atari net : %s" NL, dmabuf[8] & 2 ? "enabled" : "off");
    print_ip("  Atari IP  : ", get32(dmabuf + 28));
    print_ip("  Atari mask: ", get32(dmabuf + 32));
    printf("  MTU %u, max sectors RX %u TX %u" NL, get16(dmabuf + 36), dmabuf[38], dmabuf[39]);
    printf("  NET_CTRL calls %lu, last result %lu" NL, get32(dmabuf + 60), get32(dmabuf + 96));
    return 1;
}

static void net_ctrl(unsigned cmdword, unsigned long ip, unsigned long mask)
{
    long r;
    memset(dmabuf, 0, 512);
    memcpy(dmabuf, "ATN", 4);
    dmabuf[5] = 1;                              /* version 1 */
    dmabuf[7] = (unsigned char)cmdword;
    put32(dmabuf + 8, ip);
    put32(dmabuf + 12, mask);
    r = cmd(NET_CTRL, 0, dmabuf, 1, 1);
    printf("NET_CTRL %s: status %ld (%s)" NL NL, cmdword ? "on" : "off", r,
           r == 0 ? "stored" : r == 2 ? "refused" : "error");
}

/* the Atari asks for 3 sectors, the Pico sends n = 1, 2 or 3 */
static int short_read_test(int rounds)
{
    static unsigned char s0[512];
    unsigned long fails[4] = { 0 }, ticks[4] = { 0 }, dmastat_ok[4] = { 0 };
    unsigned long guard_bad = 0, s0_bad = 0;
    int round, n, i;

    if (read_sector0(s0) != 0) {
        printf("READ(6) sector 0 failed" NL);
        return 0;
    }
    printf("Short DMA read: Atari programs 3 sectors," NL
           "Pico sends 1-3, %d rounds" NL, rounds);
    for (round = 0; round < rounds; round++) {
        for (n = 1; n <= 3; n++) {
            long r, t;
            memset(dmabuf, 0xee, sizeof dmabuf);
            t = Supexec(get_hz);
            r = cmd(NET_TEST, (unsigned char)n, dmabuf, 3, 0);
            ticks[n] += Supexec(get_hz) - t;
            if (r != 0 || memcmp(dmabuf, "ATT", 3) || dmabuf[3] != n) {
                if (!fails[n]++)
                    printf("  n=%d: status %ld, header %02x %02x %02x %02x" NL,
                           n, r, dmabuf[0], dmabuf[1], dmabuf[2], dmabuf[3]);
                continue;
            }
            for (i = 4; i < n * 512; i++)
                if (dmabuf[i] != (unsigned char)(i ^ i >> 8 ^ 0xa5)) break;
            if (i < n * 512) {
                if (!fails[n]++) printf("  n=%d: data wrong from byte %d" NL, n, i);
                continue;
            }
            for (i = n * 512; i < (int)sizeof dmabuf; i++)
                if (dmabuf[i] != 0xee) break;
            if (i < (int)sizeof dmabuf && !guard_bad++)
                printf("  n=%d: buffer changed at byte %d" NL, n, i);
            if (a_dmastat & 1) dmastat_ok[n]++;     /* DMA status bit 0: no error */
        }
        /* the disk path must be unaffected */
        memset(dmabuf, 0, 512);
        if (read_sector0(dmabuf) != 0 || memcmp(dmabuf, s0, 512)) s0_bad++;
    }
    for (n = 1; n <= 3; n++)
        printf("  Pico sends %d: %lu errors, DMA ok %lu/%d, %lu us" NL,
               n, fails[n], dmastat_ok[n], rounds, ticks[n] * 5000UL / rounds);
    printf("  buffer changed beyond the data: %lu" NL, guard_bad);
    printf("  sector 0 errors in between: %lu of %d" NL, s0_bad, rounds);
    return fails[1] + fails[2] + fails[3] + guard_bad + s0_bad == 0;
}

int main(int argc, char **argv)
{
    char op = argc > 1 ? argv[1][0] & ~0x20 : 0;
    int ok = 1;

    printf("\033ENETTEST - ACSI_NET phase 1" NL NL);
    if (find_adapter() < 0) {
        printf("No ACSI2TNFS found on ACSI id 0-7" NL);
        ok = 0;
    } else {
        printf("ACSI2TNFS on ACSI id %d" NL, acsi_id);
        switch (op) {
        case 'I':
            ok = net_info(0);
            break;
        case 'S':
            ok = net_info(1) && short_read_test(argc > 2 ? atoi(argv[2]) : 300);
            break;
        case 'C':
            if (argc < 4) { printf("NETTEST C a.b.c.d m.m.m.m" NL); ok = 0; break; }
            if ((ok = net_info(1)) != 0) {
                net_ctrl(1, parse_ip(argv[2]), parse_ip(argv[3]));
                net_info(0);
            }
            break;
        case 'D':
            if ((ok = net_info(1)) != 0) { net_ctrl(0, 0, 0); net_info(0); }
            break;
        case 'U': {
            long r = cmd(0x2e, 0, dmabuf, 1, 0);
            printf("Unknown network sub $2e: status %ld (%s)" NL, r,
                   r == 2 ? "refused, as it should" : "UNEXPECTED");
            ok = r == 2;
            break;
        }
        default:
            ok = net_info(0) && short_read_test(300);
        }
    }
    printf(NL "%s. Press a key." NL, ok ? "Done" : "Failed");
    Cconin();
    return ok ? 0 : 1;
}
