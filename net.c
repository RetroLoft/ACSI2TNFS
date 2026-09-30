/*
 * ACSI2TNFS - core0: Wi-Fi and a minimal TNFS client (connection test)
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
#include "acsi.h"

typedef struct {
    uint32_t magic;
    char     ssid[33];
    char     pass[65];
    char     server[65];
    char     path[97];
    uint16_t port;
} net_settings_t;

#define NET_MAGIC 0x544e4653u   /* "TNFS" */

static net_settings_t g_net = {
    NET_MAGIC, "", "", "192.168.178.10", "/", 16384
};

/* status shown to the Atari (written by core0, read by core1) */
static char wifi_status[80] = "not configured";
static char tnfs_status[96] = "-";
static char tnfs_dir[240]   = "";

static volatile bool test_req;
static volatile bool cfg_new;
static uint8_t cfg_blk[512];

extern acsi_cfg_t g_cfg;

const void *net_settings_blob(uint32_t *len)
{
    *len = sizeof g_net;
    return &g_net;
}

void net_settings_load(const void *blob)
{
    const net_settings_t *s = blob;
    if (s->magic == NET_MAGIC) g_net = *s;
    g_net.ssid[32] = g_net.pass[64] = g_net.server[64] = g_net.path[96] = 0;
    if (!g_net.port) g_net.port = 16384;
}

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

#if !BOARD_HAS_WIFI

void net_init(void) { }
void net_poll(void) { cfg_new = false; test_req = false; }

#else

#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

static bool wifi_inited;

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
    struct { char *dst; size_t sz; } d[4] = {
        { g_net.ssid, sizeof g_net.ssid }, { g_net.pass, sizeof g_net.pass },
        { g_net.server, sizeof g_net.server }, { g_net.path, sizeof g_net.path } };
    for (int i = 0; i < 4; i++) {
        if (f[i] >= end || !*f[i]) continue;
        strncpy(d[i].dst, f[i], d[i].sz - 1);
        d[i].dst[d[i].sz - 1] = 0;
    }
    g_net.magic = NET_MAGIC;
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
        if (cyw43_arch_init_with_country(CYW43_COUNTRY('N', 'L', 0))) {
            snprintf(wifi_status, sizeof wifi_status, "Wi-Fi chip init FAILED");
            return false;
        }
        cyw43_arch_enable_sta_mode();
        wifi_inited = true;
    }
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP)
        return true;

    uint32_t auth = g_net.pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
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
    uint8_t tx[600];
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

static void tnfs_test(void)
{
    tnfs_dir[0] = 0;
    if (!resolve_server()) {
        printf("net: TNFS %s\n", tnfs_status);
        return;
    }
    /* MOUNT: version 1.2 (minor, major), path, user, password */
    uint8_t m[128];
    int ml = 0;
    m[ml++] = 0x02;
    m[ml++] = 0x01;
    size_t pl = strlen(g_net.path);
    memcpy(m + ml, g_net.path, pl + 1);
    ml += (int)pl + 1;
    m[ml++] = 0;            /* user     */
    m[ml++] = 0;            /* password */
    /* UDP first (the usual TNFS transport, e.g. a public server), then TCP
       (e.g. a server reached through a tunnel that only passes TCP) */
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
    const char *proto = use_tcp ? "TCP" : "UDP";
    if (n < 0) {
        snprintf(tnfs_status, sizeof tnfs_status, "no answer from %s:%u (UDP nor TCP)",
                 ipaddr_ntoa(&server_ip), g_net.port);
        printf("net: TNFS %s\n", tnfs_status);
        tcp_shut();
        return;
    }
    if (rx[4] != 0) {
        snprintf(tnfs_status, sizeof tnfs_status, "MOUNT refused: %s (0x%02x)",
                 tnfs_err(rx[4]), rx[4]);
        printf("net: TNFS %s\n", tnfs_status);
        tcp_shut();
        return;
    }
    conn_id = (uint16_t)(rx[0] | (rx[1] << 8));
    unsigned vmin = n > 5 ? rx[5] : 0, vmaj = n > 6 ? rx[6] : 0;

    /* OPENDIR "/" (relative to the mount point) and read a few names */
    int count = 0, len = 0;
    const char root[] = "/";
    n = tnfs_req(0x10, root, sizeof root);
    if (n >= 6 && rx[4] == 0) {
        uint8_t h = rx[5];
        for (int i = 0; i < 64; i++) {
            n = tnfs_req(0x11, &h, 1);
            if (n < 6 || rx[4] != 0) break;
            rx[n < (int)sizeof rx ? n : (int)sizeof rx - 1] = 0;
            const char *name = (const char *)rx + 5;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            count++;
            int w = snprintf(tnfs_dir + len, sizeof tnfs_dir - (size_t)len,
                             "%s%s", len ? ", " : "  ", name);
            if (w > 0 && len + w < (int)sizeof tnfs_dir - 8) len += w;
            else if (len < (int)sizeof tnfs_dir - 8) { strcpy(tnfs_dir + len, ", ..."); len += 5; }
        }
        tnfs_req(0x12, &h, 1);                  /* CLOSEDIR */
        if (len < (int)sizeof tnfs_dir - 3) strcpy(tnfs_dir + len, "\r\n");
    }
    tnfs_req(0x01, NULL, 0);                    /* UMOUNT */
    tcp_shut();
    snprintf(tnfs_status, sizeof tnfs_status,
             "OK! %s, TNFS %u.%u, %d entries in %s", proto, vmaj, vmin, count, g_net.path);
    printf("net: TNFS %s\n", tnfs_status);
}

void net_init(void)
{
    if (g_net.ssid[0]) snprintf(wifi_status, sizeof wifi_status, "idle (not connected yet)");
}

void net_poll(void)
{
    if (cfg_new) {
        cfg_new = false;
        apply_cfg_block();
        snprintf(wifi_status, sizeof wifi_status, "settings changed, not connected yet");
        if (wifi_inited) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    }
    if (test_req) {
        test_req = false;
        snprintf(tnfs_status, sizeof tnfs_status, "-");
        tnfs_dir[0] = 0;
        if (wifi_connect()) tnfs_test();
    }
}

#endif
