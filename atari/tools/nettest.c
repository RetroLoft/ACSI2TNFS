/*
 * NETTEST.TTP - test tool for ACSI_NET phases 1-2 (see ACSI_NET-ontwerp.md)
 *
 *   NETTEST            find the adapter, show NET_INFO, run the short DMA
 *                      read test
 *   NETTEST I          NET_INFO only
 *   NETTEST S [n]      short DMA read test only, n rounds (default 300)
 *   NETTEST C a.b.c.d m.m.m.m   NET_CTRL: store the Atari IP and mask
 *   NETTEST D          NET_CTRL: off
 *   NETTEST U          send an unknown network sub (0x2e): must be refused
 *   NETTEST A [ip [target [s]]]  bridge on, ARP request to the target (router)
 *   NETTEST L [ip [s]]           bridge on, listen, answer ARP for our IP
 *
 * ACSI is driven directly, as in atari/acsi.inc, in supervisor mode and
 * with flock taken with TAS (as a STinG driver will have to).
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <osbind.h>

/* everything on screen also goes to NETTEST.LOG in the current folder
   (on a TNFS drive it can be read on the server right away) */
static FILE *logf;

static int out(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    if (logf) {
        va_start(ap, fmt);
        vfprintf(logf, fmt, ap);
        va_end(ap);
    }
    return n;
}
#define printf out

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

/* ------------------------------------------------------------------------
 * Phase 2: real Ethernet frames through the bridge, while reading files from
 * C: and from a TNFS drive in between (disk and TNFS traffic at the same time)
 * ---------------------------------------------------------------------- */
#define NET_TX  0x22
#define NET_RX  0x23
#define CHUNK   16384L

static unsigned char mymac[6];
static unsigned long my_ip, my_mask, pico_ip, gw_ip;
static unsigned char txbuf[3 * 512] __attribute__((aligned(2)));
static unsigned char *chunkbuf;

typedef struct {
    const char *name;
    int fd;
    long nchunk, next, reads, errors, lastlen;
    unsigned long sum[64];
} FTEST;

static FTEST files[2];

static unsigned long csum(const unsigned char *p, long n)
{
    unsigned long s = 0;
    while (n--) s = (s << 1 | s >> 31) + *p++;
    return s;
}

static long chunk_len(FTEST *f, long size, long k)
{
    long l = size - k * CHUNK;
    return l > CHUNK ? CHUNK : l;
}

/* read the whole file once (before the bridge is on): reference sums */
static void ftest_init(FTEST *f, const char *name)
{
    long size, k;
    memset(f, 0, sizeof *f);
    f->name = name;
    f->fd = (int)Fopen(name, 0);
    if (f->fd < 0) { printf("  %s: not found, skipped" NL, name); return; }
    size = Fseek(0, f->fd, 2);
    f->nchunk = (size + CHUNK - 1) / CHUNK;
    if (f->nchunk > 64) f->nchunk = 64;
    for (k = 0; k < f->nchunk; k++) {
        long l = chunk_len(f, size, k);
        Fseek(k * CHUNK, f->fd, 0);
        if (Fread(f->fd, l, chunkbuf) != l) { f->nchunk = k; break; }
        f->sum[k] = csum(chunkbuf, l) ^ (unsigned long)l << 16;
    }
    f->lastlen = size;
    printf("  %s: %ld bytes, %ld chunks of 16 KB" NL, name, size, f->nchunk);
}

static void ftest_step(FTEST *f)
{
    long k, l;
    if (f->fd < 0 || !f->nchunk) return;
    k = f->next++ % f->nchunk;
    l = chunk_len(f, f->lastlen, k);
    Fseek(k * CHUNK, f->fd, 0);
    f->reads++;
    if (Fread(f->fd, l, chunkbuf) != l || (csum(chunkbuf, l) ^ (unsigned long)l << 16) != f->sum[k])
        if (!f->errors++) printf("  %s: chunk %ld read wrong!" NL, f->name, k);
}

static void ip_txt(char *s, const unsigned char *p)
{
    sprintf(s, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

static void describe(long t_ms, const unsigned char *f, unsigned len)
{
    char a[16], b[16];
    unsigned type = get16(f + 12);
    printf("  %5ld ms  %4u B  from %02x:%02x:%02x:%02x:%02x:%02x  ", t_ms, len,
           f[6], f[7], f[8], f[9], f[10], f[11]);
    if (type == 0x0806 && len >= 42) {
        ip_txt(a, f + 28); ip_txt(b, f + 38);
        printf("ARP %s %s -> %s" NL, get16(f + 20) == 1 ? "request" : "reply  ", a, b);
    } else if (type == 0x0800 && len >= 34) {
        ip_txt(a, f + 26); ip_txt(b, f + 30);
        printf("IPv4 proto %u %s -> %s" NL, f[23], a, b);
    } else
        printf("type $%04x" NL, type);
}

/* NET_TX: header + frame, padded to 60 bytes */
static long send_frame(const unsigned char *f, unsigned len)
{
    int n;
    if (len < 60) len = 60;
    memset(txbuf, 0, sizeof txbuf);
    txbuf[0] = len >> 8; txbuf[1] = len;
    memcpy(txbuf + 8, f, len);
    n = (8 + len + 511) / 512;
    return cmd(NET_TX, (unsigned char)n, txbuf, n, 1);
}

static unsigned arp_frame(unsigned char *f, int op, const unsigned char *dmac,
                          const unsigned char *tha, unsigned long tpa)
{
    memset(f, 0, 60);
    memcpy(f, dmac, 6);
    memcpy(f + 6, mymac, 6);
    f[12] = 0x08; f[13] = 0x06;                 /* ARP */
    f[15] = 1;                                  /* Ethernet */
    f[16] = 0x08;                               /* IPv4 */
    f[18] = 6; f[19] = 4;
    f[21] = (unsigned char)op;
    memcpy(f + 22, mymac, 6);
    put32(f + 28, my_ip);
    if (tha) memcpy(f + 32, tha, 6);
    put32(f + 38, tpa);
    return 60;
}

/* mode 'A': ARP request to target, wait for the reply; mode 'L': listen and
   answer ARP requests for our IP. Both read files in between. */
static int bridge_test(char mode, unsigned long target, long seconds)
{
    static const unsigned char bcast[6] = { 255, 255, 255, 255, 255, 255 };
    unsigned char f[64];
    long t0, now, next_arp = 0, tx_ok = 0, tx_busy = 0, tx_err = 0;
    long polls = 0, poll_ticks = 0, frames = 0, shown = 0, loops = 0;
    long arp_req_us = 0, arp_replies = 0, ipv4 = 0, rx_err = 0;
    int got_reply = 0, which = 0;
    char a[16];

    chunkbuf = (unsigned char *)Malloc(CHUNK);
    if (!chunkbuf) { printf("no memory" NL); return 0; }
    printf("Reference read of the test files:" NL);
    ftest_init(&files[0], "C:\\ACSITNFS.PRG");
    ftest_init(&files[1], "F:\\GAMES\\BIG.BI4");

    net_ctrl(1, my_ip, my_mask);
    if (a_result != 0) return 0;
    put32(f, target);
    ip_txt(a, f);
    if (mode == 'A') printf("ARP request for %s every second, %ld s:" NL, a, seconds);
    else printf("Listening %ld s, answering ARP for our IP:" NL, seconds);

    t0 = Supexec(get_hz);
    do {
        long t;
        now = Supexec(get_hz);
        loops++;
        if (mode == 'A' && !got_reply && now >= next_arp) {
            long r = send_frame(f, arp_frame(f, 1, bcast, NULL, target));
            if (r == 0) tx_ok++; else if (r == 8) tx_busy++; else tx_err++;
            next_arp = now + 200;
        }
        for (;;) {                              /* everything queued, at most 4 */
            long r;
            unsigned len;
            t = Supexec(get_hz);
            r = cmd(NET_RX, 3, dmabuf, 3, 0);
            poll_ticks += Supexec(get_hz) - t;
            polls++;
            if (r != 0) { if (!rx_err++) printf("  NET_RX status %ld" NL, r); break; }
            len = get16(dmabuf);
            if (!len) break;
            frames++;
            if (shown++ < 25) describe((now - t0) * 5, dmabuf + 8, len);
            if (get16(dmabuf + 8 + 12) == 0x0806) {
                const unsigned char *p = dmabuf + 8;
                if (get16(p + 20) == 2 && get32(p + 28) == target) got_reply = 1;
                if (get16(p + 20) == 1 && get32(p + 38) == my_ip) {
                    arp_req_us++;
                    if (mode == 'L') {
                        unsigned char sha[6];
                        unsigned long spa = get32(p + 28);
                        memcpy(sha, p + 22, 6);
                        if (send_frame(f, arp_frame(f, 2, sha, sha, spa)) == 0) arp_replies++;
                        else tx_err++;
                    }
                }
            } else if (get16(dmabuf + 8 + 12) == 0x0800)
                ipv4++;
            if (frames % 4 == 0) break;
        }
        ftest_step(&files[which]);                  /* disk / TNFS in between */
        which ^= 1;
    } while (now - t0 < seconds * 200);

    printf(NL "Result:" NL);
    if (mode == 'A') printf("  ARP reply from %s: %s" NL, a, got_reply ? "YES" : "no");
    printf("  frames received %ld (ARP requests for us %ld, IPv4 for us %ld)" NL,
           frames, arp_req_us, ipv4);
    printf("  NET_TX ok %ld, busy %ld, errors %ld; ARP replies sent %ld" NL,
           tx_ok + arp_replies, tx_busy, tx_err, arp_replies);
    printf("  NET_RX polls %ld (%ld failed), %ld us each" NL, polls, rx_err, polls ? poll_ticks * 5000L / polls : 0);
    printf("  %s: %ld reads, %ld errors" NL, files[0].name, files[0].reads, files[0].errors);
    printf("  %s: %ld reads, %ld errors" NL, files[1].name, files[1].reads, files[1].errors);
    printf("  bridge stays on (NETTEST D turns it off, an Atari reset too)" NL);
    if (files[0].fd >= 0) Fclose(files[0].fd);
    if (files[1].fd >= 0) Fclose(files[1].fd);
    Mfree(chunkbuf);
    return (mode != 'A' || got_reply) && !rx_err && !files[0].errors && !files[1].errors;
}

static void take_info(void)                     /* after net_info(): our addresses */
{
    memcpy(mymac, dmabuf + 10, 6);
    pico_ip = get32(dmabuf + 16);
    my_mask = get32(dmabuf + 20);
    gw_ip = get32(dmabuf + 24);
}

int main(int argc, char **argv)
{
    char op = argc > 1 ? argv[1][0] & ~0x20 : 0;
    int ok = 1, i;

    logf = fopen("NETTEST.LOG", "ab");
    fputs("\033E", stdout);                     /* clear screen, not in the log */
    if (logf) fprintf(logf, "----" NL);
    printf("NETTEST - ACSI_NET test, arguments:");
    for (i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf(NL NL);
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
        case 'A':       /* NETTEST A [atari-ip [target-ip [seconds]]] */
        case 'L':       /* NETTEST L [atari-ip [seconds]] */
            if (!(ok = net_info(1))) break;
            take_info();
            my_ip = parse_ip(argc > 2 ? argv[2] : "192.168.178.50");
            if (op == 'A')
                ok = bridge_test('A', argc > 3 ? parse_ip(argv[3]) : gw_ip,
                                 argc > 4 ? atol(argv[4]) : 10);
            else
                ok = bridge_test('L', my_ip, argc > 3 ? atol(argv[3]) : 60);
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
    if (logf) fclose(logf);
    Cconin();
    return ok ? 0 : 1;
}
