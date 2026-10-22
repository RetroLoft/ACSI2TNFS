/*
 * NETCLOCK.PRG - sets the Atari's clock from the ACSI2TNFS adapter's network
 * time (vendor sub 9), for the AUTO folder of a machine where our boot driver
 * does not load (HDDRIVER on a Mega STE, a TT booting from its own disk).
 * Waits up to 30 s per step for the adapter's Wi-Fi and time server, as the
 * driver does; ESC skips. Compiled with -mshort, no C library.
 */
#include <osbind.h>
#include <mint/basepage.h>
#include "../sting/acsinet.h"

#define NULL ((void *)0)
typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned long  uint32;

#define HZ200 (*(volatile uint32 *)0x4baL)

/* "ATC", state, year.w, month, day, hour, minute, second */
enum { CLK_VALID = 0, CLK_WAIT = 1, CLK_NONE = 2, CLK_OFF = 3, CLK_NTP = 4 };

static uint16 buf[256];         /* DMA: even, ST-RAM (program flags) */
static long sv_r;
static uint32 sv_now;

static long sv_find(void) { return sv_r = acsi_find(); }
static long sv_ask(void) { return sv_r = acsi_net(9, 0, buf, 1, 0, 1); }
static long sv_hz(void) { return sv_now = HZ200; }

void *memset(void *d, int c, unsigned long n)
{
    char *p = d;
    while (n--) *p++ = (char)c;
    return d;
}

static void say(const char *s)
{
    while (*s) {
        if (*s == '\n') Bconout(2, '\r');
        Bconout(2, *s++);
    }
}

static char *two(char *p, uint16 n) { p[0] = (char)('0' + n / 10 % 10); p[1] = (char)('0' + n % 10); return p + 2; }

static uint32 now(void) { Supexec(sv_hz); return sv_now; }

void _init(BASEPAGE *bp)
{
    const uint8 *b = (const uint8 *)buf;
    int step = -1;
    uint32 t0 = 0;
    int shown = -1;
    (void)bp;

    say("NETCLOCK: ");
    Supexec(sv_find);
    if (sv_r < 0) { say("no ACSI2TNFS adapter found, clock not set\n"); Pterm0(); }

    for (;;) {
        memset(buf, 0, sizeof buf);
        Supexec(sv_ask);
        if (sv_r != 0 || b[0] != 'A' || b[1] != 'T' || b[2] != 'C') {
            say("firmware without network clock, clock not set\n");
            Pterm0();
        }
        if (b[3] == CLK_VALID) break;
        if (b[3] == CLK_OFF) { say("network clock switched off (ACSITNFS.PRG)\n"); Pterm0(); }
        if (b[3] == CLK_NONE) { say("no network time, clock not set\n"); Pterm0(); }
        if (b[3] != step) {                     /* a new step: its own 30 s */
            if (step >= 0) say("ok\nNETCLOCK: ");
            step = b[3];
            say(step == CLK_WAIT ? "waiting for Wi-Fi (ESC: skip) " : "waiting for the time server (ESC: skip) ");
            t0 = now();
            shown = -1;
        }
        {
            int left = 30 - (int)((now() - t0) / 200);
            char n[3];
            if (left <= 0) { say("\nNETCLOCK: no network time, clock not set\n"); Pterm0(); }
            if (left != shown) {
                if (shown >= 0) say("\b\b");
                n[2] = 0;
                two(n, (uint16)left);
                say(n);
                shown = left;
            }
        }
        if (Bconstat(2) && (Bconin(2) & 0xff) == 27) { say("\nNETCLOCK: skipped\n"); Pterm0(); }
        {
            uint32 until = now() + 50;          /* 0.25 s */
            while (now() < until) ;
        }
    }
    if (step >= 0) say("ok\nNETCLOCK: ");

    {
        uint16 year = (uint16)(b[4] << 8 | b[5]);
        uint16 date = (uint16)((year - 1980) << 9 | b[6] << 5 | b[7]);
        uint16 time = (uint16)(b[8] << 11 | b[9] << 5 | b[10] >> 1);
        char m[32], *p = m;
        Tsetdate(date);
        Tsettime(time);
        Settime((uint32)date << 16 | time);     /* keyboard / battery clock */
        p = two(p, b[7]); *p++ = '-';
        p = two(p, b[6]); *p++ = '-';
        p = two(p, (uint16)(year / 100)); p = two(p, (uint16)(year % 100)); *p++ = ' ';
        p = two(p, b[8]); *p++ = ':';
        p = two(p, b[9]); *p = 0;
        say("clock set to ");
        say(m);
        say("\n");
    }
    Pterm0();
}
