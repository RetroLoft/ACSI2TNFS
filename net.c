/*
 * ACSI2TNFS - core0: Wi-Fi, TNFS client and the virtual FAT16 partition
 *
 * Runs only on boards with a CYW43 Wi-Fi chip (Pico W, Pico 2 W). The ACSI
 * bus keeps running on core1; the Atari starts the test with vendor command
 * sub 4 and reads the result in the info text (sub 0).
 *
 * TNFS (UDP, default port 16384), every datagram:
 *   [conn id lo][conn id hi][seq][cmd] data...        request
 *   [conn id lo][conn id hi][seq][cmd][status] data... reply
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "acsi.h"
#include "settings.h"

/* runtime view used by the network code: Wi-Fi credentials plus the TNFS
   drive that is mounted (the first enabled TNFS slot), derived from g_set */
typedef struct {
    uint32_t magic;
    char     ssid[33];
    char     pass[65];
    char     server[65];
    char     path[97];
    uint16_t port;
} net_settings_t;

#define NET_MAGIC 0x544e4653u   /* "TNFS": settings format of firmware 0.3 */

static net_settings_t g_net = {
    NET_MAGIC, "", "", "", "/", 16384
};

settings_t g_set, g_stage;

static void drives_from_settings(void);

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic = SET_MAGIC;
    strcpy(s->country, "NL");
    s->auth_mode = 7;                   /* WPA2 mixed */
    s->use_dhcp = 1;
    s->settings_letter = 'C';
    strcpy(s->ntp_server, "pool.ntp.org");
    strcpy(s->utc_offset, "+1");
}

static void settings_to_runtime(void)
{
    memset(&g_net, 0, sizeof g_net);
    g_net.magic = NET_MAGIC;
    snprintf(g_net.ssid, sizeof g_net.ssid, "%s", g_set.ssid);
    snprintf(g_net.pass, sizeof g_net.pass, "%s", g_set.pass);
    g_net.port = 16384;
    strcpy(g_net.path, "/");
    for (int i = 0; i < SET_MAX_DRIVES; i++) {
        const set_drive_t *d = &g_set.drv[i];
        if (d->state != DRV_ENABLED || d->type != DRV_TYPE_TNFS) continue;
        snprintf(g_net.server, sizeof g_net.server, "%s", d->host);
        snprintf(g_net.path, sizeof g_net.path, "%s", d->mount_path[0] ? d->mount_path : "/");
        g_net.port = d->port ? d->port : 16384;
        break;
    }
    drives_from_settings();
}

/* persisted blob = the staged settings (what the configuration program saved) */
const void *net_settings_blob(uint32_t *len)
{
    *len = sizeof g_stage;
    return &g_stage;
}

void net_settings_load(const void *blob)
{
    const settings_t *s = blob;
    const net_settings_t *old = blob;
    settings_defaults(&g_set);
    if (s->magic == SET_MAGIC) {
        g_set = *s;
    } else if (old->magic == NET_MAGIC) {
        /* firmware 0.3 format: Wi-Fi + one TNFS server */
        snprintf(g_set.ssid, sizeof g_set.ssid, "%.32s", old->ssid);
        snprintf(g_set.pass, sizeof g_set.pass, "%.64s", old->pass);
        if (old->server[0]) {
            set_drive_t *d = &g_set.drv[0];
            d->state = DRV_ENABLED;
            d->letter = 'D';
            d->type = DRV_TYPE_TNFS;
            d->transport = DRV_UDP;
            d->port = old->port ? old->port : 16384;
            snprintf(d->nickname, sizeof d->nickname, "TNFS");
            snprintf(d->host, sizeof d->host, "%.63s", old->server);
            snprintf(d->mount_path, sizeof d->mount_path, "%.31s", old->path[0] ? old->path : "/");
        }
        printf("settings: converted from the 0.3 format\n");
    }
    g_stage = g_set;
    settings_to_runtime();
}

/* status shown to the Atari (written by core0, read by core1) */
static char wifi_status[80] = "not configured";

static volatile bool test_req;

extern acsi_cfg_t g_cfg;

void net_request_test(void) { test_req = true; }

void net_console_status(void)
{
    char buf[512];
    net_status_text(buf, sizeof buf);
    for (char *q = buf; *q; q++) if (*q != '\r') putchar(*q);
    printf("\n");
}

/* ---------------- clock (set by NTP on core0, read by the driver via core1) ---------------- */

/* CLK_WAIT: waiting for Wi-Fi, CLK_NTP: Wi-Fi up, waiting for the time server */
enum { CLK_VALID = 0, CLK_WAIT = 1, CLK_NONE = 2, CLK_OFF = 3, CLK_NTP = 4 };
static volatile uint8_t  clk_state = CLK_OFF;
static volatile uint32_t clk_seq, clk_unix;    /* UTC seconds at clk_us */
static volatile uint64_t clk_us;
static volatile bool     clk_sync_now;          /* console K: sync without waiting */

static void clock_set(uint32_t unix)
{
    clk_seq++;                                  /* odd: being written */
    __dmb();
    clk_unix = unix;
    clk_us = time_us_64();
    __dmb();
    clk_seq++;
    clk_state = CLK_VALID;
}

/* fixed UTC offset from the settings: "+1", "-5", "+5.5", "+5:30" */
static int32_t utc_offset_sec(void)
{
    const char *s = g_set.utc_offset;
    int sign = 1, h = 0, m = 0;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9') h = h * 10 + (*s++ - '0');
    if (*s == '.' && s[1] >= '0' && s[1] <= '9') m = (s[1] - '0') * 6;
    else if (*s == ':') m = (s[1] - '0') * 10 + (s[2] >= '0' && s[2] <= '9' ? s[2] - '0' : 0);
    return sign * (h * 3600 + m * 60);
}

/* UTC seconds now (any core; only meaningful in state CLK_VALID) */
static uint32_t clock_now(void)
{
    uint32_t seq, unix;
    uint64_t us;
    do {
        seq = clk_seq;
        __dmb();
        unix = clk_unix;
        us = clk_us;
        __dmb();
    } while ((seq & 1) || seq != clk_seq);
    return unix + (uint32_t)((time_us_64() - us) / 1000000u);
}

/* The configuration program restarts the Pico and the Atari together; Wi-Fi
   and NTP then take longer than the driver waits. Keep the time across that
   watchdog reboot in the watchdog scratch registers, which survive it. */
#define CLK_KEEP_MAGIC  0x434c4b31u                     /* "CLK1" */

void net_clock_keep(void)
{
    if (clk_state != CLK_VALID) return;
    watchdog_hw->scratch[5] = clock_now();
    watchdog_hw->scratch[4] = CLK_KEEP_MAGIC;
}

static void clock_restore(void)
{
    if (watchdog_hw->scratch[4] != CLK_KEEP_MAGIC) return;
    watchdog_hw->scratch[4] = 0;
    if (g_set.rtc_enabled)      /* + time since this boot; the reboot itself is quick */
        clock_set(watchdog_hw->scratch[5] + (uint32_t)(time_us_64() / 1000000u));
}

/* core1, vendor sub 9: "ATC", state, then the local time as year (hi, lo),
   month, day, hour, minute, second (only valid in state CLK_VALID) */
void net_clock(uint8_t *out)
{
    memcpy(out, "ATC", 3);
    out[3] = clk_state;
    if (clk_state != CLK_VALID) return;
    int64_t t = (int64_t)clock_now() + utc_offset_sec();
    uint32_t days = (uint32_t)(t / 86400), sec = (uint32_t)(t % 86400);
    /* civil from days (Howard Hinnant) */
    int32_t z = (int32_t)days + 719468;
    int32_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    out[4] = (uint8_t)(y >> 8);
    out[5] = (uint8_t)y;
    out[6] = (uint8_t)m;
    out[7] = (uint8_t)d;
    out[8] = (uint8_t)(sec / 3600);
    out[9] = (uint8_t)(sec / 60 % 60);
    out[10] = (uint8_t)(sec % 60);
}

/* console K: network clock on/off (stored; active from the next Pico start) */
void net_clock_toggle(void)
{
    g_stage.rtc_enabled = !g_stage.rtc_enabled;
    if (g_stage.rtc_enabled && !g_stage.ntp_server[0]) strcpy(g_stage.ntp_server, "pool.ntp.org");
    g_set.rtc_enabled = g_stage.rtc_enabled;
    strcpy(g_set.ntp_server, g_stage.ntp_server);
    if (!g_set.rtc_enabled) clk_state = CLK_OFF;
    else if (clk_state == CLK_OFF) { clk_state = CLK_NTP; clk_sync_now = true; }
    cfg_save();
    printf("network clock %s\n", g_set.rtc_enabled ? "ON" : "OFF");
}

/* configuration program status: 0 disabled, 1 synchronised, 2 not synchronised */
uint32_t net_clock_sync_state(void)
{
    if (!g_set.rtc_enabled) return 0;
    return clk_state == CLK_VALID ? 1 : 2;
}

/* ---------------- virtual partition: core1 <-> core0 hand-over ---------------- */

/* core1 posts a sector request, core0 (network) fills or stores the buffer */
static volatile int      vreq_state;         /* 0 idle, 1 pending, 2 done, 3 failed */
static volatile uint32_t vreq_drive, vreq_rel, vreq_n;
static volatile bool     vreq_write;
static uint8_t *volatile vreq_buf;

#define VB_SPC      2
#define VB_ROOTENT  512
#define VB_SPF      64

/* BPB of the virtual partition; static, so core1 can serve it without the
   network (the driver reads it during boot) */
void vfat_bootsector(uint8_t *b)
{
    memset(b, 0, 512);
    b[0] = 0xeb; b[1] = 0x3c; b[2] = 0x90;
    memcpy(b + 3, "ACSI2TNF", 8);
    b[11] = 0x00; b[12] = 0x02;                 /* 512 bytes per sector   */
    b[13] = VB_SPC;
    b[14] = 1;                                  /* reserved sectors       */
    b[16] = 2;                                  /* FATs                   */
    b[17] = VB_ROOTENT & 0xff; b[18] = VB_ROOTENT >> 8;
    b[19] = VFAT_SECTORS & 0xff; b[20] = VFAT_SECTORS >> 8;
    b[21] = 0xf8;
    b[22] = VB_SPF;                             /* sectors per FAT        */
    b[38] = 0x29;
    memcpy(b + 43, "TNFS       ", 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xaa;
}

static bool vreq_run(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf, bool write)
{
    if (vreq_state == 1) return false;          /* core0 still busy */
    vreq_buf = buf;
    vreq_drive = drive;
    vreq_rel = rel;
    vreq_n = n;
    vreq_write = write;
    __dmb();
    vreq_state = 1;
    uint32_t t0 = time_us_32();
    while (vreq_state == 1) {
        if (!gpio_get(PIN_RST)) return false;   /* Atari reset */
        if (time_us_32() - t0 > 9000000) return false;
    }
    return vreq_state == 2;
}

/* core1: read n sectors of virtual partition 'drive' into buf */
bool net_vread(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf)
{
    if (!BOARD_HAS_WIFI) return false;
    if (rel == 0 && n == 1) { vfat_bootsector(buf); return true; }
    return vreq_run(drive, rel, n, buf, false);
}

/* core1: write n sectors from buf to virtual partition 'drive' */
bool net_vwrite(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf)
{
    if (!BOARD_HAS_WIFI) return false;
    return vreq_run(drive, rel, n, buf, true);
}

#if !BOARD_HAS_WIFI

bool net_link_up(void) { return false; }
void net_init(void) { clk_state = g_set.rtc_enabled ? CLK_NONE : CLK_OFF; clock_restore(); }
void net_poll(void) { test_req = false; }
static void drives_from_settings(void) { }
uint32_t net_vdrives(void) { return 0; }
uint8_t net_vdrive_letter(uint32_t k) { (void)k; return 0; }
uint32_t net_status_text(char *p, uint32_t max)
{
    int n = snprintf(p, max, "\r\nNo Wi-Fi on this board\r\n");
    return n < 0 ? 0 : ((uint32_t)n < max ? (uint32_t)n : max - 1);
}

#else

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

static bool wifi_inited;

bool net_link_up(void)
{
    return wifi_inited && cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

/* ---------------- Wi-Fi ---------------- */

static bool wifi_connect(void)
{
    if (!g_net.ssid[0]) {
        snprintf(wifi_status, sizeof wifi_status, "no SSID set (ACSITNFS.PRG: Config)");
        return false;
    }
    if (!wifi_inited) {
        snprintf(wifi_status, sizeof wifi_status, "starting Wi-Fi chip...");
        char c0 = g_set.country[0] ? g_set.country[0] : 'X';
        char c1 = g_set.country[1] ? g_set.country[1] : 'X';
        if (cyw43_arch_init_with_country(CYW43_COUNTRY(c0, c1, 0))) {
            snprintf(wifi_status, sizeof wifi_status, "Wi-Fi chip init FAILED");
            return false;
        }
        cyw43_arch_enable_sta_mode();
        wifi_inited = true;
    }
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
        return true;

    /* SideTNFS auth_mode mapping: 0 open, 1-2 WPA TKIP, 3-5 WPA2 AES, 6-8 mixed */
    static const uint32_t auth_map[9] = {
        CYW43_AUTH_OPEN, CYW43_AUTH_WPA_TKIP_PSK, CYW43_AUTH_WPA_TKIP_PSK,
        CYW43_AUTH_WPA2_AES_PSK, CYW43_AUTH_WPA2_AES_PSK, CYW43_AUTH_WPA2_AES_PSK,
        CYW43_AUTH_WPA2_MIXED_PSK, CYW43_AUTH_WPA2_MIXED_PSK, CYW43_AUTH_WPA2_MIXED_PSK };
    uint32_t auth = g_set.auth_mode <= 8 ? auth_map[g_set.auth_mode] : CYW43_AUTH_WPA2_MIXED_PSK;
    if (!g_net.pass[0]) auth = CYW43_AUTH_OPEN;
    for (int attempt = 1; ; attempt++) {
        snprintf(wifi_status, sizeof wifi_status, "connecting to '%s' (try %d)...",
                 g_net.ssid, attempt);
        if (cyw43_arch_wifi_connect_async(g_net.ssid, g_net.pass[0] ? g_net.pass : NULL, auth)) {
            snprintf(wifi_status, sizeof wifi_status, "connect call FAILED");
            return false;
        }
        absolute_time_t until = make_timeout_time_ms(20000);
        const char *err = NULL;
        for (;;) {
            int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (st == CYW43_LINK_UP) break;
            if (st == CYW43_LINK_NOIP && !g_set.use_dhcp) {    /* joined: static IP */
                ip4_addr_t ip, mask, gw, dns;
                if (ip4addr_aton(g_set.ip, &ip) && ip4addr_aton(g_set.netmask, &mask) &&
                    ip4addr_aton(g_set.gateway, &gw)) {
                    cyw43_arch_lwip_begin();
                    dhcp_stop(netif_default);
                    netif_set_addr(netif_default, &ip, &mask, &gw);
                    if (ip4addr_aton(g_set.dns, &dns)) {
                        ip_addr_t d;
                        ip_addr_copy_from_ip4(d, dns);
                        dns_setserver(0, &d);
                    }
                    cyw43_arch_lwip_end();
                    break;
                }
                err = "invalid static IP settings";
            }
            if (st == CYW43_LINK_BADAUTH) err = "wrong password";
            else if (st == CYW43_LINK_NONET) err = "network not found";
            else if (st == CYW43_LINK_FAIL) err = "connection failed";
            else if (time_reached(until)) err = "timeout";
            if (err) break;
            sleep_ms(50);
        }
        if (!err) break;
        printf("net: [%lu ms] Wi-Fi try %d: %s\n", (unsigned long)(time_us_64() / 1000), attempt, err);
        /* a wrong password will not get better by retrying */
        if (attempt >= 3 || !strcmp(err, "wrong password")) {
            snprintf(wifi_status, sizeof wifi_status, "'%s': %s", g_net.ssid, err);
            return false;
        }
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        sleep_ms(250);                  /* the first join after power-up often fails */
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    snprintf(wifi_status, sizeof wifi_status, "connected, IP %s", ip4addr_ntoa(ip));
    printf("net: [%lu ms] Wi-Fi %s\n", (unsigned long)(time_us_64() / 1000), wifi_status);
    return true;
}

/* ---------------- TNFS drives ----------------
 *
 * Every enabled TNFS slot of the settings (at most VDRIVES_MAX) is one
 * virtual partition with its own server, session and part of the node table.
 * core0 talks to one server at a time: 'cur' is the drive being served.
 */
typedef struct {
    int      slot;                  /* settings slot                         */
    uint8_t  letter;                /* wanted drive letter, 0 = any          */
    char     server[65];
    char     path[33];
    uint16_t port;
    bool     prefer_tcp;
    /* connection */
    ip_addr_t ip;
    bool     use_tcp, mounted;
    struct tcp_pcb *tpcb;
    volatile bool tcp_up, tcp_dead;
    volatile int  tcp_last_err;
    uint16_t conn_id;
    uint8_t  seq;
    uint8_t  gen;                   /* +1 at every new session (old handles dead) */
    unsigned vmaj, vmin;
    /* tree: nodes root .. node_end-1 of vn[], cluster owners valloc[va0 .. va0+van-1] */
    uint16_t root, node_end, node_limit;
    uint32_t pool_end, pool_limit;
    uint16_t va0, van;
    uint32_t files, dirs, skipped;
    volatile bool ready;
    /* open file cache */
    int      vf_node;
    uint8_t  vf_handle;
    uint32_t vf_pos;
    /* writes: every written sector lives in a hidden file on the server */
    bool     tmp_open, tmp_used, wr_pending;
    uint8_t  tmp_handle;
    uint32_t last_wr_ms;
    char     status[96];
    /* not available (no Wi-Fi, no server, mount refused): the drive shows
       one read-only file NET_ERR.TXT with this text, as SideTNFS does */
    bool     failed;
    uint16_t errlen;
    char     errtxt[400];
    char     dir[160];              /* top level names, for the info text    */
} vdrive_t;

static vdrive_t vd[VDRIVES_MAX];
static int      nvd;
static vdrive_t *cur;

static void drives_from_settings(void)
{
    nvd = 0;
    for (int i = 0; i < SET_MAX_DRIVES && nvd < VDRIVES_MAX; i++) {
        const set_drive_t *s = &g_set.drv[i];
        if (s->state != DRV_ENABLED || s->type != DRV_TYPE_TNFS || !s->host[0]) continue;
        vdrive_t *d = &vd[nvd++];
        memset(d, 0, sizeof *d);
        d->slot = i;
        d->letter = s->letter >= 'D' && s->letter <= 'P' ? s->letter : 0;   /* TOS: A: .. P: */
        snprintf(d->server, sizeof d->server, "%s", s->host);
        snprintf(d->path, sizeof d->path, "%s", s->mount_path[0] ? s->mount_path : "/");
        d->port = s->port ? s->port : 16384;
        d->prefer_tcp = s->transport == DRV_TCP;
        d->vf_node = -1;
        snprintf(d->status, sizeof d->status, "not connected yet");
    }
}

uint32_t net_vdrives(void) { return (uint32_t)nvd; }
uint8_t net_vdrive_letter(uint32_t k) { return k < (uint32_t)nvd ? vd[k].letter : 0; }

uint32_t net_status_text(char *p, uint32_t max)
{
    uint32_t n = 0;
#define ADD(...) do { int r_ = snprintf(p + n, max - n, __VA_ARGS__); \
                      if (r_ > 0) n = n + (uint32_t)r_ < max ? n + (uint32_t)r_ : max - 1; } while (0)
    ADD("\r\nWi-Fi SSID : %s\r\nWi-Fi      : %s\r\n",
        g_net.ssid[0] ? g_net.ssid : "(not set)", wifi_status);
    if (!nvd) ADD("TNFS       : no drive configured\r\n");
    for (int i = 0; i < nvd; i++)
        ADD("TNFS %d     : %s:%u %s\r\n  %s\r\n%s", i + 1, vd[i].server, vd[i].port,
            vd[i].path, vd[i].status, vd[i].dir);
#undef ADD
    return n;
}

/* ---------------- TNFS over UDP or TCP ---------------- */
/* Same messages on both transports; TCP has no extra framing. */

static struct udp_pcb *pcb;
static uint8_t  rx[600];
static volatile int rx_len;

static void udp_rx(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                   const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb;
    vdrive_t *d = cur;
    if (!rx_len && d && port == d->port && ip_addr_cmp(addr, &d->ip)) {
        int n = p->tot_len < sizeof rx ? p->tot_len : (int)sizeof rx;
        pbuf_copy_partial(p, rx, (u16_t)n, 0);
        rx_len = n;
    }
    pbuf_free(p);
}

static err_t tcp_rx(void *arg, struct tcp_pcb *t, struct pbuf *p, err_t err)
{
    (void)err;
    vdrive_t *d = arg;
    if (!p) { d->tcp_dead = true; return ERR_OK; }      /* closed by server */
    if (d == cur) {
        int room = (int)sizeof rx - rx_len;
        int n = p->tot_len < room ? p->tot_len : room;
        if (n > 0) pbuf_copy_partial(p, rx + rx_len, (u16_t)n, 0);
        rx_len += n;
    }
    tcp_recved(t, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_connected(void *arg, struct tcp_pcb *t, err_t err)
{
    (void)t;
    ((vdrive_t *)arg)->tcp_up = err == ERR_OK;
    return ERR_OK;
}

static void tcp_error(void *arg, err_t err)
{
    vdrive_t *d = arg;
    d->tcp_last_err = err;
    d->tpcb = NULL;                                     /* freed by lwIP */
    d->tcp_dead = true;
}

static bool tcp_open(vdrive_t *d)
{
    d->tcp_up = d->tcp_dead = false;
    d->tcp_last_err = 0;
    err_t ce = ERR_OK;
    cyw43_arch_lwip_begin();
    d->tpcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (d->tpcb) {
        tcp_arg(d->tpcb, d);
        tcp_recv(d->tpcb, tcp_rx);
        tcp_err(d->tpcb, tcp_error);
        ce = tcp_connect(d->tpcb, &d->ip, d->port, tcp_connected);
        if (ce != ERR_OK) {
            tcp_abort(d->tpcb);
            d->tpcb = NULL;
        }
    }
    cyw43_arch_lwip_end();
    /* the first SYN can be lost while ARP resolves the server; lwIP only
       retransmits after 3 s, so allow for that */
    absolute_time_t until = make_timeout_time_ms(10000);
    while (d->tpcb && !d->tcp_up && !d->tcp_dead && !time_reached(until)) sleep_ms(5);
    if (d->tcp_up) return true;
    printf("net: TCP connect to %s:%u failed (connect %d, error %d, %s)\n",
           ipaddr_ntoa(&d->ip), d->port, ce, d->tcp_last_err,
           d->tcp_dead ? "refused/reset" : "timeout");
    cyw43_arch_lwip_begin();
    if (d->tpcb) { tcp_abort(d->tpcb); d->tpcb = NULL; }
    cyw43_arch_lwip_end();
    return false;
}

static void tcp_shut(vdrive_t *d)
{
    if (!wifi_inited || !d->tpcb) return;   /* lwIP not started yet / nothing open */
    cyw43_arch_lwip_begin();
    if (d->tpcb && tcp_close(d->tpcb) != ERR_OK) tcp_abort(d->tpcb);
    d->tpcb = NULL;
    cyw43_arch_lwip_end();
}

static bool udp_open(void)
{
    if (pcb) return true;
    cyw43_arch_lwip_begin();
    pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb) {
        udp_bind(pcb, IP_ANY_TYPE, 0);
        udp_recv(pcb, udp_rx, NULL);
    }
    cyw43_arch_lwip_end();
    return pcb != NULL;
}

/* send one request to the current drive's server and wait for the matching
   reply; returns reply length (>= 5) or -1. UDP retries with the same
   sequence number, as TNFS expects. */
static int tnfs_req1(uint8_t cmd, const void *data, int dlen)
{
    static uint8_t tx[600];                 /* core0 only, not reentrant */
    vdrive_t *d = cur;
    if (dlen > (int)sizeof tx - 4) return -1;
    tx[0] = d->conn_id & 0xff;
    tx[1] = d->conn_id >> 8;
    tx[2] = ++d->seq;
    tx[3] = cmd;
    if (dlen) memcpy(tx + 4, data, (size_t)dlen);
    for (int attempt = 0; attempt < (d->use_tcp ? 1 : 4); attempt++) {
        rx_len = 0;
        cyw43_arch_lwip_begin();
        if (d->use_tcp) {
            if (d->tpcb) {
                tcp_write(d->tpcb, tx, (u16_t)(dlen + 4), TCP_WRITE_FLAG_COPY);
                tcp_output(d->tpcb);
            }
        } else {
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(dlen + 4), PBUF_RAM);
            if (p) {
                memcpy(p->payload, tx, (size_t)dlen + 4);
                udp_sendto(pcb, p, &d->ip, d->port);
                pbuf_free(p);
            }
        }
        cyw43_arch_lwip_end();
        absolute_time_t until = make_timeout_time_ms(d->use_tcp ? 3000 : 1000);
        while (!time_reached(until)) {
            if (rx_len >= 5 && rx[2] == tx[2] && rx[3] == cmd) {
                sleep_ms(d->use_tcp ? 5 : 0);       /* rest of a split TCP segment */
                return rx_len;
            }
            if (!d->use_tcp && rx_len) rx_len = 0;  /* stale UDP reply */
            if (d->use_tcp && d->tcp_dead) return -1;
            sleep_ms(2);
        }
    }
    return -1;
}

static bool tnfs_mount(void);
static void tcp_shut(vdrive_t *d);

/* tnfs_req1, and when the server no longer knows our session (it drops idle
   sessions, tnfsd after 10 minutes, or it was restarted: status 0xFF) mount
   again. Requests with a path are repeated; requests with a file or
   directory handle fail, their callers open the file again (d->gen). */
static int tnfs_req(uint8_t cmd, const void *data, int dlen)
{
    static bool remounting;
    vdrive_t *d = cur;
    int n = tnfs_req1(cmd, data, dlen);
    if (n < 5 || rx[4] != 0xff || cmd <= 0x01 || remounting) return n;
    printf("net: TNFS %s: session gone, mounting again\n", d->server);
    remounting = true;
    if (d->use_tcp) tcp_shut(d);
    d->mounted = false;
    bool ok = tnfs_mount();
    remounting = false;
    if (!ok) { printf("net: TNFS %s: %s\n", d->server, d->status); return -1; }
    d->gen++;
    d->vf_node = -1;                        /* handles of the old session */
    d->tmp_open = false;
    switch (cmd) {
    case 0x10: case 0x13: case 0x14: case 0x24: case 0x26: case 0x28: case 0x29:
        return tnfs_req1(cmd, data, dlen);  /* OPENDIR MKDIR RMDIR STAT UNLINK RENAME OPEN */
    default:
        return -1;
    }
}

/* ---------------- server address: IP or host name (DNS) ---------------- */

static volatile int dns_state;      /* 0 busy, 1 found, -1 not found */
static ip_addr_t dns_ip;

static void dns_found_cb(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name; (void)arg;
    if (ip) { dns_ip = *ip; dns_state = 1; }
    else dns_state = -1;
}

/* IP address or host name -> *ip */
static bool resolve_host(const char *host, ip_addr_t *ip)
{
    if (ipaddr_aton(host, ip)) return true;
    dns_state = 0;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &dns_ip, dns_found_cb, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) { *ip = dns_ip; return true; }     /* cached */
    if (e == ERR_INPROGRESS) {
        absolute_time_t until = make_timeout_time_ms(5000);
        while (!dns_state && !time_reached(until)) sleep_ms(10);
        if (dns_state == 1) { *ip = dns_ip; return true; }
    }
    return false;
}

static bool resolve_server(vdrive_t *d)
{
    snprintf(d->status, sizeof d->status, "looking up '%s'...", d->server);
    if (resolve_host(d->server, &d->ip)) return true;
    snprintf(d->status, sizeof d->status, "host name '%s' not found (DNS)", d->server);
    return false;
}

/* ---------------- network time (NTP) ----------------
 * The Pico keeps the time (it has its own power supply, so it stays in sync
 * while the Atari is switched off); the driver asks for it at boot. */

#define NTP_RESYNC_MS   (6u * 3600u * 1000u)     /* keep the Pico clock in step */
#define NTP_RETRY_MS    (30u * 1000u)

static struct udp_pcb *ntp_pcb;
static volatile uint32_t ntp_rx_unix;
static absolute_time_t ntp_next;
static int ntp_fails;           /* failed rounds since Wi-Fi came up */
#define NTP_FAST_RETRIES 4      /* the first answer after joining is sometimes lost */
#define NTP_FAST_MS      2000u

static void ntp_rx(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                   const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb; (void)addr; (void)port;
    uint8_t b[48];
    if (p->tot_len >= 48 && pbuf_copy_partial(p, b, 48, 0) == 48 && (b[0] & 7) == 4) {
        uint32_t ntp = ((uint32_t)b[40] << 24) | (b[41] << 16) | (b[42] << 8) | b[43];
        ntp_rx_unix = ntp - 2208988800u;                /* 1900 -> 1970 */
    }
    pbuf_free(p);
}

static bool ntp_sync(void)
{
    ip_addr_t ip;
    if (!resolve_host(g_set.ntp_server, &ip)) {
        printf("net: NTP server '%s' not found\n", g_set.ntp_server);
        return false;
    }
    cyw43_arch_lwip_begin();
    if (!ntp_pcb && (ntp_pcb = udp_new_ip_type(IPADDR_TYPE_V4)) != NULL)
        udp_recv(ntp_pcb, ntp_rx, NULL);
    cyw43_arch_lwip_end();
    if (!ntp_pcb) return false;
    for (int attempt = 0; attempt < 3; attempt++) {
        ntp_rx_unix = 0;
        cyw43_arch_lwip_begin();
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
        if (p) {
            memset(p->payload, 0, 48);
            ((uint8_t *)p->payload)[0] = 0x23;          /* version 4, client */
            udp_sendto(ntp_pcb, p, &ip, 123);
            pbuf_free(p);
        }
        cyw43_arch_lwip_end();
        absolute_time_t until = make_timeout_time_ms(2000);
        while (!ntp_rx_unix && !time_reached(until)) sleep_ms(5);
        if (ntp_rx_unix) {
            clock_set(ntp_rx_unix);
            printf("net: [%lu ms] NTP time from %s\n", (unsigned long)(time_us_64() / 1000), g_set.ntp_server);
            return true;
        }
    }
    printf("net: no answer from NTP server %s\n", g_set.ntp_server);
    return false;
}

static const char *tnfs_err(uint8_t st)
{
    switch (st) {
    case 0x01: return "permission denied";
    case 0x02: return "no such file or directory";
    case 0x05: return "I/O error";
    case 0x0d: return "access denied";
    case 0x14: return "not a directory";
    case 0x1c: return "no space";
    case 0x21: return "end of file";
    default:   return "error";
    }
}

/* ---------------- TNFS session ---------------- */

/* MOUNT the current drive's path over the preferred transport (UDP unless the
   slot says TCP), falling back to the other one: a public server may answer
   UDP only, a server behind a tunnel TCP only. */
static bool tnfs_mount(void)
{
    vdrive_t *d = cur;
    if (d->mounted) return true;
    if (!resolve_server(d)) return false;
    uint8_t m[128];
    int ml = 0;
    m[ml++] = 0x02;                             /* version 1.2: minor, major */
    m[ml++] = 0x01;
    size_t pl = strlen(d->path);
    memcpy(m + ml, d->path, pl + 1);
    ml += (int)pl + 1;
    m[ml++] = 0;                                /* user     */
    m[ml++] = 0;                                /* password */
    int n = -1;
    for (int t = 0; t < 2 && n < 0; t++) {
        bool tcp = (t == 0) == d->prefer_tcp;
        d->use_tcp = false;
        if (tcp) {
            snprintf(d->status, sizeof d->status, "connecting (TCP)...");
            if (!tcp_open(d)) continue;
            d->use_tcp = true;
        } else {
            snprintf(d->status, sizeof d->status, "mounting (UDP)...");
            if (!udp_open()) continue;
        }
        d->conn_id = 0;
        n = tnfs_req(0x00, m, ml);
        if (n < 0 && tcp) tcp_shut(d);
    }
    if (n < 0) {
        snprintf(d->status, sizeof d->status, "no answer from %s:%u (UDP nor TCP)",
                 ipaddr_ntoa(&d->ip), d->port);
        return false;
    }
    if (rx[4] != 0) {
        snprintf(d->status, sizeof d->status, "MOUNT %s refused: %s (0x%02x)",
                 d->path, tnfs_err(rx[4]), rx[4]);
        tcp_shut(d);
        return false;
    }
    d->conn_id = (uint16_t)(rx[0] | (rx[1] << 8));
    d->vmin = n > 5 ? rx[5] : 0;
    d->vmaj = n > 6 ? rx[6] : 0;
    d->mounted = true;
    return true;
}

static void tnfs_unmount(void)
{
    vdrive_t *d = cur;
    if (d->mounted) tnfs_req(0x01, NULL, 0);
    d->mounted = false;
    tcp_shut(d);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------- virtual FAT16 over TNFS ----------------
 *
 * Partition layout (VFAT_SECTORS sectors of 512 bytes, 2 per cluster):
 *   0                      boot sector (BPB)
 *   1 .. SPF               FAT 1        (both FATs are generated, identical)
 *   SPF+1 .. 2*SPF         FAT 2
 *   2*SPF+1 .. +RDLEN      root directory
 *   DATREC ..              clusters 2..
 *
 * The whole tree below the mount point is scanned once after mounting and
 * every file and directory gets a fixed, contiguous run of clusters. The FAT
 * therefore never changes while GEMDOS has it cached. All drives share one
 * node table and name pool; each drive owns a contiguous range of it.
 */
#define V_SPC       2
#define V_RES       1
#define V_ROOTENT   512
#define V_RDLEN     (V_ROOTENT * 32 / 512)
#define V_SPF       64
#define V_DATREC    (V_RES + 2 * V_SPF + V_RDLEN)
#define V_NCL       ((VFAT_SECTORS - V_DATREC) / V_SPC)
#define V_CLBYTES   (V_SPC * 512)

#define VNODES      1024

typedef struct {
    uint16_t parent;                /* the drive's root node: itself */
    uint16_t child_start, child_count;
    uint16_t first_cl, ncl;         /* generated cluster run (scan)          */
    uint16_t start_cl;              /* start cluster as GEMDOS has it now    */
    uint8_t  isdir;
    uint8_t  flags;                 /* VF_*                                  */
    char     n83[11];
    uint32_t size, mtime;
    uint32_t name_off;
    uint32_t sig;                   /* cluster chain at the last sync        */
} vnode_t;

#define VF_RO       0x01            /* no write permission on the server     */
#define VF_DEL      0x02            /* deleted on the server                 */
#define VF_SEEN     0x04            /* found again by the current sync       */

#define TMP_NAME    "/.A2T.TMP"     /* hidden: the scan skips names with '.' */
#define NODE_SPARE  64              /* room per drive for new files and dirs */
#define POOL_SPARE  2048

/* per drive: sector written since the scan (data in TMP_NAME), and since the
   last sync */
static uint8_t wbits[VDRIVES_MAX][(VFAT_SECTORS + 7) / 8];
static uint8_t dbits[VDRIVES_MAX][(VFAT_SECTORS + 7) / 8];
#define BIT(m, r)   ((m)[(r) >> 3] & (1u << ((r) & 7)))

static vnode_t  vn[VNODES];
static uint16_t vn_count;
static char     vpool[32768];
static uint32_t vpool_len;
static uint16_t valloc[VNODES];     /* nodes that own clusters, per drive in cluster order */
static uint16_t valloc_n;

static char *vname(uint16_t i) { return vpool + vn[i].name_off; }

/* server path of node i, relative to the mount point ("/" for the root) */
static int vpath(uint16_t i, char *buf, int max)
{
    if (vn[i].parent == i) { snprintf(buf, (size_t)max, "/"); return 1; }
    int n = vpath(vn[i].parent, buf, max);
    if (n > 1 && n < max - 1) buf[n++] = '/';
    n += snprintf(buf + n, (size_t)(max - n), "%s", vname(i));
    return n < max ? n : max - 1;
}

static bool valid83(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'()-@^_`{}~", c);
}

/* long server name -> unique 8.3 name among the siblings of dir d */
static void make83(uint16_t d, const char *name, char out[11])
{
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    char base[9] = "", ext[4] = "";
    int bl = 0, el = 0;
    for (const char *p = name; *p && (!dot || p < dot) && bl < 8; p++) {
        char c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        if (c == ' ' || c == '.') continue;
        base[bl++] = valid83(c) ? c : '_';
    }
    for (const char *p = dot ? dot + 1 : ""; *p && el < 3; p++) {
        char c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        if (c == ' ' || c == '.') continue;
        ext[el++] = valid83(c) ? c : '_';
    }
    if (!bl) base[bl++] = '_';
    for (int tries = 0; tries < 100; tries++) {
        memset(out, ' ', 11);
        if (tries == 0) {
            memcpy(out, base, (size_t)bl);
        } else {
            char tail[4];
            int tl = snprintf(tail, sizeof tail, "~%d", tries);
            int keep = bl + tl > 8 ? 8 - tl : bl;
            memcpy(out, base, (size_t)keep);
            memcpy(out + keep, tail, (size_t)tl);
        }
        memcpy(out + 8, ext, (size_t)el);
        bool clash = false;
        /* siblings so far; the node being named is the last one (vn_count - 1) */
        for (uint16_t k = vn[d].child_start; k + 1 < vn_count; k++)
            if (!memcmp(vn[k].n83, out, 11)) { clash = true; break; }
        if (!clash) return;
    }
}

/* STAT: fills size, mtime, isdir, ro (nobody may write) */
static bool tnfs_stat(const char *path, uint32_t *size, uint32_t *mtime, bool *isdir, bool *ro)
{
    int n = tnfs_req(0x24, path, (int)strlen(path) + 1);
    if (n < 5 + 22 || rx[4] != 0) return false;
    uint16_t mode = le16(rx + 5);
    *isdir = (mode & 0170000) == 0040000;
    *ro = (mode & 0222) == 0;
    *size  = le32(rx + 11);
    *mtime = le32(rx + 19);
    return true;
}

static void vscan_dir(uint16_t d)
{
    static char path[256], child[320];      /* not reentrant: keep off the stack */
    vdrive_t *dr = cur;
    vpath(d, path, sizeof path);
    vn[d].child_start = vn_count;
    vn[d].child_count = 0;
    int n = tnfs_req(0x10, path, (int)strlen(path) + 1);    /* OPENDIR */
    if (n < 6 || rx[4] != 0) return;
    uint8_t h = rx[5];
    for (;;) {
        n = tnfs_req(0x11, &h, 1);                          /* READDIR */
        if (n < 6 || rx[4] != 0) break;
        rx[n < (int)sizeof rx ? n : (int)sizeof rx - 1] = 0;
        static char name[128];
        snprintf(name, sizeof name, "%s", (const char *)rx + 5);
        if (name[0] == '.') continue;                       /* ., .., hidden */
        if (vn_count >= dr->node_limit || (d == dr->root && vn[d].child_count >= V_ROOTENT - 1) ||
            vpool_len + strlen(name) + 1 > dr->pool_limit) { dr->skipped++; continue; }
        snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", name);
        uint32_t size = 0, mtime = 0;
        bool isdir = false, ro = false;
        if (!tnfs_stat(child, &size, &mtime, &isdir, &ro)) { dr->skipped++; continue; }
        uint16_t i = vn_count++;
        memset(&vn[i], 0, sizeof vn[i]);
        vn[i].parent = d;
        vn[i].isdir = isdir;
        vn[i].flags = ro && !isdir ? VF_RO : 0;
        vn[i].size = isdir ? 0 : size;
        vn[i].mtime = mtime;
        vn[i].name_off = vpool_len;
        strcpy(vpool + vpool_len, name);
        vpool_len += (uint32_t)strlen(name) + 1;
        make83(d, name, vn[i].n83);
        vn[d].child_count++;
        if (isdir) dr->dirs++; else dr->files++;
    }
    tnfs_req(0x12, &h, 1);                                  /* CLOSEDIR */
}

/* signature of a cluster chain: changes with any cluster or the length */
static uint32_t sig_add(uint32_t sig, uint32_t c) { return sig * 31u + c; }

static uint32_t run_sig(uint32_t first, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = sig_add(s, first + i);
    return s;
}

/* scan the current drive into the next free part of the node table */
static void vfat_scan(void)
{
    vdrive_t *d = cur;
    int left = nvd - (int)(d - vd);                         /* this and later drives */
    d->ready = false;
    d->files = d->dirs = d->skipped = 0;
    d->root = vn_count;
    d->node_limit = (uint16_t)(vn_count + (VNODES - vn_count) / left);
    d->pool_limit = vpool_len + (uint32_t)(sizeof vpool - vpool_len) / (uint32_t)left;
    uint16_t r = vn_count++;
    memset(&vn[r], 0, sizeof vn[r]);
    vn[r].parent = r;
    vn[r].isdir = 1;
    vn[r].name_off = 0;                                     /* vpool[0] = "" */

    /* breadth first: children of a directory are contiguous in vn[] */
    for (uint16_t i = r; i < vn_count; i++) {
        if (!vn[i].isdir) continue;
        snprintf(d->status, sizeof d->status, "scanning... %lu files, %lu dirs",
                 (unsigned long)d->files, (unsigned long)d->dirs);
        vscan_dir(i);
    }
    d->node_end = vn_count;

    /* fixed cluster runs, in node order */
    uint32_t next = 2;
    d->va0 = valloc_n;
    for (uint16_t i = r + 1; i < vn_count; i++) {
        uint32_t bytes = vn[i].isdir ? (uint32_t)(vn[i].child_count + 2) * 32 : vn[i].size;
        uint32_t ncl = (bytes + V_CLBYTES - 1) / V_CLBYTES;
        if (next + ncl > V_NCL + 2) {                       /* does not fit: hide */
            ncl = 0;
            vn[i].size = 0;
            d->skipped++;
        }
        vn[i].first_cl = vn[i].start_cl = ncl ? (uint16_t)next : 0;
        vn[i].ncl = (uint16_t)ncl;
        vn[i].sig = run_sig(vn[i].first_cl, ncl);
        if (ncl) valloc[valloc_n++] = i;
        next += ncl;
    }
    d->van = (uint16_t)(valloc_n - d->va0);

    /* keep room for files and directories the Atari creates */
    d->pool_end = vpool_len;
    if (d->node_limit > d->node_end + NODE_SPARE) d->node_limit = (uint16_t)(d->node_end + NODE_SPARE);
    if (d->pool_limit > d->pool_end + POOL_SPARE) d->pool_limit = d->pool_end + POOL_SPARE;
    vn_count = d->node_limit;
    vpool_len = d->pool_limit;
    int k = (int)(d - vd);
    memset(wbits[k], 0, sizeof wbits[k]);
    memset(dbits[k], 0, sizeof dbits[k]);
    d->tmp_open = d->tmp_used = d->wr_pending = false;
    __dmb();
    d->ready = true;
}

/* node of the current drive owning cluster c, or -1 */
static int vfind(uint32_t c)
{
    int lo = cur->va0, hi = (int)cur->va0 + cur->van - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const vnode_t *v = &vn[valloc[mid]];
        if (c < v->first_cl) hi = mid - 1;
        else if (c >= (uint32_t)v->first_cl + v->ncl) lo = mid + 1;
        else return valloc[mid];
    }
    return -1;
}

/* unix time -> DOS time/date */
static void dos_time(uint32_t t, uint16_t *time, uint16_t *date)
{
    if (t < 315532800u) t = 315532800u;                     /* 1980-01-01 */
    uint32_t days = t / 86400, sec = t % 86400;
    *time = (uint16_t)(((sec / 3600) << 11) | (((sec / 60) % 60) << 5) | ((sec % 60) / 2));
    /* civil from days (Howard Hinnant) */
    int32_t z = (int32_t)days + 719468;
    int32_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }

static void vdirent(uint8_t *e, const char n83[11], uint8_t attr, uint32_t mtime,
                    uint16_t cl, uint32_t size)
{
    uint16_t t, d;
    memcpy(e, n83, 11);
    e[11] = attr;
    dos_time(mtime, &t, &d);
    put16(e + 22, t);
    put16(e + 24, d);
    put16(e + 26, cl);
    put32(e + 28, size);
}

/* directory entry number k of directory node d (root: no . and ..) */
static void vdir_entry(uint16_t d, uint32_t k, uint8_t *e)
{
    memset(e, 0, 32);
    if (d != cur->root) {
        if (k == 0) { vdirent(e, ".          ", 0x10, vn[d].mtime, vn[d].first_cl, 0); return; }
        if (k == 1) {
            uint16_t p = vn[d].parent;
            vdirent(e, "..         ", 0x10, vn[d].mtime, p != cur->root ? vn[p].first_cl : 0, 0);
            return;
        }
        k -= 2;
    }
    if (k >= vn[d].child_count) return;
    const vnode_t *v = &vn[vn[d].child_start + k];
    vdirent(e, v->n83, v->isdir ? 0x10 : (v->flags & VF_RO) ? 0x01 : 0x00,
            v->mtime, v->first_cl, v->size);
}

/* open file handle cache (one per drive) */
static void vfile_close(void)
{
    vdrive_t *d = cur;
    if (d->vf_node >= 0 && d->mounted) tnfs_req(0x23, &d->vf_handle, 1);   /* CLOSE */
    d->vf_node = -1;
}

static bool vfile_read1(uint16_t node, uint32_t off, uint8_t *dst, uint32_t len)
{
    vdrive_t *d = cur;
    if (d->vf_node != node) {
        vfile_close();
        uint8_t req[300];
        req[0] = 0x01; req[1] = 0x00;                      /* O_RDONLY */
        req[2] = 0x00; req[3] = 0x00;                      /* mode     */
        int pl = vpath(node, (char *)req + 4, (int)sizeof req - 4);
        int n = tnfs_req(0x29, req, 4 + pl + 1);           /* OPEN     */
        if (n < 6 || rx[4] != 0) return false;
        d->vf_handle = rx[5];
        d->vf_node = node;
        d->vf_pos = 0;
    }
    if (off != d->vf_pos) {
        uint8_t req[6] = { d->vf_handle, 0 };              /* SEEK_SET */
        put32(req + 2, off);
        int n = tnfs_req(0x25, req, 6);                    /* LSEEK    */
        if (n < 5 || rx[4] != 0) { vfile_close(); return false; }
        d->vf_pos = off;
    }
    while (len) {
        uint16_t want = len > 512 ? 512 : (uint16_t)len;
        uint8_t req[3] = { d->vf_handle, (uint8_t)(want & 0xff), (uint8_t)(want >> 8) };
        int n = tnfs_req(0x21, req, 3);                    /* READ     */
        if (n < 0) return false;                           /* session gone */
        if (n < 7 || rx[4] != 0) break;                    /* EOF/error: zeros */
        uint16_t got = le16(rx + 5);
        if (got > n - 7) got = (uint16_t)(n - 7);
        if (got > want) got = want;
        memcpy(dst, rx + 7, got);
        dst += got; len -= got; d->vf_pos += got;
        if (!got) break;
    }
    return true;
}

static bool vfile_read(uint16_t node, uint32_t off, uint8_t *dst, uint32_t len)
{
    uint8_t gen = cur->gen;
    if (vfile_read1(node, off, dst, len)) return true;
    return cur->gen != gen && vfile_read1(node, off, dst, len);   /* new session */
}

/* ---------------- writing ----------------
 *
 * GEMDOS writes plain sectors: data first, to clusters that are still free,
 * then (at Fclose) the FAT and the directory entry that say which file the
 * data belongs to. So every written sector goes to TMP_NAME on the server at
 * offset rel * 512 and is read back from there: the Atari always sees what it
 * wrote. When the Atari has written nothing for a second, vsync() walks the
 * directory tree as GEMDOS has it now and applies the differences to the
 * server: new, changed, renamed and deleted files and directories.
 */
#define TO_RDONLY   0x0001
#define TO_WRONLY   0x0002
#define TO_RDWR     0x0003
#define TO_CREAT    0x0100
#define TO_TRUNC    0x0200

static bool tnfs_open(const char *path, uint16_t flags, uint8_t *h)
{
    uint8_t req[300];
    req[0] = flags & 0xff; req[1] = flags >> 8;
    req[2] = 0xa4; req[3] = 0x01;                          /* mode 0644 */
    int pl = snprintf((char *)req + 4, sizeof req - 4, "%s", path);
    int n = tnfs_req(0x29, req, 4 + pl + 1);
    if (n < 6 || rx[4] != 0) return false;
    *h = rx[5];
    return true;
}

static bool tnfs_seek(uint8_t h, uint32_t off)
{
    uint8_t req[6] = { h, 0 };                             /* SEEK_SET */
    put32(req + 2, off);
    int n = tnfs_req(0x25, req, 6);
    return n >= 5 && rx[4] == 0;
}

static bool tnfs_write(uint8_t h, const uint8_t *buf, uint16_t len)
{
    static uint8_t req[3 + 512];
    req[0] = h; req[1] = len & 0xff; req[2] = len >> 8;
    memcpy(req + 3, buf, len);
    int n = tnfs_req(0x22, req, 3 + len);
    return n >= 7 && rx[4] == 0 && le16(rx + 5) == len;
}

static void tnfs_close(uint8_t h) { tnfs_req(0x23, &h, 1); }

/* UNLINK, MKDIR, RMDIR: one path; false with the error in rx[4] */
static bool tnfs_path(uint8_t cmd, const char *path)
{
    int n = tnfs_req(cmd, path, (int)strlen(path) + 1);
    return n >= 5 && rx[4] == 0;
}

static bool tnfs_rename(const char *from, const char *to)
{
    static char req[512];
    int a = (int)strlen(from) + 1, b = (int)strlen(to) + 1;
    if (a + b > (int)sizeof req) return false;
    memcpy(req, from, (size_t)a);
    memcpy(req + a, to, (size_t)b);
    int n = tnfs_req(0x28, req, a + b);
    return n >= 5 && rx[4] == 0;
}

/* one sector of TMP_NAME */
static bool tmp_io1(uint32_t rel, uint8_t *buf, bool write)
{
    vdrive_t *d = cur;
    if (!d->tmp_open) {
        uint16_t fl = TO_RDWR | TO_CREAT | (d->tmp_used ? 0 : TO_TRUNC);
        if (!tnfs_open(TMP_NAME, fl, &d->tmp_handle)) {
            printf("net: cannot create %s: %s (0x%02x)\n", TMP_NAME, tnfs_err(rx[4]), rx[4]);
            return false;
        }
        d->tmp_open = d->tmp_used = true;
    }
    if (!tnfs_seek(d->tmp_handle, rel * 512u)) return false;
    if (write) return tnfs_write(d->tmp_handle, buf, 512);
    uint8_t req[3] = { d->tmp_handle, 0x00, 0x02 };        /* 512 bytes */
    int n = tnfs_req(0x21, req, 3);
    if (n < 7 || rx[4] != 0) return false;
    uint16_t got = le16(rx + 5);
    if (got > 512 || got > n - 7) return false;
    memcpy(buf, rx + 7, got);
    return true;
}

static bool tmp_io(uint32_t rel, uint8_t *buf, bool write)
{
    uint8_t gen = cur->gen;
    if (tmp_io1(rel, buf, write)) return true;
    return cur->gen != gen && tmp_io1(rel, buf, write);         /* new session */
}

static bool vfat_write(uint32_t rel, const uint8_t *buf)
{
    vdrive_t *d = cur;
    int k = (int)(d - vd);
    if (rel == 0) return true;                              /* boot sector: fixed */
    if (!d->ready || !tmp_io(rel, (uint8_t *)buf, true)) return false;
    wbits[k][rel >> 3] |= (uint8_t)(1u << (rel & 7));
    dbits[k][rel >> 3] |= (uint8_t)(1u << (rel & 7));
    d->wr_pending = true;
    d->last_wr_ms = to_ms_since_boot(get_absolute_time());
    return true;
}

/* drive i cannot be used: show NET_ERR.TXT with the reason instead of an
   empty drive, and tell GEMDOS to read it again */
static void vdrive_fail(int i, const char *reason)
{
    vdrive_t *d = &vd[i];
    int n = snprintf(d->errtxt, sizeof d->errtxt,
                     "ACSI2TNFS: network drive not available\r\n"
                     "\r\n"
                     "Reason : %s\r\n"
                     "Server : %s:%u\r\n"
                     "Folder : %s\r\n"
                     "\r\n"
                     "Check that the TNFS server runs and can be reached,\r\n"
                     "then press N on the USB console of the adapter or\r\n"
                     "restart it. The drive reads its folders again then.\r\n",
                     reason, d->server, d->port, d->path);
    d->errlen = (uint16_t)(n < 0 ? 0 : n < (int)sizeof d->errtxt ? n : (int)sizeof d->errtxt - 1);
    d->ready = false;
    d->failed = true;
    disk_changed(1 + i);
}

/* one sector of the current drive's virtual partition */
static bool vfat_sector(uint32_t rel, uint8_t *buf)
{
    vdrive_t *d = cur;
    memset(buf, 0, 512);
    if (rel == 0) { vfat_bootsector(buf); return true; }
    if (!d->ready) {                                        /* NET_ERR.TXT or empty */
        if (!d->failed) return true;
        if (rel < 1 + 2 * V_SPF) {
            if ((rel - 1) % V_SPF == 0) {                   /* clusters 0, 1 and 2 */
                put16(buf, 0xfff8);
                put16(buf + 2, 0xffff);
                put16(buf + 4, 0xffff);
            }
        } else if (rel == 1 + 2 * V_SPF) {                  /* first root sector */
            vdirent(buf, "NET_ERR TXT", 0x01, 0, 2, d->errlen);
        } else if (rel == V_DATREC) {                       /* cluster 2 */
            memcpy(buf, d->errtxt, d->errlen);
        }
        return true;
    }
    if (BIT(wbits[d - vd], rel)) return tmp_io(rel, buf, false);   /* written by the Atari */
    if (rel < 1 + 2 * V_SPF) {                              /* FAT 1 and FAT 2 */
        uint32_t c0 = ((rel - 1) % V_SPF) * 256;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = c0 + i;
            uint16_t v = 0;
            if (c == 0) v = 0xfff8;
            else if (c == 1) v = 0xffff;
            else if (d->ready) {
                int nd = vfind(c);
                if (nd >= 0) v = c + 1 < (uint32_t)vn[nd].first_cl + vn[nd].ncl ? (uint16_t)(c + 1) : 0xffff;
            }
            put16(buf + i * 2, v);
        }
        return true;
    }
    if (rel < V_DATREC) {                                   /* root directory */
        if (!d->ready) return true;
        uint32_t k0 = (rel - (1 + 2 * V_SPF)) * 16;
        for (uint32_t k = 0; k < 16; k++) vdir_entry(d->root, k0 + k, buf + k * 32);
        return true;
    }
    if (!d->ready) return true;
    uint32_t c = 2 + (rel - V_DATREC) / V_SPC;
    uint32_t s = (rel - V_DATREC) % V_SPC;
    int nd = vfind(c);
    if (nd < 0 || (vn[nd].flags & VF_DEL)) return true;     /* free or deleted: zeros */
    uint32_t off = ((c - vn[nd].first_cl) * V_SPC + s) * 512;
    if (vn[nd].isdir) {
        for (uint32_t k = 0; k < 16; k++) vdir_entry((uint16_t)nd, off / 32 + k, buf + k * 32);
        return true;
    }
    if (off >= vn[nd].size) return true;
    uint32_t len = vn[nd].size - off;
    return vfile_read((uint16_t)nd, off, buf, len > 512 ? 512 : len);
}

/* ---------------- sync: the Atari's view -> the server ---------------- */

static uint8_t fsec[512];                   /* last FAT sector read by vfat_get */
static int32_t fsec_rel = -1;

static bool valid_cl(uint32_t c) { return c >= 2 && c < V_NCL + 2; }

/* the request core1 is waiting for (an Atari read or write of a TNFS drive).
   Called from net_poll and, between its own steps, by a sync: one large file
   can take seconds to sync, longer than core1 may wait (9 s). */
static void vreq_service(void)
{
    if (vreq_state != 1) return;
    vdrive_t *save = cur;
    bool ok = vreq_drive < (uint32_t)nvd;
    if (ok) cur = &vd[vreq_drive];
    for (uint32_t i = 0; i < vreq_n && ok; i++)
        ok = vreq_write ? vfat_write(vreq_rel + i, vreq_buf + i * 512)
                        : vfat_sector(vreq_rel + i, vreq_buf + i * 512);
    if (vreq_write) fsec_rel = -1;          /* the FAT may have changed */
    cur = save;
    __dmb();
    vreq_state = ok ? 2 : 3;
}

/* sectors written since the last sync, as taken over by the running sync;
   writes during the sync go to dbits again for the next one */
static uint8_t sbits[(VFAT_SECTORS + 7) / 8];

/* FAT entry of cluster c as GEMDOS has it now */
static uint32_t vfat_get(uint32_t c)
{
    uint32_t rel = 1 + c / 256;
    if ((int32_t)rel != fsec_rel) {
        if (!vfat_sector(rel, fsec)) { fsec_rel = -1; return 0xffff; }
        fsec_rel = (int32_t)rel;
    }
    return le16(fsec + (c % 256) * 2);
}

/* chain from cluster c: its signature, and whether the Atari wrote one of
   its sectors since the last sync */
static uint32_t chain_info(uint32_t c, bool *dirty)
{
    const uint8_t *db = sbits;
    uint32_t sig = 0;
    *dirty = false;
    for (uint32_t i = 0; valid_cl(c) && i < V_NCL; i++) {
        sig = sig_add(sig, c);
        uint32_t rel = V_DATREC + (c - 2) * V_SPC;
        for (uint32_t s = 0; s < V_SPC; s++)
            if (BIT(db, rel + s)) *dirty = true;
        c = vfat_get(c);
    }
    return sig;
}

/* "NAME    EXT" -> "NAME.EXT" */
static void n83_str(const char *n83, char *out)
{
    int o = 0;
    for (int i = 0; i < 8 && n83[i] != ' '; i++) out[o++] = n83[i];
    if (n83[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && n83[i] != ' '; i++) out[o++] = n83[i];
    }
    out[o] = 0;
}

/* server path of a (new) name in directory node dir */
static void child_path(uint16_t dir, const char *n83, char *buf, int max)
{
    char name[13];
    n83_str(n83, name);
    int n = vpath(dir, buf, max);
    snprintf(buf + n, (size_t)(max - n), "%s%s", n > 1 ? "/" : "", name);
}

/* give node i the name n83 in directory dir */
static bool vnode_place(uint16_t i, uint16_t dir, const char *n83)
{
    vdrive_t *d = cur;
    char name[13];
    n83_str(n83, name);
    size_t l = strlen(name) + 1;
    if (d->pool_end + l > d->pool_limit) return false;
    memcpy(vpool + d->pool_end, name, l);
    vn[i].name_off = d->pool_end;
    d->pool_end += (uint32_t)l;
    memcpy(vn[i].n83, n83, 11);
    vn[i].parent = dir;
    return true;
}

static int vnode_new(uint16_t dir, const char *n83, bool isdir)
{
    vdrive_t *d = cur;
    if (d->node_end >= d->node_limit) return -1;
    uint16_t i = d->node_end;
    memset(&vn[i], 0, sizeof vn[i]);
    if (!vnode_place(i, dir, n83)) return -1;
    vn[i].isdir = isdir;
    vn[i].start_cl = 0xffff;                                /* never synced */
    vn[i].size = 0xffffffffu;
    d->node_end++;
    return i;
}

/* write file node n (size bytes, chain from cluster c) to the server. With
   every sector written by the Atari it goes straight into the file; else a
   new file is built next to it from the old data and the new, then swapped. */
static bool vbuild(uint16_t n, uint32_t c, uint32_t size)
{
    static char path[256], tmp[256];
    static uint8_t sec[512];
    const uint8_t *wb = wbits[cur - vd];
    vpath(n, path, sizeof path);

    bool direct = true;
    uint32_t left = size, cc = c;
    for (uint32_t i = 0; left && valid_cl(cc) && i < V_NCL; i++, cc = vfat_get(cc))
        for (uint32_t s = 0; s < V_SPC && left; s++) {
            if (!BIT(wb, V_DATREC + (cc - 2) * V_SPC + s)) direct = false;
            left -= left > 512 ? 512 : left;
        }
    if (direct) {
        snprintf(tmp, sizeof tmp, "%s", path);
    } else {
        int k = vpath(vn[n].parent, tmp, sizeof tmp);
        snprintf(tmp + k, sizeof tmp - (size_t)k, "%s.A2T.NEW", k > 1 ? "/" : "");
    }
    uint8_t h;
    if (!tnfs_open(tmp, TO_WRONLY | TO_CREAT | TO_TRUNC, &h)) {
        printf("sync: cannot create %s: %s (0x%02x)\n", tmp, tnfs_err(rx[4]), rx[4]);
        return false;
    }
    bool ok = true;
    left = size; cc = c;
    for (uint32_t i = 0; ok && left && valid_cl(cc) && i < V_NCL; i++, cc = vfat_get(cc))
        for (uint32_t s = 0; ok && s < V_SPC && left; s++) {
            uint16_t len = left > 512 ? 512 : (uint16_t)left;
            ok = vfat_sector(V_DATREC + (cc - 2) * V_SPC + s, sec) && tnfs_write(h, sec, len);
            left -= len;
            vreq_service();
        }
    tnfs_close(h);
    if (left) ok = false;                                   /* FAT not complete (yet) */
    if (ok && !direct) {
        vfile_close();
        tnfs_path(0x26, path);                              /* old version, if any */
        ok = tnfs_rename(tmp, path);
    }
    return ok;
}

/* one directory entry e in directory node dir: match it with a node, create,
   rename or rewrite on the server as needed */
static void vsync_entry(uint16_t dir, const uint8_t *e, uint16_t *queue, int *nq, int *acts)
{
    static char path[256], path2[256];
    vdrive_t *d = cur;
    const char *n83 = (const char *)e;
    bool isdir = (e[11] & 0x10) != 0;
    uint32_t c = le16(e + 26), size = isdir ? 0 : le32(e + 28);
    int n = -1;

    /* same name in the same directory */
    for (uint16_t i = d->root + 1; i < d->node_end && n < 0; i++)
        if (!(vn[i].flags & (VF_DEL | VF_SEEN)) && vn[i].parent == dir &&
            vn[i].isdir == isdir && !memcmp(vn[i].n83, n83, 11))
            n = i;
    /* same start cluster under another name or in another directory */
    for (uint16_t i = d->root + 1; i < d->node_end && n < 0 && valid_cl(c); i++)
        if (!(vn[i].flags & (VF_DEL | VF_SEEN)) && vn[i].start_cl == c && vn[i].isdir == isdir) {
            vpath(i, path, sizeof path);
            child_path(dir, n83, path2, sizeof path2);
            vfile_close();
            bool ok = tnfs_rename(path, path2);
            printf("sync: rename %s -> %s %s\n", path, path2, ok ? "ok" : tnfs_err(rx[4]));
            (*acts)++;
            if (ok && vnode_place(i, dir, n83)) n = i;
        }
    if (n < 0) {
        n = vnode_new(dir, n83, isdir);
        if (n < 0) { printf("sync: no room for %.11s\n", n83); return; }
        if (isdir) {
            vpath((uint16_t)n, path, sizeof path);
            bool ok = tnfs_path(0x13, path);
            printf("sync: mkdir %s %s\n", path, ok ? "ok" : tnfs_err(rx[4]));
            (*acts)++;
        }
    }
    vn[n].flags |= VF_SEEN;
    if (isdir) {
        vn[n].start_cl = (uint16_t)c;
        queue[(*nq)++] = (uint16_t)n;
        return;
    }
    bool dirty;
    uint32_t sig = chain_info(c, &dirty);
    if (dirty || size != vn[n].size || c != vn[n].start_cl || sig != vn[n].sig) {
        bool ok = vbuild((uint16_t)n, c, size);
        vpath((uint16_t)n, path, sizeof path);
        printf("sync: write %s, %lu bytes %s\n", path, (unsigned long)size, ok ? "ok" : "FAILED");
        (*acts)++;
        if (ok) { vn[n].size = size; vn[n].start_cl = (uint16_t)c; vn[n].sig = sig; }
    }
}

/* the Atari wrote to the current drive and is quiet now: walk the directory
   tree as GEMDOS has it and bring the server in line */
static void vsync(void)
{
    static uint8_t sec[512];
    static uint16_t queue[VNODES];
    static char path[256];
    vdrive_t *d = cur;
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    int nq = 0, acts = 0;

    int k0 = (int)(d - vd);
    uint8_t gen0 = d->gen;
    memcpy(sbits, dbits[k0], sizeof sbits);
    memset(dbits[k0], 0, sizeof dbits[0]);
    d->wr_pending = false;                  /* writes from now on: next sync */
    fsec_rel = -1;
    for (uint16_t i = d->root; i < d->node_end; i++) vn[i].flags &= (uint8_t)~VF_SEEN;
    vn[d->root].flags |= VF_SEEN;
    queue[nq++] = d->root;
    for (int qi = 0; qi < nq; qi++) {
        uint16_t dir = queue[qi];
        bool root = dir == d->root;
        uint32_t c = vn[dir].start_cl, rel0 = 1 + 2 * V_SPF, nsec = root ? V_RDLEN : V_SPC;
        bool end = false;
        for (uint32_t i = 0; !end && i < V_NCL; i++) {
            if (!root) {
                if (!valid_cl(c)) break;
                rel0 = V_DATREC + (c - 2) * V_SPC;
            }
            for (uint32_t s = 0; !end && s < nsec; s++) {
                if (!vfat_sector(rel0 + s, sec)) {
                    printf("sync: read error, try again later\n");
                    for (uint32_t b = 0; b < sizeof sbits; b++) dbits[k0][b] |= sbits[b];
                    d->wr_pending = true;
                    d->last_wr_ms = to_ms_since_boot(get_absolute_time());
                    return;
                }
                for (int k = 0; k < 16 && !end; k++) {
                    const uint8_t *e = sec + 32 * k;
                    if (e[0] == 0) end = true;
                    else if (e[0] != 0xe5 && e[0] != '.' && !(e[11] & 0x08))   /* not deleted, ., .., label */
                        vsync_entry(dir, e, queue, &nq, &acts);
                }
                vreq_service();
            }
            if (root) break;
            c = vfat_get(c);
        }
    }
    /* nodes GEMDOS no longer has: children before their directory */
    for (int i = d->node_end - 1; i > d->root; i--) {
        vnode_t *v = &vn[i];
        if (v->flags & (VF_SEEN | VF_DEL)) continue;
        vpath((uint16_t)i, path, sizeof path);
        vfile_close();
        bool ok = tnfs_path(v->isdir ? 0x14 : 0x26, path);
        printf("sync: %s %s %s\n", v->isdir ? "rmdir" : "delete", path, ok ? "ok" : tnfs_err(rx[4]));
        v->flags |= VF_DEL;
        acts++;
        vreq_service();
    }
    vfile_close();
    if (d->gen != gen0) {                   /* new session halfway: files may be */
        for (uint32_t b = 0; b < sizeof sbits; b++) dbits[k0][b] |= sbits[b];   /* incomplete */
        d->wr_pending = true;
        d->last_wr_ms = to_ms_since_boot(get_absolute_time());
    }
    printf("sync: TNFS %d, %d change(s), %lu ms\n", (int)(d - vd) + 1, acts,
           (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
}

/* close and unmount every drive */
static void drives_down(void)
{
    for (int i = 0; i < nvd; i++) {
        cur = &vd[i];
        cur->ready = false;
        vfile_close();
        if (cur->tmp_open && cur->mounted) tnfs_close(cur->tmp_handle);
        cur->tmp_open = false;
        tnfs_unmount();
    }
    cur = NULL;
}

/* connect, mount and scan every drive: at start-up and by console N */
static void tnfs_connect_and_scan(void)
{
    drives_down();
    vn_count = 0;
    valloc_n = 0;
    vpool[0] = 0;
    vpool_len = 1;
    for (int i = 0; i < nvd; i++) vd[i].failed = false;
    if (!wifi_connect()) {
        if (g_set.rtc_enabled && clk_state != CLK_VALID) clk_state = CLK_NONE;
        char why[120];
        snprintf(why, sizeof why, "no Wi-Fi (%s)", wifi_status);
        for (int i = 0; i < nvd; i++) vdrive_fail(i, why);
        return;
    }
    /* the time first: the driver may be waiting for it at boot */
    if (g_set.rtc_enabled) {
        if (clk_state != CLK_VALID) clk_state = CLK_NTP;
        ntp_fails = 0;
        bool ok = ntp_sync();
        if (!ok) ntp_fails = 1;     /* CLK_NTP stays: quick retries from net_poll */
        ntp_next = make_timeout_time_ms(ok ? NTP_RESYNC_MS : NTP_FAST_MS);
    }
    if (!nvd) { printf("net: no TNFS drive configured\n"); return; }
    for (int i = 0; i < nvd; i++) {
        vdrive_t *d = cur = &vd[i];
        d->dir[0] = 0;
        printf("net: TNFS %d: %s:%u %s\n", i + 1, d->server, d->port, d->path);
        if (!tnfs_mount()) {
            printf("net: TNFS %d %s\n", i + 1, d->status);
            vdrive_fail(i, d->status);
            continue;
        }
        vfat_scan();
        for (uint16_t k = d->root + 1; k < d->node_end; k++) {
            char p[256];
            vpath(k, p, sizeof p);
            printf("  %.8s.%.3s %s %8lu  cl %5u+%-4u %s\n", vn[k].n83, vn[k].n83 + 8,
                   vn[k].isdir ? "<DIR>" : "     ", (unsigned long)vn[k].size,
                   vn[k].first_cl, vn[k].ncl, p);
        }
        int len = 0;
        for (uint16_t k = 0; k < vn[d->root].child_count && len < (int)sizeof d->dir - 40; k++) {
            const vnode_t *v = &vn[vn[d->root].child_start + k];
            len += snprintf(d->dir + len, sizeof d->dir - (size_t)len, "%s%s%s",
                            len ? ", " : "  ", vpool + v->name_off, v->isdir ? "\\" : "");
        }
        if (vn[d->root].child_count && len < (int)sizeof d->dir - 3) strcpy(d->dir + len, "\r\n");
        snprintf(d->status, sizeof d->status,
                 "OK! %s, TNFS %u.%u: %lu files, %lu dirs%s",
                 d->use_tcp ? "TCP" : "UDP", d->vmaj, d->vmin,
                 (unsigned long)d->files, (unsigned long)d->dirs, d->skipped ? " (some skipped)" : "");
        printf("net: TNFS %d %s\n", i + 1, d->status);
        disk_changed(1 + i);                /* new tree: GEMDOS must re-read */
    }
    cur = NULL;
}

void net_init(void)
{
    clk_state = !g_set.rtc_enabled ? CLK_OFF : g_net.ssid[0] ? CLK_WAIT : CLK_NONE;
    clock_restore();
    if (g_net.ssid[0]) {
        snprintf(wifi_status, sizeof wifi_status, "idle (not connected yet)");
        test_req = true;                            /* auto-connect at start-up */
    }
}

void net_poll(void)
{
    net_bridge_publish();                           /* Wi-Fi state for NET_INFO */
    if (g_set.rtc_enabled && !test_req && net_link_up() && (clk_sync_now || time_reached(ntp_next))) {
        clk_sync_now = false;
        bool ok = ntp_sync();
        if (ok) ntp_fails = 0;
        else if (ntp_fails < NTP_FAST_RETRIES && ++ntp_fails >= NTP_FAST_RETRIES &&
                 clk_state != CLK_VALID)
            clk_state = CLK_NONE;   /* the driver stops waiting */
        ntp_next = make_timeout_time_ms(ok ? NTP_RESYNC_MS :
                                        ntp_fails < NTP_FAST_RETRIES ? NTP_FAST_MS : NTP_RETRY_MS);
    }
    if (test_req) {
        test_req = false;
        tnfs_connect_and_scan();
    }
    vreq_service();
    /* a second after the last write: bring the server in line */
    uint32_t now = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < nvd && vreq_state != 1; i++)
        if (vd[i].wr_pending && now - vd[i].last_wr_ms > 1000) {
            cur = &vd[i];
            vsync();
            cur = NULL;
        }
}

#endif
