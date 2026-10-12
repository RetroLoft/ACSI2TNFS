/*
 * PORTINFO.PRG - what STinG thinks of its ports and routes: name, active,
 * IP address and mask of every port, the routing table and the ACSI_IP and
 * ACSI_MASK lines of DEFAULT.CFG. A test tool for ACSI_NET.STX.
 * Compiled with -mshort like the STX, no C library (start-up: init.S).
 */
#include <osbind.h>
#include <mint/basepage.h>

#define cdecl
#define NULL ((void *)0)
typedef BASEPAGE BASPAG;
#include "transprt.h"
#include "port.h"

TPL *tpl;
STX *stx;

static void say(const char *s)
{
    while (*s) {
        if (*s == '\n') Bconout(2, '\r');
        Bconout(2, *s++);
    }
}

static void say_num(uint32 n)
{
    char d[12], *p = d + 11;
    *p = '\0';
    do *--p = (char)('0' + n % 10); while (n /= 10);
    say(p);
}

static void say_ip(uint32 ip)
{
    int i;
    for (i = 24; i >= 0; i -= 8) {
        say_num(ip >> i & 0xff);
        if (i) say(".");
    }
}

static long get_sting_cookie(void)
{
    long *p;
    for (p = *(long **)0x5a0L; p && *p; p += 2)
        if (*p == 0x5354694bL)              /* 'STiK' */
            return p[1];
    return 0;
}

static void done(int code)
{
    say("\nPress a key.");
    Cnecin();
    Pterm(code);
}

void _init(BASEPAGE *bp)
{
    DRV_LIST *sting;
    PORT *port;
    DRIVER *drv;
    int16 i;
    (void)bp;

    say("\nPORTINFO: STinG ports and routes\n\n");
    sting = (DRV_LIST *)Supexec(get_sting_cookie);
    if (!sting) { say("STinG is not loaded.\n"); done(1); }
    tpl = (TPL *)(*sting->get_dftab)(TRANSPORT_DRIVER);
    stx = (STX *)(*sting->get_dftab)(MODULE_DRIVER);
    if (!tpl || !stx) { say("STinG tables not found.\n"); done(1); }

    say("DEFAULT.CFG: ACSI_IP = ");
    say(getvstr((char *)"ACSI_IP"));
    say(", ACSI_MASK = ");
    say(getvstr((char *)"ACSI_MASK"));
    say(", ACTIVATE = ");
    say(getvstr((char *)"ACTIVATE"));
    say("\n\nPorts:\n");

    query_chains((void **)&port, (void **)&drv, NULL);
    for (; port; port = port->next) {
        say("  ");
        say(port->name);
        say(port->active ? "  active  " : "  off     ");
        say_ip(port->ip_addr);
        say(" / ");
        say_ip(port->sub_mask);
        say("\n");
    }

    say("\nRoutes:\n");
    for (i = 0; i < 32; i++) {
        uint32 tmpl, mask, gw;
        PORT *rp;
        if (get_route_entry(i, &tmpl, &mask, &rp, &gw) < 0) break;
        say("  ");
        say_ip(tmpl);
        say(" / ");
        say_ip(mask);
        say("  ");
        say(rp ? rp->name : "?");
        say("  gateway ");
        say_ip(gw);
        say("\n");
    }
    if (i == 0) say("  (none)\n");
    done(0);
}
