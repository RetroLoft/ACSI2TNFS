/*
 * URLVIEW.TTP - show a web page (the source, HTTP) through STinG, a screen
 * at a time, like the desktop's file viewer: "-Meer-" waits for a key,
 * Control-C quits at once. Nothing is written to disk.
 *
 *   URLVIEW http://host[:port]/path        (also without http://)
 *   URLVIEW -h http://...                  also show the HTTP headers
 *
 * Only http://, no https:// (no TLS on a 68000). Compiled with -mshort
 * (STinG passes 16-bit ints), no C library: BIOS for the screen and keys.
 */
#include <osbind.h>
#include <mint/basepage.h>

#define cdecl
#define NULL ((void *)0)
typedef BASEPAGE BASPAG;
#include "transprt.h"

TPL *tpl;
char stack_area[4096];                      /* start.S switches to it */

#define CTRL_C  3
#define ESC     "\033"

/* ---------------- screen ---------------- */

static int rows = 25, cols = 80, line, col, quit;

static void out(char c) { Bconout(2, c); }

static void outs(const char *s)
{
    while (*s) out(*s++);
}

static void outnum(long n)
{
    char b[12];
    int i = 0;
    if (n < 0) { out('-'); n = -n; }
    do b[i++] = (char)('0' + n % 10); while (n /= 10);
    while (i) out(b[--i]);
}

/* screen size from the Line-A variables (V_CEL_MX/MY: last column/row) */
static void screen_size(void)
{
    register short *la __asm__("a0");
    __asm__ volatile (".word 0xa000" : "=r"(la) : : "d0", "d1", "d2", "a1", "a2", "memory");
    cols = la[-22] + 1;                     /* -44: V_CEL_MX */
    rows = la[-21] + 1;                     /* -42: V_CEL_MY */
    if (cols < 20 || cols > 200) cols = 80;
    if (rows < 5 || rows > 100) rows = 25;
}

static int key(void)                        /* BIOS: no GEMDOS ^C handling */
{
    return (int)(Bconin(2) & 0xff);
}

static void new_line(void)
{
    outs("\r\n");
    col = 0;
    if (++line >= rows - 1) {
        outs(ESC "p-Meer-" ESC "q");
        if (key() == CTRL_C) quit = 1;
        outs("\r" ESC "K");
        line = 0;
    }
}

/* one byte of the page: line breaks, tabs, wrapping; UTF-8 and other
   non-ASCII bytes as '?' (the Atari character set differs) */
static void show(unsigned char c)
{
    if (quit) return;
    if (c == '\n') { new_line(); return; }
    if (c == '\r') return;
    if (c == '\t') {
        do { out(' '); col++; } while (col % 8 && col < cols);
    } else if (c >= 0x80 && c < 0xc0) {
        return;                             /* UTF-8 continuation byte */
    } else {
        out(c < 32 || c == 127 ? '.' : c >= 0x80 ? '?' : (char)c);
        col++;
    }
    if (col >= cols) new_line();
}

static void shows(const char *s)
{
    while (*s) show((unsigned char)*s++);
}

/* ---------------- STinG ---------------- */

static long hz(void) { return *(volatile long *)0x4baL; }
static long now(void) { return Supexec(hz); }

static long get_sting(void)
{
    long *p;
    for (p = *(long **)0x5a0L; p && *p; p += 2)
        if (*p == 0x5354694bL)              /* 'STiK' */
            return p[1];
    return 0;
}

static void fail(const char *what, int16 err)
{
    outs(what);
    if (err) { outs(": "); outs(get_err_text(err)); }
    outs("\r\n");
}

static int same_prefix(const char *s, const char *p)
{
    while (*p) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != *p++) return 0;
    }
    return 1;
}

/* ---------------- main ---------------- */

static char url[256], host[128], req[512];
static unsigned char buf[512];

static void tos_quit(void) { Pterm0(); }

void tos_main(BASEPAGE *bp)
{
    DRV_LIST *sting;
    char *u, *h, *path;
    uint32 ip;
    uint16 port = 80;
    int16 cn, r, i, n, show_headers = 0;
    int state = 0, status_ok = 1;           /* 0: status line, 1: headers, 2: body */
    char status[80];
    int sl = 0;
    long last;
    int16 closed;

    Mshrink(bp, (long)bp->p_bbase + bp->p_blen - (long)bp);
    screen_size();
    outs(ESC "E" ESC "f");                  /* clear, cursor off */

    /* command line or ask */
    n = (unsigned char)bp->p_cmdlin[0];
    for (i = 0; i < n && i < (int)sizeof url - 1; i++) url[i] = bp->p_cmdlin[1 + i];
    url[i] = 0;
    u = url;
    while (*u == ' ') u++;
    if (u[0] == '-' && (u[1] == 'h' || u[1] == 'H')) {
        show_headers = 1;
        u += 2;
        while (*u == ' ') u++;
    }
    if (!*u) {
        outs("URL: " ESC "e");
        url[0] = (char)(sizeof url - 3);
        Cconrs(url);
        url[2 + (unsigned char)url[1]] = 0;
        u = url + 2;
        outs(ESC "f\r\n");
    }
    if (same_prefix(u, "https://")) {
        outs("https:// wordt niet ondersteund (geen TLS op een 68000).\r\n");
        goto done;
    }
    if (same_prefix(u, "http://")) u += 7;
    h = host;
    while (*u && *u != '/' && *u != ':' && *u != ' ' && h < host + sizeof host - 1) *h++ = *u++;
    *h = 0;
    if (*u == ':') {
        port = 0;
        for (u++; *u >= '0' && *u <= '9'; u++) port = (uint16)(port * 10 + (*u - '0'));
    }
    path = *u == '/' ? u : "/";
    for (h = path; *h && *h != ' '; h++) ;
    *h = 0;
    if (!host[0]) { outs("Gebruik: URLVIEW [-h] http://host/pad\r\n"); goto done; }

    sting = (DRV_LIST *)Supexec(get_sting);
    if (!sting) { outs("STinG is niet geladen.\r\n"); goto done; }
    tpl = (TPL *)(*sting->get_dftab)(TRANSPORT_DRIVER);
    if (!tpl) { outs("STinG transport niet gevonden.\r\n"); goto done; }

    outs(host); outs(" opzoeken...\r\n");
    r = resolve(host, NULL, &ip, 1);
    if (r <= 0) { fail("Naam niet gevonden", r); goto done; }
    outs("Verbinden met ");
    outnum(ip >> 24); out('.'); outnum(ip >> 16 & 255); out('.');
    outnum(ip >> 8 & 255); out('.'); outnum(ip & 255); out(':'); outnum(port);
    outs("...\r\n");
    cn = TCP_open(ip, port, 0, 2000);
    if (cn < 0) { fail("Verbinden mislukt", cn); goto done; }
    r = TCP_wait_state(cn, TESTABLISH, 20);
    if (r < 0) { fail("Verbinden mislukt", r); TCP_close(cn, 0, &closed); goto done; }

    /* HTTP/1.0 with Connection: close - the server ends the page by closing */
    n = 0;
    {
        const char *parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                                "\r\nUser-Agent: URLVIEW (Atari ST, STinG)\r\n"
                                "Connection: close\r\n\r\n", NULL };
        for (i = 0; parts[i]; i++)
            for (h = (char *)parts[i]; *h && n < (int)sizeof req; h++) req[n++] = *h;
    }
    last = now();
    while ((r = TCP_send(cn, req, n)) == E_OBUFFULL)
        if (now() - last > 2000) break;
    if (r < 0) { fail("Versturen mislukt", r); TCP_close(cn, 0, &closed); goto done; }

    outs(ESC "E");
    line = col = 0;
    last = now();
    while (!quit) {
        n = CNbyte_count(cn);
        if (n > 0) {
            if (n > (int16)sizeof buf) n = sizeof buf;
            n = CNget_block(cn, buf, n);
            if (n <= 0) continue;
            last = now();
            for (i = 0; i < n && !quit; i++) {
                unsigned char c = buf[i];
                if (state == 2) { show(c); continue; }
                if (state == 0) {           /* status line: "HTTP/1.x 200 OK" */
                    if (c == '\n') {
                        status[sl] = 0;
                        for (h = status; *h && *h != ' '; h++) ;
                        status_ok = h[0] == ' ' && h[1] == '2';
                        if (show_headers || !status_ok) { shows(status); show('\n'); }
                        state = 1;
                        sl = 0;
                    } else if (c != '\r' && sl < (int)sizeof status - 1) status[sl++] = (char)c;
                    continue;
                }
                /* headers: an empty line ends them */
                if (c == '\n') {
                    status[sl] = 0;
                    if (!sl) {
                        state = 2;
                        if (show_headers || !status_ok) show('\n');
                    } else if (show_headers || (!status_ok && same_prefix(status, "location:"))) {
                        shows(status); show('\n');
                    }
                    sl = 0;
                } else if (c != '\r' && sl < (int)sizeof status - 1) status[sl++] = (char)c;
            }
        } else if (n == 0 || n == E_NODATA) {
            if (Bconstat(2) && key() == CTRL_C) quit = 1;
            else if (now() - last > 6000) { new_line(); outs("(geen antwoord meer, 30 s)"); break; }
        } else
            break;                          /* E_EOF: the page is complete */
    }
    TCP_close(cn, 0, &closed);
    if (!quit) {
        if (col) new_line();
        outs(ESC "p-Einde-" ESC "q");
        key();
    }
    outs(ESC "e\r\n");
    tos_quit();
done:
    outs("Druk op een toets.");
    key();
    outs(ESC "e\r\n");
    tos_quit();
}
