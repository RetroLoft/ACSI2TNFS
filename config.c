/*
 * ACSI2TNFS - SideTNFS configuration protocol (v3) over ACSI
 *
 * Lets the SideTNFS-Config GEM application (built with its ACSI transport)
 * edit the settings of this adapter. Command numbers, fields, lengths and
 * status codes follow SideTNFS-Firmware docs/sidetnfs-config-protocol.md;
 * only the transport differs:
 *
 *   vendor sub 5 (Atari -> Pico, 512 bytes):  [cmd u16][seq u16][request]
 *   vendor sub 6 (Pico -> Atari, 512 bytes):  [cmd u16][seq u16][response]
 *
 * All numbers are big-endian (native for the 68000), 32-bit unless noted;
 * strings are fixed-length, NUL padded. The Atari polls sub 6 until cmd and
 * seq match its request, so slow commands (flash writes) cannot hit an ACSI
 * timeout. Requests are handled on core0; core1 only copies buffers.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "acsi.h"
#include "settings.h"

#define CMD_GET_CONFIG_INFO     0x040d
#define CMD_GET_DRIVE           0x040e
#define CMD_SET_DRIVE           0x040f
#define CMD_DELETE_DRIVE        0x0410
#define CMD_SET_CONFIG_DRIVE    0x0411
#define CMD_SAVE_CONFIG         0x0412
#define CMD_GET_NETWORK_CONFIG  0x0413
#define CMD_SET_NETWORK_CONFIG  0x0414
#define CMD_SAVE_NETWORK_CONFIG 0x0415
#define CMD_GET_RTC_CONFIG      0x0416
#define CMD_SET_RTC_CONFIG      0x0417
#define CMD_SAVE_RTC_CONFIG     0x0418
#define CMD_REBOOT_PICO         0x041b
#define CMD_CHECK_UPDATE        0x041c
#define CMD_GET_STATUS          0x0480      /* ACSI2TNFS: live link / clock state */

enum {  /* sidetnfs_config_status_t */
    ST_OK = 0, ST_INVALID_INDEX, ST_EMPTY_SLOT, ST_INVALID_DRIVE_LETTER,
    ST_DUPLICATE_DRIVE_LETTER, ST_INVALID_TYPE, ST_INVALID_TRANSPORT,
    ST_INVALID_PORT, ST_INVALID_HOST, ST_INVALID_MOUNT_PATH, ST_INVALID_SD_PATH,
    ST_TOO_MANY_DRIVES, ST_FLASH_WRITE_FAILED, ST_CRC_MISMATCH,
    ST_UNSUPPORTED_VERSION, ST_INVALID_DRIVE_STATE
};
enum {  /* sidetnfs_netconfig_status_t */
    NS_OK = 0, NS_INVALID_SSID, NS_INVALID_PASSWORD, NS_INVALID_AUTH_MODE,
    NS_INVALID_COUNTRY, NS_INVALID_DHCP, NS_INVALID_IP, NS_INVALID_NETMASK,
    NS_INVALID_GATEWAY, NS_INVALID_DNS
};
enum {  /* sidetnfs_rtcconfig_status_t */
    RS_OK = 0, RS_INVALID_ENABLED, RS_INVALID_NTP_SERVER, RS_INVALID_UTC_OFFSET
};

bool net_link_up(void);

static uint8_t req[512], resp[512];
static volatile bool req_pending;
static bool reboot_after_reply;

/* ---------------- core1 side ---------------- */

void cfgrpc_request(const uint8_t *blk512)
{
    memcpy(req, blk512, sizeof req);
    __dmb();
    req_pending = true;
}

void cfgrpc_response(uint8_t *out512)
{
    memcpy(out512, resp, sizeof resp);
}

/* vendor sub 7: a crash dump (or any debug block) from the Atari */
static uint8_t dbg[512];
static volatile bool dbg_pending;

void cfgrpc_debug(const uint8_t *blk512)
{
    memcpy(dbg, blk512, sizeof dbg);
    __dmb();
    dbg_pending = true;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* layout from CONFIG.TOS: [0] $380..$3FF (TOS crash save area), [128] tbase of
   CONFIG.TOS itself (= where the crashed program was most likely loaded) */
static void print_debug(void)
{
    const uint8_t *a = dbg;
    printf("\n=== Atari crash dump (TOS $380 save area) ===\n");
    printf("valid     : %08lx (12345678 = TOS saved a crash)\n", (unsigned long)be32(a));
    for (int i = 0; i < 8; i++)
        printf("D%d %08lx  A%d %08lx\n", i, (unsigned long)be32(a + 4 + i * 4),
               i, (unsigned long)be32(a + 0x24 + i * 4));
    printf("exception : %lu (bombs)\n", (unsigned long)be32(a + 0x44) >> 24);
    printf("USP       : %08lx\n", (unsigned long)be32(a + 0x48));
    printf("stack     :");
    for (int i = 0; i < 16; i++) printf(" %02x%02x", a[0x4c + i * 2], a[0x4d + i * 2]);
    printf("\ntbase now : %08lx\n", (unsigned long)be32(a + 128));
    printf("=== end ===\n");
}

/* ---------------- encoding ---------------- */

static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
    return p + 4;
}
static uint8_t *putstr(uint8_t *p, const char *s, int len)
{
    memset(p, 0, (size_t)len);
    strncpy((char *)p, s, (size_t)len - 1);
    return p + len;
}
static const uint8_t *getstr(const uint8_t *p, char *dst, int len)
{
    memcpy(dst, p, (size_t)len);
    dst[len - 1] = 0;
    return p + len;
}

/* ---------------- validation ---------------- */

static bool valid_ipv4(const char *s)
{
    int parts = 0, val = -1;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = (val < 0 ? 0 : val * 10) + (*s - '0');
            if (val > 255) return false;
        } else if (*s == '.' || !*s) {
            if (val < 0) return false;
            parts++;
            val = -1;
            if (!*s) break;
        } else return false;
    }
    return parts == 4;
}

static bool letter_used(const settings_t *s, uint8_t letter, int skip)
{
    if (letter == s->settings_letter) return true;
    for (int i = 0; i < SET_MAX_DRIVES; i++)
        if (i != skip && s->drv[i].state != DRV_EMPTY && s->drv[i].letter == letter) return true;
    return false;
}

static uint32_t validate_drive(const set_drive_t *d, int index)
{
    if (d->state > DRV_ENABLED) return ST_INVALID_DRIVE_STATE;
    if (d->state == DRV_EMPTY) return ST_OK;
    if (d->letter < 'C' || d->letter > 'Z') return ST_INVALID_DRIVE_LETTER;
    if (letter_used(&g_stage, d->letter, index)) return ST_DUPLICATE_DRIVE_LETTER;
    if (d->type != DRV_TYPE_TNFS) return ST_INVALID_TYPE;        /* no SD card support (yet) */
    if (d->transport > DRV_TCP) return ST_INVALID_TRANSPORT;
    if (!d->port) return ST_INVALID_PORT;
    if (!d->host[0]) return ST_INVALID_HOST;
    if (d->mount_path[0] != '/') return ST_INVALID_MOUNT_PATH;
    return ST_OK;
}

static bool save_stage(void)
{
    cfg_save();
    return true;
}

/* ---------------- request handling (core0) ---------------- */

static void handle(void)
{
    const uint16_t cmd = (uint16_t)((req[0] << 8) | req[1]);
    const uint8_t *q = req + 4;
    uint8_t out[504];
    uint8_t *o = out;
    memset(out, 0, sizeof out);

    switch (cmd) {
    case CMD_GET_CONFIG_INFO: {
        uint32_t used = 0;
        for (int i = 0; i < SET_MAX_DRIVES; i++) if (g_stage.drv[i].state != DRV_EMPTY) used++;
        o = put32(o, 3);                                /* protocol version */
        o = put32(o, SET_MAX_DRIVES);
        o = put32(o, used);
        o = put32(o, g_stage.settings_letter);
        o = put32(o, ST_OK);
        break;
    }
    case CMD_GET_DRIVE: {
        uint32_t i = get32(q);
        if (i >= SET_MAX_DRIVES) { o = put32(o, ST_INVALID_INDEX); break; }
        const set_drive_t *d = &g_stage.drv[i];
        o = put32(o, ST_OK);
        o = put32(o, d->state);
        o = put32(o, d->letter);
        o = put32(o, d->type);
        o = put32(o, d->transport);
        o = put32(o, d->port);
        o = putstr(o, d->nickname, SET_NICKNAME_LEN);
        o = putstr(o, d->host, SET_HOST_LEN);
        o = putstr(o, d->mount_path, SET_MOUNTPATH_LEN);
        o = putstr(o, d->sd_path, SET_SDPATH_LEN);
        break;
    }
    case CMD_SET_DRIVE: {
        uint32_t i = get32(q);
        if (i >= SET_MAX_DRIVES) { o = put32(o, ST_INVALID_INDEX); break; }
        set_drive_t d;
        memset(&d, 0, sizeof d);
        uint32_t state = get32(q + 4), letter = get32(q + 8), type = get32(q + 12);
        uint32_t transport = get32(q + 16), port = get32(q + 20);
        const uint8_t *s = q + 24;
        s = getstr(s, d.nickname, SET_NICKNAME_LEN);
        s = getstr(s, d.host, SET_HOST_LEN);
        s = getstr(s, d.mount_path, SET_MOUNTPATH_LEN);
        getstr(s, d.sd_path, SET_SDPATH_LEN);
        if (state > DRV_ENABLED) { o = put32(o, ST_INVALID_DRIVE_STATE); break; }
        if (transport > 255 || type > 255) { o = put32(o, ST_INVALID_TYPE); break; }
        if (port > 65535) { o = put32(o, ST_INVALID_PORT); break; }
        d.state = (uint8_t)state;
        d.letter = (uint8_t)(letter >= 'a' && letter <= 'z' ? letter - 32 : letter);
        d.type = (uint8_t)type;
        d.transport = (uint8_t)transport;
        d.port = (uint16_t)port;
        uint32_t st = validate_drive(&d, (int)i);
        if (st == ST_OK) g_stage.drv[i] = d;
        o = put32(o, st);
        break;
    }
    case CMD_DELETE_DRIVE: {
        uint32_t i = get32(q);
        if (i >= SET_MAX_DRIVES) { o = put32(o, ST_INVALID_INDEX); break; }
        if (g_stage.drv[i].state == DRV_EMPTY) { o = put32(o, ST_EMPTY_SLOT); break; }
        memset(&g_stage.drv[i], 0, sizeof g_stage.drv[i]);
        o = put32(o, ST_OK);
        break;
    }
    case CMD_SET_CONFIG_DRIVE: {
        uint32_t l = get32(q);
        if (l >= 'a' && l <= 'z') l -= 32;
        if (l < 'C' || l > 'Z') { o = put32(o, ST_INVALID_DRIVE_LETTER); break; }
        bool dup = false;
        for (int i = 0; i < SET_MAX_DRIVES; i++)
            if (g_stage.drv[i].state != DRV_EMPTY && g_stage.drv[i].letter == l) dup = true;
        if (dup) { o = put32(o, ST_DUPLICATE_DRIVE_LETTER); break; }
        g_stage.settings_letter = (uint8_t)l;
        o = put32(o, ST_OK);
        break;
    }
    case CMD_SAVE_CONFIG:
    case CMD_SAVE_NETWORK_CONFIG:
    case CMD_SAVE_RTC_CONFIG:
        o = put32(o, save_stage() ? 0 : (cmd == CMD_SAVE_CONFIG ? ST_FLASH_WRITE_FAILED : 11));
        break;

    case CMD_GET_NETWORK_CONFIG:
        o = put32(o, NS_OK);
        o = put32(o, g_stage.auth_mode);
        o = put32(o, g_stage.use_dhcp);
        o = putstr(o, g_stage.ssid, SET_SSID_LEN);
        o = putstr(o, g_stage.pass, SET_PASSWORD_LEN);
        o = putstr(o, g_stage.country[0] ? g_stage.country : "XX", SET_COUNTRY_LEN);
        o = putstr(o, g_stage.ip, SET_IPV4_LEN);
        o = putstr(o, g_stage.netmask, SET_IPV4_LEN);
        o = putstr(o, g_stage.gateway, SET_IPV4_LEN);
        o = putstr(o, g_stage.dns, SET_IPV4_LEN);
        break;
    case CMD_SET_NETWORK_CONFIG: {
        settings_t n = g_stage;
        uint32_t auth = get32(q), dhcp = get32(q + 4);
        const uint8_t *s = q + 8;
        char ssid[SET_SSID_LEN], pass[SET_PASSWORD_LEN];
        s = getstr(s, ssid, SET_SSID_LEN);
        s = getstr(s, pass, SET_PASSWORD_LEN);
        s = getstr(s, n.country, SET_COUNTRY_LEN);
        s = getstr(s, n.ip, SET_IPV4_LEN);
        s = getstr(s, n.netmask, SET_IPV4_LEN);
        s = getstr(s, n.gateway, SET_IPV4_LEN);
        getstr(s, n.dns, SET_IPV4_LEN);
        uint32_t st = NS_OK;
        if (strlen(ssid) > 32) st = NS_INVALID_SSID;
        else if (strlen(pass) > 64) st = NS_INVALID_PASSWORD;
        else if (auth > 8) st = NS_INVALID_AUTH_MODE;
        else if (dhcp > 1) st = NS_INVALID_DHCP;
        else {
            for (int k = 0; k < 2; k++)
                if (n.country[k] >= 'a' && n.country[k] <= 'z') n.country[k] -= 32;
            if (strlen(n.country) != 2 || n.country[0] < 'A' || n.country[0] > 'Z' ||
                n.country[1] < 'A' || n.country[1] > 'Z') st = NS_INVALID_COUNTRY;
            else if (!dhcp && !valid_ipv4(n.ip)) st = NS_INVALID_IP;
            else if (!dhcp && !valid_ipv4(n.netmask)) st = NS_INVALID_NETMASK;
            else if (!dhcp && !valid_ipv4(n.gateway)) st = NS_INVALID_GATEWAY;
            else if (!dhcp && n.dns[0] && !valid_ipv4(n.dns)) st = NS_INVALID_DNS;
        }
        if (st == NS_OK) {
            strcpy(n.ssid, ssid);
            strcpy(n.pass, pass);
            n.auth_mode = (uint8_t)auth;
            n.use_dhcp = (uint8_t)dhcp;
            g_stage = n;
        }
        o = put32(o, st);
        break;
    }

    case CMD_GET_RTC_CONFIG:
        o = put32(o, RS_OK);
        o = put32(o, g_stage.rtc_enabled);
        o = putstr(o, g_stage.ntp_server, SET_NTP_LEN);
        o = putstr(o, g_stage.utc_offset, SET_UTCOFF_LEN);
        break;
    case CMD_SET_RTC_CONFIG: {
        uint32_t en = get32(q);
        char ntp[SET_NTP_LEN], off[SET_UTCOFF_LEN];
        const uint8_t *s = getstr(q + 4, ntp, SET_NTP_LEN);
        getstr(s, off, SET_UTCOFF_LEN);
        uint32_t st = RS_OK;
        if (en > 1) st = RS_INVALID_ENABLED;
        else if (en && !ntp[0]) st = RS_INVALID_NTP_SERVER;
        else if (off[0] != '+' && off[0] != '-' && (off[0] < '0' || off[0] > '9')) st = RS_INVALID_UTC_OFFSET;
        if (st == RS_OK) {
            g_stage.rtc_enabled = (uint8_t)en;
            strcpy(g_stage.ntp_server, ntp);
            strcpy(g_stage.utc_offset, off);
        }
        o = put32(o, st);
        break;
    }

    case CMD_CHECK_UPDATE:
        o = put32(o, 2);                                /* ERROR: not implemented yet */
        o = putstr(o, "", 16);
        o = putstr(o, "v" FW_VERSION_STR, 16);
        break;

    case CMD_REBOOT_PICO:
        reboot_after_reply = true;
        break;

    case CMD_GET_STATUS:
        o = put32(o, g_set.rtc_enabled ? 2 : 0);        /* clock: disabled / not synced */
        o = put32(o, net_link_up() ? 1 : 0);
        break;

    default:
        o = put32(o, 0xffffffffu);
        break;
    }
    (void)o;

    /* payload first, header (cmd + seq) last: core1 may copy at any time */
    memcpy(resp + 4, out, sizeof out);
    __dmb();
    resp[0] = req[0]; resp[1] = req[1]; resp[2] = req[2]; resp[3] = req[3];
}

void cfgrpc_poll(void)
{
    if (dbg_pending) {
        dbg_pending = false;
        print_debug();
    }
    if (!req_pending) return;
    req_pending = false;
    __dmb();
    handle();
    printf("config: cmd %04x seq %u -> %02x %02x %02x %02x %02x %02x %02x %02x\n",
           (req[0] << 8) | req[1], (req[2] << 8) | req[3],
           resp[4], resp[5], resp[6], resp[7], resp[8], resp[9], resp[10], resp[11]);
    if (reboot_after_reply) {
        reboot_after_reply = false;
        printf("config: reboot requested by the Atari\n");
        sleep_ms(500);                                  /* let the Atari read the ACK */
        watchdog_reboot(0, 0, 0);
    }
}
