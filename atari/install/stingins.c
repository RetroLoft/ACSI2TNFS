/*
 * STINGINS.TOS - installs STinG for the network function of ACSI2TNFS, or
 * changes only its network settings when STinG is there already.
 *
 * Carries the files it installs (payload.c, made by mkpayload.py). Asks the
 * adapter for its own Wi-Fi address (NET_INFO) to suggest mask, gateway and
 * DNS. The Atari's address goes into DEFAULT.CFG as ACSI_IP / ACSI_MASK:
 * ACSI_NET.STX (00.08 and later) switches its port on with it, so XControl
 * is not needed. DEFAULT.CFG and ROUTE.TAB are updated, not replaced: other
 * settings and the routes of other ports stay.
 *
 * Compiled with -mshort like ACSI_NET.STX (acsinet.c), no C library.
 */
#include <osbind.h>
#include <mint/basepage.h>
#include "../sting/acsinet.h"

#define NULL ((void *)0)
typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned long  uint32;

#define VERSION "1.0"

typedef struct {
    const char  *dir;           /* "AUTO", "STING", "STING\\TOOLS" */
    const char  *name;
    const uint8 *data;
    long         size;
} PFILE;

extern const PFILE payload[];   /* payload.c, ends with name NULL */
extern const uint8 cfg_default[];       /* DEFAULT.CFG of STinG, when none is there */
extern const long cfg_default_size;

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

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static char *scpy(char *d, const char *s) { while ((*d = *s++) != 0) d++; return d; }

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static void say(const char *s)
{
    while (*s) {
        if (*s == '\n') Bconout(2, '\r');
        Bconout(2, *s++);
    }
}

static char *put_ip(char *p, uint32 ip)
{
    int i;
    for (i = 24; i >= 0; i -= 8) {
        uint16 n = (uint16)(ip >> i & 0xff);
        char d[4], *q = d + 3;
        *q = 0;
        do *--q = (char)('0' + n % 10); while (n /= 10);
        p = scpy(p, q);
        if (i) *p++ = '.';
    }
    *p = 0;
    return p;
}

static void say_ip(uint32 ip) { char b[16]; put_ip(b, ip); say(b); }

/* "a.b.c.d" (spaces around it allowed) -> *ip; 0 = not an address */
static int parse_ip(const char *s, uint32 *ip)
{
    uint32 v = 0;
    int part;
    while (*s == ' ' || *s == '\t') s++;
    for (part = 0; part < 4; part++) {
        uint16 n = 0;
        int digits = 0;
        if (part && *s++ != '.') return 0;
        while (*s >= '0' && *s <= '9' && digits < 3) n = (uint16)(n * 10 + (*s++ - '0')), digits++;
        if (!digits || n > 255) return 0;
        v = v << 8 | n;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s) return 0;
    *ip = v;
    return 1;
}

/* a line from the keyboard; empty = keep the default */
static void ask(const char *prompt, const char *def, char *out, int max)
{
    char buf[2 + 64];
    say(prompt);
    if (def && *def) { say(" ["); say(def); say("]"); }
    say(": ");
    buf[0] = (char)(max < 63 ? max : 63);
    Cconrs(buf);
    say("\n");
    buf[2 + (uint8)buf[1]] = 0;
    scpy(out, (uint8)buf[1] ? buf + 2 : (def ? def : ""));
}

static uint32 ask_ip(const char *prompt, uint32 def)
{
    char d[16], s[64];
    uint32 ip;
    put_ip(d, def);
    for (;;) {
        ask(prompt, def ? d : "", s, 15);
        if (parse_ip(s, &ip)) return ip;
        say("  that is not an IP address (like 192.168.1.50)\n");
    }
}

static int ask_yes(const char *prompt)
{
    long k;
    say(prompt);
    say(" (Y/N) ");
    for (;;) {
        k = Cnecin() & 0xff;
        if (upper((int)k) == 'Y' || upper((int)k) == 'J') { say("yes\n"); return 1; }
        if (upper((int)k) == 'N') { say("no\n"); return 0; }
    }
}

static void quit(int code)
{
    say("\nPress a key.");
    Cnecin();
    Pterm(code);
}

/* ---------------- files ---------------- */

static char filebuf[32768];     /* DEFAULT.CFG, ROUTE.TAB */

/* whole file into filebuf (0-terminated); -1 = cannot read */
static long read_file(const char *path)
{
    long h = Fopen(path, 0), n;
    if (h < 0) return -1;
    n = Fread((int)h, (long)sizeof filebuf - 1, filebuf);
    Fclose((int)h);
    if (n < 0) return -1;
    filebuf[n] = 0;
    return n;
}

static int write_file(const char *path, const void *data, long size)
{
    long h = Fcreate(path, 0);
    long n;
    if (h < 0) return 0;
    n = Fwrite((int)h, size, data);
    Fclose((int)h);
    return n == size;
}

static int exists(const char *path)
{
    long h = Fopen(path, 0);
    if (h < 0) return 0;
    Fclose((int)h);
    return 1;
}

/* "X:\STING\" + name */
static char *path_of(char *out, const char *dir, const char *name)
{
    char *p = scpy(out, dir);
    if (p > out && p[-1] != '\\') *p++ = '\\';
    scpy(p, name);
    return out;
}

/* ---------------- DEFAULT.CFG and ROUTE.TAB ---------------- */

/* does line l (up to the line end) set variable name? */
static int is_var(const char *l, const char *name)
{
    int n = slen(name), i;
    while (*l == ' ' || *l == '\t') l++;
    for (i = 0; i < n; i++)
        if (upper(l[i]) != upper(name[i])) return 0;
    l += n;
    while (*l == ' ' || *l == '\t') l++;
    return *l == '=';
}

/* value of variable name in the text (up to the line end) into out */
static int get_var(const char *t, const char *name, char *out, int max)
{
    while (*t) {
        if (is_var(t, name)) {
            int n = 0;
            while (*t != '=') t++;
            t++;
            while (*t == ' ' || *t == '\t') t++;
            while (*t && *t != '\r' && *t != '\n' && n < max - 1) out[n++] = *t++;
            while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
            out[n] = 0;
            return 1;
        }
        while (*t && *t != '\n') t++;
        if (*t) t++;
    }
    return 0;
}

static char outbuf[32768 + 512];

/* DEFAULT.CFG in filebuf with ACSI_IP, ACSI_MASK and NAMESERVER set: lines
   with them are replaced; missing ones go after ACTIVATE (else at the end).
   Always written with CR LF: STinG 1.26 skips the line after every variable
   in a file with LF only (init_cfg overwrites the LF it then looks for). */
static long update_cfg(uint32 ip, uint32 mask, uint32 dns)
{
    static const char *vars[3] = { "ACSI_IP", "ACSI_MASK", "NAMESERVER" };
    uint32 val[3];
    int done[3] = { 0, 0, 0 }, k;
    const char *t, *nl = "\r\n";
    char *o = outbuf;
    val[0] = ip; val[1] = mask; val[2] = dns;

    for (t = filebuf; *t; ) {
        const char *e = t;
        int which = -1, keep = 1;
        while (*e && *e != '\n') e++;
        if (*e) e++;
        for (k = 0; k < 3; k++) if (is_var(t, vars[k])) which = k;
        if (which >= 0) {
            keep = 0;
            if (!done[which]) {
                o = scpy(o, vars[which]);
                o = scpy(o, which == 1 ? "   = " : which == 0 ? "     = " : "  = ");
                o = put_ip(o, val[which]);
                o = scpy(o, nl);
                done[which] = 1;
            }
        }
        if (keep) {                                         /* the line, with CR LF */
            const char *c = e;
            while (c > t && (c[-1] == '\n' || c[-1] == '\r')) c--;
            memcpy(o, t, (unsigned long)(c - t));
            o = scpy(o + (c - t), nl);
        }
        if (keep && is_var(t, "ACTIVATE")) {
            for (k = 0; k < 2; k++) {
                const char *s = filebuf;
                char dummy[32];
                if (get_var(s, vars[k], dummy, sizeof dummy)) continue;   /* comes later */
                o = scpy(o, vars[k]);
                o = scpy(o, k ? "   = " : "     = ");
                o = put_ip(o, val[k]);
                o = scpy(o, nl);
                done[k] = 1;
            }
        }
        t = e;
    }
    for (k = 0; k < 3; k++)
        if (!done[k]) {
            o = scpy(o, vars[k]);
            o = scpy(o, " = ");
            o = put_ip(o, val[k]);
            o = scpy(o, nl);
        }
    return o - outbuf;
}

/* ROUTE.TAB in filebuf (or empty): lines for other ports stay, the ones for
   ACSI2TNFS are replaced by the subnet and the default route */
static long update_routes(uint32 ip, uint32 mask, uint32 gw)
{
    const char *t;
    char *o = outbuf;
    int any = 0;
    for (t = filebuf; *t; ) {
        const char *e = t, *p;
        int ours = 0;
        while (*e && *e != '\n') e++;
        if (*e) e++;
        for (p = t; *t != '#' && p + 9 <= e; p++)               /* comments stay */
            if (p[0] == 'A' && p[1] == 'C' && p[2] == 'S' && p[3] == 'I' && p[4] == '2' &&
                p[5] == 'T' && p[6] == 'N' && p[7] == 'F' && p[8] == 'S') ours = 1;
        if (!ours) { memcpy(o, t, (unsigned long)(e - t)); o += e - t; any = 1; }
        t = e;
    }
    if (any && o[-1] != '\n') o = scpy(o, "\r\n");
    if (!any) o = scpy(o, "# ROUTE.TAB for STinG: network  mask  port  gateway (tabs)\r\n");
    o = put_ip(o, ip & mask);
    *o++ = '\t';
    o = put_ip(o, mask);
    o = scpy(o, "\tACSI2TNFS\t0.0.0.0\r\n0.0.0.0\t\t0.0.0.0\t\tACSI2TNFS\t");
    o = put_ip(o, gw);
    o = scpy(o, "\r\n");
    return o - outbuf;
}

/* ---------------- the adapter ---------------- */

static uint16 info[256];        /* NET_INFO, 512 bytes, even address in ST-RAM */
static long sv_r;
static long sv_info(void)
{
    if (acsi_find() < 0) return sv_r = -1;
    return sv_r = acsi_net(NET_INFO, 0, info, 1, 0, 1);
}

static uint32 get32(const uint8 *p) { return (uint32)p[0] << 24 | (uint32)p[1] << 16 | (uint32)p[2] << 8 | p[3]; }

static long sv_bootdev(void) { return *(volatile short *)0x446L; }

/* ---------------- main ---------------- */

void _init(BASEPAGE *bp)
{
    char drive[8], stdir[64], path[96], s[64], cur[16];
    uint32 pico_ip = 0, pico_mask = 0, pico_gw = 0, ip, mask, gw, dns;
    int have_old = 0, full = 1, i;
    const uint8 *b = (const uint8 *)info;
    long n;
    (void)bp;

    say("\033E ACSI2TNFS: STinG installer " VERSION "\n");
    say(" STinG 1.26 (Peter Rottengatter, Ronald Andersson; freeware)\n");
    say(" with TCP.STX 1.41 and ACSI_NET.STX for the adapter\n\n");

    Supexec(sv_info);
    if (sv_r == 0 && b[0] == 'A' && b[1] == 'T' && b[2] == 'N') {
        pico_ip = get32(b + 16);
        pico_mask = get32(b + 20);
        pico_gw = get32(b + 24);
        say("Adapter found, Wi-Fi ");
        if (pico_ip) { say_ip(pico_ip); say(" / "); say_ip(pico_mask); say(", gateway "); say_ip(pico_gw); }
        else say("not connected yet");
        say("\n\n");
    } else
        say("No ACSI2TNFS adapter with network function found (installing anyway).\n\n");

    /* where: the boot drive by default */
    drive[0] = (char)('A' + (int)Supexec(sv_bootdev));
    drive[1] = 0;
    ask("Install on drive", drive, s, 1);
    drive[0] = (char)upper(s[0]);
    if (drive[0] < 'A' || drive[0] > 'P') { say("No such drive.\n"); quit(1); }
    drive[1] = ':'; drive[2] = '\\'; drive[3] = 0;

    /* STinG there already? AUTO\STING.INF says where its folder is */
    scpy(stdir, drive);
    scpy(stdir + 3, "STING\\");
    path_of(path, drive, "AUTO\\STING.INF");
    if (read_file(path) > 0) {
        for (i = 0; filebuf[i] && filebuf[i] != '\r' && filebuf[i] != '\n' && i < 60; i++) stdir[i] = filebuf[i];
        stdir[i] = 0;
        if (i && stdir[i - 1] != '\\') { stdir[i] = '\\'; stdir[i + 1] = 0; }
        path_of(path, stdir, "DEFAULT.CFG");
        cur[0] = 0;
        if (read_file(path) > 0 && get_var(filebuf, "ACSI_IP", cur, sizeof cur)) have_old = 1;
        say("STinG is installed in "); say(stdir);
        if (have_old) { say(", Atari IP "); say(cur); }
        say("\n  1 = change the network settings only\n  2 = install everything again\n");
        for (;;) {
            ask("Choice", "1", s, 1);
            if (s[0] == '1') { full = 0; break; }
            if (s[0] == '2') break;
        }
    }

    /* the addresses */
    ip = 0;
    if (have_old) parse_ip(cur, &ip);
    if (!ip && pico_ip) ip = (pico_ip & pico_mask) | 50;     /* a suggestion */
    say("\nThe Atari needs its own, free IP address in the adapter's network\n"
        "(not the adapter's, and outside the range your router hands out).\n");
    for (;;) {
        ip = ask_ip("Atari IP address", ip);
        if (ip == pico_ip) { say("  that is the adapter's own address\n"); continue; }
        break;
    }
    mask = 0;
    path_of(path, stdir, "DEFAULT.CFG");
    if (read_file(path) > 0 && get_var(filebuf, "ACSI_MASK", s, sizeof s)) parse_ip(s, &mask);
    if (!mask) mask = pico_mask ? pico_mask : 0xffffff00UL;
    mask = ask_ip("Subnet mask", mask);
    gw = pico_gw ? pico_gw : (ip & mask) | 1;
    gw = ask_ip("Gateway (router)", gw);
    dns = 0;
    if (read_file(path) > 0 && get_var(filebuf, "NAMESERVER", s, sizeof s)) parse_ip(s, &dns);
    if (!dns) dns = gw;
    dns = ask_ip("DNS server", dns);

    say("\n");
    say(full ? "Install STinG in " : "Change the settings in ");
    say(stdir);
    say(" with Atari IP ");
    say_ip(ip);
    if (!ask_yes("?")) quit(0);

    /* program files */
    if (full) {
        path_of(path, drive, "AUTO");
        (void)Dcreate(path);
        scpy(path, stdir);
        path[slen(path) - 1] = 0;                           /* no backslash at the end */
        (void)Dcreate(path);
        for (i = 0; payload[i].name; i++) {
            const PFILE *f = &payload[i];
            char dir[96], fp[128];
            if (f->dir[0] == 'A') path_of(dir, drive, "AUTO");
            else if (f->dir[5] == '\\') {                     /* STING\TOOLS */
                path_of(dir, stdir, f->dir + 6);
                (void)Dcreate(dir);
            } else {
                scpy(dir, stdir);
                dir[slen(dir) - 1] = 0;
            }
            path_of(fp, dir, f->name);
            say("  "); say(fp); say("\n");
            if (!write_file(fp, f->data, f->size)) { say("  cannot write it\n"); quit(1); }
        }
        /* AUTO\STING.INF: the STinG folder */
        n = scpy(scpy(s, stdir), "\r\n") - s;
        path_of(path, drive, "AUTO\\STING.INF");
        say("  "); say(path); say("\n");
        if (!write_file(path, s, n)) { say("  cannot write it\n"); quit(1); }
    }

    /* DEFAULT.CFG: the one there, or the template */
    path_of(path, stdir, "DEFAULT.CFG");
    if (read_file(path) < 0) {
        memcpy(filebuf, cfg_default, (unsigned long)cfg_default_size);
        filebuf[cfg_default_size] = 0;
    }
    n = update_cfg(ip, mask, dns);
    say("  "); say(path); say("\n");
    if (!write_file(path, outbuf, n)) { say("  cannot write it\n"); quit(1); }

    /* ROUTE.TAB */
    path_of(path, stdir, "ROUTE.TAB");
    if (read_file(path) < 0) filebuf[0] = 0;
    n = update_routes(ip, mask, gw);
    say("  "); say(path); say("\n");
    if (!write_file(path, outbuf, n)) { say("  cannot write it\n"); quit(1); }

    say("\nDone. Restart the Atari: STinG starts from ");
    say(drive);
    say("AUTO.\nTest it with ");
    say(stdir);
    say("TOOLS\\PING.PRG.\n");
    if (exists("C:\\XCONTROL.ACC") || exists("C:\\CPX\\STNGPORT.CPX"))
        say("XControl with STNGPORT.CPX is not needed for the adapter any more.\n");
    quit(0);
}
