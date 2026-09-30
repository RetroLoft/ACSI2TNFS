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
static int g_net_drive = -1;            /* slot mounted as the TNFS drive */

settings_t g_set, g_stage;

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
    g_net_drive = -1;
    for (int i = 0; i < SET_MAX_DRIVES; i++) {
        const set_drive_t *d = &g_set.drv[i];
        if (d->state != DRV_ENABLED || d->type != DRV_TYPE_TNFS) continue;
        snprintf(g_net.server, sizeof g_net.server, "%s", d->host);
        snprintf(g_net.path, sizeof g_net.path, "%s", d->mount_path[0] ? d->mount_path : "/");
        g_net.port = d->port ? d->port : 16384;
        g_net_drive = i;
        break;
    }
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
static char tnfs_status[96] = "-";
static char tnfs_dir[240]   = "";

static volatile bool test_req;
static volatile bool cfg_new;
static uint8_t cfg_blk[512];

extern acsi_cfg_t g_cfg;

/* core1 -> core0 hand-over */
void net_settings_from_atari(const uint8_t *blk512)
{
    memcpy(cfg_blk, blk512, sizeof cfg_blk);
    cfg_new = true;
}

void net_request_test(void) { test_req = true; }

uint32_t net_status_text(char *p, uint32_t max)
{
    int n = snprintf(p, max,
        "\r\nWi-Fi SSID : %s\r\n"
        "Wi-Fi      : %s\r\n"
        "TNFS server: %s:%u  path %s\r\n"
        "TNFS       : %s\r\n"
        "%s",
        g_net.ssid[0] ? g_net.ssid : "(not set)", wifi_status,
        g_net.server, g_net.port, g_net.path, tnfs_status, tnfs_dir);
    return n < 0 ? 0 : ((uint32_t)n < max ? (uint32_t)n : max - 1);
}

void net_console_status(void)
{
    char buf[512];
    net_status_text(buf, sizeof buf);
    for (char *q = buf; *q; q++) if (*q != '\r') putchar(*q);
    printf("\n");
}

/* ---------------- virtual partition: core1 <-> core0 hand-over ---------------- */

/* core1 posts a sector request, core0 (network) fills the buffer */
static volatile int      vreq_state;         /* 0 idle, 1 pending, 2 done, 3 failed */
static volatile uint32_t vreq_rel, vreq_n;
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

/* core1: read n sectors of the virtual partition into buf */
bool net_vread(uint32_t rel, uint32_t n, uint8_t *buf)
{
    if (!BOARD_HAS_WIFI) return false;
    if (rel == 0 && n == 1) { vfat_bootsector(buf); return true; }
    if (vreq_state == 1) return false;          /* core0 still busy */
    vreq_buf = buf;
    vreq_rel = rel;
    vreq_n = n;
    __dmb();
    vreq_state = 1;
    uint32_t t0 = time_us_32();
    while (vreq_state == 1) {
        if (!gpio_get(PIN_RST)) return false;   /* Atari reset */
        if (time_us_32() - t0 > 9000000) return false;
    }
    return vreq_state == 2;
}

#if !BOARD_HAS_WIFI

bool net_link_up(void) { return false; }
void net_init(void) { }
void net_poll(void) { cfg_new = false; test_req = false; }

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

/* apply "SSID\0PASS\0SERVER\0PATH\0" from the Atari; empty = keep */
static void apply_cfg_block(void)
{
    const char *f[4];
    const char *q = (const char *)cfg_blk, *end = q + sizeof cfg_blk;
    for (int i = 0; i < 4; i++) {
        f[i] = q;
        while (q < end && *q) q++;
        if (q < end) q++;
    }
    /* CONFIG.TOS option 3: Wi-Fi + the first TNFS drive, applied at once */
    set_drive_t *drv = &g_stage.drv[g_net_drive >= 0 ? g_net_drive : 0];
    struct { char *dst; size_t sz; } d[4] = {
        { g_stage.ssid, 33 }, { g_stage.pass, 65 },
        { drv->host, sizeof drv->host }, { drv->mount_path, sizeof drv->mount_path } };
    for (int i = 0; i < 4; i++) {
        if (f[i] >= end || !*f[i]) continue;
        strncpy(d[i].dst, f[i], d[i].sz - 1);
        d[i].dst[d[i].sz - 1] = 0;
    }
    if (drv->host[0] && drv->state == DRV_EMPTY) {
        drv->state = DRV_ENABLED;
        drv->type = DRV_TYPE_TNFS;
        drv->letter = 'D';
        drv->port = 16384;
        if (!drv->mount_path[0]) strcpy(drv->mount_path, "/");
        snprintf(drv->nickname, sizeof drv->nickname, "TNFS");
    }
    g_set = g_stage;
    settings_to_runtime();
    printf("net: settings from Atari: ssid '%s', server %s, path %s\n",
           g_net.ssid, g_net.server, g_net.path);
    cfg_save();
}

/* ---------------- Wi-Fi ---------------- */

static bool wifi_connect(void)
{
    if (!g_net.ssid[0]) {
        snprintf(wifi_status, sizeof wifi_status, "no SSID set (CONFIG.TOS option 3)");
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
        printf("net: Wi-Fi try %d: %s\n", attempt, err);
        /* a wrong password will not get better by retrying */
        if (attempt >= 3 || !strcmp(err, "wrong password")) {
            snprintf(wifi_status, sizeof wifi_status, "'%s': %s", g_net.ssid, err);
            return false;
        }
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        sleep_ms(1000);
    }
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    snprintf(wifi_status, sizeof wifi_status, "connected, IP %s", ip4addr_ntoa(ip));
    printf("net: Wi-Fi %s\n", wifi_status);
    return true;
}

/* ---------------- TNFS over TCP (preferred) or UDP ---------------- */
/* Same messages on both transports; TCP has no extra framing. */

static struct udp_pcb *pcb;
static struct tcp_pcb *tpcb;
static volatile bool tcp_up, tcp_dead;
static bool use_tcp;
static ip_addr_t server_ip;
static uint8_t  rx[600];
static volatile int rx_len;
static uint16_t conn_id;
static uint8_t  seq;

static void udp_rx(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                   const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)upcb; (void)addr; (void)port;
    if (!rx_len) {
        int n = p->tot_len < sizeof rx ? p->tot_len : (int)sizeof rx;
        pbuf_copy_partial(p, rx, (u16_t)n, 0);
        rx_len = n;
    }
    pbuf_free(p);
}

static err_t tcp_rx(void *arg, struct tcp_pcb *t, struct pbuf *p, err_t err)
{
    (void)arg; (void)err;
    if (!p) { tcp_dead = true; return ERR_OK; }         /* closed by server */
    int room = (int)sizeof rx - rx_len;
    int n = p->tot_len < room ? p->tot_len : room;
    if (n > 0) pbuf_copy_partial(p, rx + rx_len, (u16_t)n, 0);
    rx_len += n;
    tcp_recved(t, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_connected(void *arg, struct tcp_pcb *t, err_t err)
{
    (void)arg; (void)t;
    tcp_up = err == ERR_OK;
    return ERR_OK;
}

static volatile int tcp_last_err;

static void tcp_error(void *arg, err_t err)
{
    (void)arg;
    tcp_last_err = err;
    tpcb = NULL;                                        /* freed by lwIP */
    tcp_dead = true;
}

static bool tcp_open(void)
{
    tcp_up = tcp_dead = false;
    tcp_last_err = 0;
    err_t ce = ERR_OK;
    cyw43_arch_lwip_begin();
    tpcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (tpcb) {
        tcp_recv(tpcb, tcp_rx);
        tcp_err(tpcb, tcp_error);
        ce = tcp_connect(tpcb, &server_ip, g_net.port, tcp_connected);
        if (ce != ERR_OK) {
            tcp_abort(tpcb);
            tpcb = NULL;
        }
    }
    cyw43_arch_lwip_end();
    /* the first SYN can be lost while ARP resolves the server; lwIP only
       retransmits after 3 s, so allow for that */
    absolute_time_t until = make_timeout_time_ms(10000);
    while (tpcb && !tcp_up && !tcp_dead && !time_reached(until)) sleep_ms(5);
    if (tcp_up) return true;
    printf("net: TCP connect to %s:%u failed (connect %d, error %d, %s)\n",
           ipaddr_ntoa(&server_ip), g_net.port, ce, tcp_last_err,
           tcp_dead ? "refused/reset" : "timeout");
    cyw43_arch_lwip_begin();
    if (tpcb) { tcp_abort(tpcb); tpcb = NULL; }
    cyw43_arch_lwip_end();
    return false;
}

static void tcp_shut(void)
{
    if (!wifi_inited || !tpcb) return;      /* lwIP not started yet / nothing open */
    cyw43_arch_lwip_begin();
    if (tpcb && tcp_close(tpcb) != ERR_OK) tcp_abort(tpcb);
    tpcb = NULL;
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

/* send one request and wait for the matching reply; returns reply length
   (>= 5) or -1. UDP retries with the same sequence number, as TNFS expects. */
static int tnfs_req(uint8_t cmd, const void *data, int dlen)
{
    static uint8_t tx[600];                 /* core0 only, not reentrant */
    if (dlen > (int)sizeof tx - 4) return -1;
    tx[0] = conn_id & 0xff;
    tx[1] = conn_id >> 8;
    tx[2] = ++seq;
    tx[3] = cmd;
    if (dlen) memcpy(tx + 4, data, (size_t)dlen);
    for (int attempt = 0; attempt < (use_tcp ? 1 : 4); attempt++) {
        rx_len = 0;
        cyw43_arch_lwip_begin();
        if (use_tcp) {
            if (tpcb) {
                tcp_write(tpcb, tx, (u16_t)(dlen + 4), TCP_WRITE_FLAG_COPY);
                tcp_output(tpcb);
            }
        } else {
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(dlen + 4), PBUF_RAM);
            if (p) {
                memcpy(p->payload, tx, (size_t)dlen + 4);
                udp_sendto(pcb, p, &server_ip, g_net.port);
                pbuf_free(p);
            }
        }
        cyw43_arch_lwip_end();
        absolute_time_t until = make_timeout_time_ms(use_tcp ? 3000 : 1000);
        while (!time_reached(until)) {
            if (rx_len >= 5 && rx[2] == tx[2] && rx[3] == cmd) {
                sleep_ms(use_tcp ? 5 : 0);          /* rest of a split TCP segment */
                return rx_len;
            }
            if (!use_tcp && rx_len) rx_len = 0;     /* stale UDP reply */
            if (use_tcp && tcp_dead) return -1;
            sleep_ms(2);
        }
    }
    return -1;
}

/* ---------------- server address: IP or host name (DNS) ---------------- */

static volatile int dns_state;      /* 0 busy, 1 found, -1 not found */

static void dns_found_cb(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name; (void)arg;
    if (ip) { server_ip = *ip; dns_state = 1; }
    else dns_state = -1;
}

static bool resolve_server(void)
{
    if (ipaddr_aton(g_net.server, &server_ip)) return true;
    snprintf(tnfs_status, sizeof tnfs_status, "looking up '%s'...", g_net.server);
    dns_state = 0;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(g_net.server, &server_ip, dns_found_cb, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) return true;                       /* cached */
    if (e == ERR_INPROGRESS) {
        absolute_time_t until = make_timeout_time_ms(5000);
        while (!dns_state && !time_reached(until)) sleep_ms(10);
        if (dns_state == 1) return true;
    }
    snprintf(tnfs_status, sizeof tnfs_status, "host name '%s' not found (DNS)", g_net.server);
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

static bool     mounted;
static unsigned srv_vmaj, srv_vmin;

/* MOUNT g_net.path: UDP first (the usual TNFS transport, e.g. a public
   server), then TCP (e.g. a server reached through a tunnel that only
   passes TCP). */
static bool tnfs_mount(void)
{
    if (mounted) return true;
    if (!resolve_server()) return false;
    uint8_t m[128];
    int ml = 0;
    m[ml++] = 0x02;                             /* version 1.2: minor, major */
    m[ml++] = 0x01;
    size_t pl = strlen(g_net.path);
    memcpy(m + ml, g_net.path, pl + 1);
    ml += (int)pl + 1;
    m[ml++] = 0;                                /* user     */
    m[ml++] = 0;                                /* password */
    int n = -1;
    use_tcp = false;
    if (udp_open()) {
        snprintf(tnfs_status, sizeof tnfs_status, "mounting (UDP)...");
        conn_id = 0;
        n = tnfs_req(0x00, m, ml);
    }
    if (n < 0) {
        snprintf(tnfs_status, sizeof tnfs_status, "no UDP answer, trying TCP...");
        use_tcp = tcp_open();
        if (use_tcp) {
            conn_id = 0;
            n = tnfs_req(0x00, m, ml);
        }
    }
    if (n < 0) {
        snprintf(tnfs_status, sizeof tnfs_status, "no answer from %s:%u (UDP nor TCP)",
                 ipaddr_ntoa(&server_ip), g_net.port);
        tcp_shut();
        return false;
    }
    if (rx[4] != 0) {
        snprintf(tnfs_status, sizeof tnfs_status, "MOUNT %s refused: %s (0x%02x)",
                 g_net.path, tnfs_err(rx[4]), rx[4]);
        tcp_shut();
        return false;
    }
    conn_id = (uint16_t)(rx[0] | (rx[1] << 8));
    srv_vmin = n > 5 ? rx[5] : 0;
    srv_vmaj = n > 6 ? rx[6] : 0;
    mounted = true;
    return true;
}

static void tnfs_unmount(void)
{
    if (mounted) tnfs_req(0x01, NULL, 0);
    mounted = false;
    tcp_shut();
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---------------- virtual FAT16 over TNFS (read-only) ----------------
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
 * therefore never changes while GEMDOS has it cached.
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
    uint16_t parent;
    uint16_t child_start, child_count;
    uint16_t first_cl, ncl;
    uint8_t  isdir;
    char     n83[11];
    uint32_t size, mtime;
    uint32_t name_off;
} vnode_t;

static vnode_t  vn[VNODES];
static uint16_t vn_count;
static char     vpool[32768];
static uint32_t vpool_len;
static uint16_t valloc[VNODES];     /* nodes that own clusters, in cluster order */
static uint16_t valloc_n;
static uint32_t vfiles, vdirs, vskipped;
static volatile bool vready;        /* tree scanned, node table stable */

static char *vname(uint16_t i) { return vpool + vn[i].name_off; }

/* server path of node i, relative to the mount point ("/" for the root) */
static int vpath(uint16_t i, char *buf, int max)
{
    if (i == 0) { snprintf(buf, (size_t)max, "/"); return 1; }
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

/* STAT: fills size, mtime, isdir */
static bool tnfs_stat(const char *path, uint32_t *size, uint32_t *mtime, bool *isdir)
{
    int n = tnfs_req(0x24, path, (int)strlen(path) + 1);
    if (n < 5 + 22 || rx[4] != 0) return false;
    uint16_t mode = le16(rx + 5);
    *isdir = (mode & 0170000) == 0040000;
    *size  = le32(rx + 11);
    *mtime = le32(rx + 19);
    return true;
}

static void vscan_dir(uint16_t d)
{
    static char path[256], child[320];      /* not reentrant: keep off the stack */
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
        if (vn_count >= VNODES || (d == 0 && vn[d].child_count >= V_ROOTENT - 1) ||
            vpool_len + strlen(name) + 1 > sizeof vpool) { vskipped++; continue; }
        snprintf(child, sizeof child, "%s%s%s", path, strcmp(path, "/") ? "/" : "", name);
        uint32_t size = 0, mtime = 0;
        bool isdir = false;
        if (!tnfs_stat(child, &size, &mtime, &isdir)) { vskipped++; continue; }
        uint16_t i = vn_count++;
        memset(&vn[i], 0, sizeof vn[i]);
        vn[i].parent = d;
        vn[i].isdir = isdir;
        vn[i].size = isdir ? 0 : size;
        vn[i].mtime = mtime;
        vn[i].name_off = vpool_len;
        strcpy(vpool + vpool_len, name);
        vpool_len += (uint32_t)strlen(name) + 1;
        make83(d, name, vn[i].n83);
        vn[d].child_count++;
        if (isdir) vdirs++; else vfiles++;
    }
    tnfs_req(0x12, &h, 1);                                  /* CLOSEDIR */
}

static void vfat_scan(void)
{
    vready = false;
    vn_count = 1;
    vpool_len = 0;
    vfiles = vdirs = vskipped = 0;
    memset(&vn[0], 0, sizeof vn[0]);
    vn[0].isdir = 1;
    vpool[0] = 0;
    vpool_len = 1;

    /* breadth first: children of a directory are contiguous in vn[] */
    for (uint16_t i = 0; i < vn_count; i++) {
        if (!vn[i].isdir) continue;
        snprintf(tnfs_status, sizeof tnfs_status, "scanning... %lu files, %lu dirs",
                 (unsigned long)vfiles, (unsigned long)vdirs);
        vscan_dir(i);
    }

    /* fixed cluster runs, in node order */
    uint32_t next = 2;
    valloc_n = 0;
    for (uint16_t i = 1; i < vn_count; i++) {
        uint32_t bytes = vn[i].isdir ? (uint32_t)(vn[i].child_count + 2) * 32 : vn[i].size;
        uint32_t ncl = (bytes + V_CLBYTES - 1) / V_CLBYTES;
        if (next + ncl > V_NCL + 2) {                       /* does not fit: hide */
            ncl = 0;
            vn[i].size = 0;
            vskipped++;
        }
        vn[i].first_cl = ncl ? (uint16_t)next : 0;
        vn[i].ncl = (uint16_t)ncl;
        if (ncl) valloc[valloc_n++] = i;
        next += ncl;
    }
    __dmb();
    vready = true;
}

/* node owning cluster c, or -1 */
static int vfind(uint32_t c)
{
    int lo = 0, hi = (int)valloc_n - 1;
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
    if (d != 0) {
        if (k == 0) { vdirent(e, ".          ", 0x10, vn[d].mtime, vn[d].first_cl, 0); return; }
        if (k == 1) {
            uint16_t p = vn[d].parent;
            vdirent(e, "..         ", 0x10, vn[d].mtime, p ? vn[p].first_cl : 0, 0);
            return;
        }
        k -= 2;
    }
    if (k >= vn[d].child_count) return;
    const vnode_t *v = &vn[vn[d].child_start + k];
    /* files read-only: this first version cannot write to the server */
    vdirent(e, v->n83, v->isdir ? 0x10 : 0x01, v->mtime, v->first_cl, v->size);
}

/* open file handle cache */
static int      vf_node = -1;
static uint8_t  vf_handle;
static uint32_t vf_pos;

static void vfile_close(void)
{
    if (vf_node >= 0 && mounted) tnfs_req(0x23, &vf_handle, 1);        /* CLOSE */
    vf_node = -1;
}

static bool vfile_read(uint16_t node, uint32_t off, uint8_t *dst, uint32_t len)
{
    if (vf_node != node) {
        vfile_close();
        uint8_t req[300];
        req[0] = 0x01; req[1] = 0x00;                      /* O_RDONLY */
        req[2] = 0x00; req[3] = 0x00;                      /* mode     */
        int pl = vpath(node, (char *)req + 4, (int)sizeof req - 4);
        int n = tnfs_req(0x29, req, 4 + pl + 1);           /* OPEN     */
        if (n < 6 || rx[4] != 0) return false;
        vf_handle = rx[5];
        vf_node = node;
        vf_pos = 0;
    }
    if (off != vf_pos) {
        uint8_t req[6] = { vf_handle, 0 };                 /* SEEK_SET */
        put32(req + 2, off);
        int n = tnfs_req(0x25, req, 6);                    /* LSEEK    */
        if (n < 5 || rx[4] != 0) { vfile_close(); return false; }
        vf_pos = off;
    }
    while (len) {
        uint16_t want = len > 512 ? 512 : (uint16_t)len;
        uint8_t req[3] = { vf_handle, (uint8_t)(want & 0xff), (uint8_t)(want >> 8) };
        int n = tnfs_req(0x21, req, 3);                    /* READ     */
        if (n < 7 || rx[4] != 0) break;                    /* EOF/error: zeros */
        uint16_t got = le16(rx + 5);
        if (got > n - 7) got = (uint16_t)(n - 7);
        if (got > want) got = want;
        memcpy(dst, rx + 7, got);
        dst += got; len -= got; vf_pos += got;
        if (!got) break;
    }
    return true;
}

/* one sector of the virtual partition */
static bool vfat_sector(uint32_t rel, uint8_t *buf)
{
    memset(buf, 0, 512);
    if (rel == 0) { vfat_bootsector(buf); return true; }
    if (rel < 1 + 2 * V_SPF) {                              /* FAT 1 and FAT 2 */
        uint32_t c0 = ((rel - 1) % V_SPF) * 256;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = c0 + i;
            uint16_t v = 0;
            if (c == 0) v = 0xfff8;
            else if (c == 1) v = 0xffff;
            else if (vready) {
                int nd = vfind(c);
                if (nd >= 0) v = c + 1 < (uint32_t)vn[nd].first_cl + vn[nd].ncl ? (uint16_t)(c + 1) : 0xffff;
            }
            put16(buf + i * 2, v);
        }
        return true;
    }
    if (rel < V_DATREC) {                                   /* root directory */
        if (!vready) return true;
        uint32_t k0 = (rel - (1 + 2 * V_SPF)) * 16;
        for (uint32_t k = 0; k < 16; k++) vdir_entry(0, k0 + k, buf + k * 32);
        return true;
    }
    if (!vready) return true;
    uint32_t c = 2 + (rel - V_DATREC) / V_SPC;
    uint32_t s = (rel - V_DATREC) % V_SPC;
    int nd = vfind(c);
    if (nd < 0) return true;
    uint32_t off = ((c - vn[nd].first_cl) * V_SPC + s) * 512;
    if (vn[nd].isdir) {
        for (uint32_t k = 0; k < 16; k++) vdir_entry((uint16_t)nd, off / 32 + k, buf + k * 32);
        return true;
    }
    if (off >= vn[nd].size) return true;
    uint32_t len = vn[nd].size - off;
    return vfile_read((uint16_t)nd, off, buf, len > 512 ? 512 : len);
}

/* connect, mount and scan: triggered at start-up and by CONFIG.TOS option 4 */
static void tnfs_connect_and_scan(void)
{
    vready = false;
    vfile_close();
    tnfs_unmount();
    tnfs_dir[0] = 0;
    if (!wifi_connect()) return;
    if (!tnfs_mount()) { printf("net: TNFS %s\n", tnfs_status); return; }
    vfat_scan();
    for (uint16_t i = 1; i < vn_count; i++) {
        char p[256];
        vpath(i, p, sizeof p);
        printf("  %.8s.%.3s %s %8lu  cl %5u+%-4u %s\n", vn[i].n83, vn[i].n83 + 8,
               vn[i].isdir ? "<DIR>" : "     ", (unsigned long)vn[i].size,
               vn[i].first_cl, vn[i].ncl, p);
    }
    int len = 0;
    for (uint16_t k = 0; k < vn[0].child_count && len < (int)sizeof tnfs_dir - 40; k++) {
        const vnode_t *v = &vn[vn[0].child_start + k];
        len += snprintf(tnfs_dir + len, sizeof tnfs_dir - (size_t)len, "%s%s%s",
                        len ? ", " : "  D:\\ ", vpool + v->name_off, v->isdir ? "\\" : "");
    }
    if (vn[0].child_count && len < (int)sizeof tnfs_dir - 3) strcpy(tnfs_dir + len, "\r\n");
    snprintf(tnfs_status, sizeof tnfs_status,
             "OK! %s, TNFS %u.%u: %lu files, %lu dirs%s",
             use_tcp ? "TCP" : "UDP", srv_vmaj, srv_vmin,
             (unsigned long)vfiles, (unsigned long)vdirs, vskipped ? " (some skipped)" : "");
    printf("net: TNFS %s\n", tnfs_status);
}

void net_init(void)
{
    if (g_net.ssid[0]) {
        snprintf(wifi_status, sizeof wifi_status, "idle (not connected yet)");
        test_req = true;                            /* auto-connect at start-up */
    }
}

void net_poll(void)
{
    if (cfg_new) {
        cfg_new = false;
        apply_cfg_block();
        snprintf(wifi_status, sizeof wifi_status, "settings changed, not connected yet");
        vready = false;
        vfile_close();
        tnfs_unmount();
        if (wifi_inited) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    }
    if (test_req) {
        test_req = false;
        tnfs_connect_and_scan();
    }
    if (vreq_state == 1) {
        bool ok = true;
        for (uint32_t i = 0; i < vreq_n && ok; i++)
            ok = vfat_sector(vreq_rel + i, vreq_buf + i * 512);
        __dmb();
        vreq_state = ok ? 2 : 3;
    }
}

#endif
