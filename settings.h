#pragma once
/*
 * Persistent settings, modelled on the SideTNFS configuration protocol v3
 * (github.com/RetroLoft/SideTNFS-Firmware, docs/sidetnfs-config-protocol.md)
 * so the SideTNFS-Config GEM application can edit them over ACSI.
 */
#include <stdint.h>

#define SET_MAGIC           0x41325433u     /* "A2T3" */
#define SET_MAX_DRIVES      8               /* the UI requires exactly 8 slots */

#define SET_NICKNAME_LEN    24
#define SET_HOST_LEN        64
#define SET_MOUNTPATH_LEN   32
#define SET_SDPATH_LEN      64
#define SET_SSID_LEN        36
#define SET_PASSWORD_LEN    68
#define SET_COUNTRY_LEN     4
#define SET_IPV4_LEN        16
#define SET_NTP_LEN         64
#define SET_UTCOFF_LEN      4

enum { DRV_EMPTY = 0, DRV_DISABLED = 1, DRV_ENABLED = 2 };
enum { DRV_TYPE_NONE = 0, DRV_TYPE_SD = 1, DRV_TYPE_TNFS = 2 };
enum { DRV_UDP = 0, DRV_TCP = 1 };

typedef struct {
    uint8_t  state, letter, type, transport;
    uint16_t port;
    char     nickname[SET_NICKNAME_LEN];
    char     host[SET_HOST_LEN];
    char     mount_path[SET_MOUNTPATH_LEN];
    char     sd_path[SET_SDPATH_LEN];
} set_drive_t;

typedef struct {
    uint32_t    magic;
    /* network */
    char        ssid[SET_SSID_LEN];
    char        pass[SET_PASSWORD_LEN];
    char        country[SET_COUNTRY_LEN];
    uint8_t     auth_mode;                  /* 0..8, SideTNFS mapping */
    uint8_t     use_dhcp;
    char        ip[SET_IPV4_LEN], netmask[SET_IPV4_LEN];
    char        gateway[SET_IPV4_LEN], dns[SET_IPV4_LEN];
    /* drives */
    uint8_t     settings_letter;            /* letter wish for the flash drive */
    set_drive_t drv[SET_MAX_DRIVES];
    /* clock */
    uint8_t     rtc_enabled;
    char        ntp_server[SET_NTP_LEN];
    char        utc_offset[SET_UTCOFF_LEN];
} settings_t;

extern settings_t g_set;    /* active: what the firmware runs with (loaded at boot) */
extern settings_t g_stage;  /* edited by the configuration program, persisted by SAVE */

void settings_defaults(settings_t *s);
