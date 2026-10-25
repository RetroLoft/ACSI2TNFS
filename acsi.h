#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

/* ---------------------------------------------------------------------------
   Pin map of v2 and v2-cable (hardware/README.md)
--------------------------------------------------------------------------- */
#define PIN_LED_DISK 0  /* D1: flashes on disk activity (READ/WRITE)          */
#define PIN_DBUS    6   /* D7..D0 = GP6..GP13 (reversed); first pin of a PIO sample */
#define PIN_DIR     14  /* U1 DIR, 1 = Atari -> Pico, 0 = Pico -> Atari       */
#define PIN_OE      15  /* U1 /OE, low = databuffer enabled                   */
#define PIN_CS      16
#define PIN_RST     17
#define PIN_ACK     18
#define PIN_A1      19
#define PIN_RW      20
#define PIN_IRQ     21  /* through the 74LS07, low = /IRQ asserted. The GPIO  */
#define PIN_DRQ     22  /* output is inverted: 1 = asserted for code and PIO  */
#define PIN_BUTTON  26  /* button to GND (hidden mode)                        */
#define PIN_LED_STATUS 28 /* D2: on, blinking while hidden                     */
#ifdef CYW43_WL_GPIO_LED_PIN
#define BOARD_HAS_WIFI 1
#else
#define BOARD_HAS_WIFI 0
#endif

/* time of the last disk access (core1), for the disk led (core0) */
extern volatile uint32_t g_disk_led_t;

static inline uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xcc) >> 2) | ((b & 0x33) << 2));
    return (uint8_t)(((b & 0xaa) >> 1) | ((b & 0x55) << 1));
}

/* a raw PIO sample (pins from GP6: D7..D0, DIR, /OE, /CS, ...) as 13 bits:
   D0..D7, /CS, /RESET, /ACK, A1, R/W */
static inline uint32_t smp_norm(uint32_t s)
{
    return rev8((uint8_t)s) | ((s >> 2) & 0x1f00);
}

/* bits in a normalised sample */
#define S_DATA(s)   ((uint8_t)((s) & 0xff))
#define S_CS        (1u << 8)
#define S_RST       (1u << 9)
#define S_ACK       (1u << 10)
#define S_A1        (1u << 11)
#define S_RW        (1u << 12)

#define ACSI_PIO    pio0
#define SM_CS       0
#define SM_ACK      1
#define SM_DOUT     2
#define SM_DIN      3

/* ---------------------------------------------------------------------------
   Flash layout (Pico: 2 MB)
     0x000000 .. 0x0FFFFF  firmware (incl. the built-in disk image)
     0x100000 .. 0x1FEFFF  disk image (512-byte sectors)
     0x1FF000 .. 0x1FFFFF  settings
--------------------------------------------------------------------------- */
#define DISK_FLASH_OFFSET   0x100000u
#define DISK_SECTORS        2040u   /* in flash; the C: partition is larger,
                                       the rest is marked bad (atari/mkdisk.py) */
#define CFG_FLASH_OFFSET    0x1FF000u

/* Virtual FAT16 partition backed by the TNFS server (Wi-Fi boards only).
   16 MB: TOS 1.x Rwabs sector numbers are signed 16-bit (512-byte sectors).
   Also the disk size without TNFS drives: C: ends before it. */
#define VFAT_START          8192u
#define VFAT_SECTORS        32760u
/* One partition per TNFS drive, every VFAT_STRIDE sectors from VFAT_START.
   The root sector has 4 entries: C: plus up to 3 TNFS drives. */
#define VFAT_STRIDE         32768u
#define VDRIVES_MAX         3

/* Root sector: byte holding the ACSI id (patched by the firmware at runtime) */
#define ROOT_ID_OFFSET      2

/* FW_VERSION_STR comes from version.txt (set by CMakeLists.txt) */
#ifndef FW_VERSION_STR
#error "FW_VERSION_STR not set: build with CMake (version.txt)"
#endif

enum { MODE_SNIFF = 0, MODE_TARGET = 1 };

#define ACSI_DEFAULT_ID     6           /* id of a new adapter, see acsi2tnfs.c */

typedef struct {
    volatile uint8_t mode;       /* requested mode                        */
    volatile uint8_t acsi_id;    /* our ACSI controller number (0..7)     */
    volatile bool    verbose;    /* log every command in target mode      */
    volatile bool    save_req;   /* core1 asks core0 to store settings    */
    volatile bool    hidden;     /* act as if unplugged: answer only our own
                                    vendor command ($11 'AT'), nothing else */
    volatile bool    mute_until_reset; /* C: was rewritten under a running Atari:
                                    answer only our vendor command until the
                                    driver has seen the media change (sub 10)
                                    or the Atari resets, so GEMDOS cannot
                                    write with its stale FAT */
    volatile bool    removable;  /* console M: report a removable unit, so
                                    HDDRIVER asks TEST UNIT READY and sees
                                    UNIT ATTENTION after a drive started
                                    afresh. Not with AHDI: it then logs the
                                    drives in again and again (whole FAT) */
} acsi_cfg_t;

extern acsi_cfg_t g_cfg;

/* ---------------------------------------------------------------------------
   Event ring core1 -> core0
--------------------------------------------------------------------------- */
enum {
    EV_CS = 0,      /* sample = /CS sample                                   */
    EV_ACK,         /* sample = /ACK sample                                  */
    EV_RESET,       /* sample = 0 reset asserted, 1 released                 */
    EV_CMD,         /* target: finished command, see cmd_log                 */
    EV_DROP,        /* sample = number of dropped events                     */
    EV_MODE,        /* sample = new mode                                     */
};

typedef struct {
    uint32_t t_us;
    uint16_t sample;
    uint8_t  type;
    uint8_t  idx;   /* EV_CMD: index into cmd_log                            */
} acsi_event_t;

typedef struct {
    uint8_t  cdb[12];
    uint8_t  cdb_len;
    uint8_t  status;
    uint8_t  sense;
    uint8_t  result;    /* 0 ok, 1 timeout, 2 reset, 3 host protocol error */
    uint32_t bytes;     /* data bytes moved                                */
    uint32_t acks;      /* /ACK pulses counted during data phase           */
    uint32_t us;        /* duration                                        */
    uint16_t cap_n;     /* /ACK samples captured (first 1024)              */
    uint16_t mism;      /* first byte where ACK data != sent data, or 0xffff */
    uint8_t  got[8], exp[8];
} cmd_log_t;

#define CMD_LOG_N 32
extern cmd_log_t g_cmd_log[CMD_LOG_N];

void ev_push(uint8_t type, uint16_t sample, uint8_t idx);
bool ev_pop(acsi_event_t *e);

/* statistics */
typedef struct {
    volatile uint32_t commands;
    volatile uint32_t sectors_read;
    volatile uint32_t sectors_written;
    volatile uint32_t errors;
    volatile uint32_t resets;
    volatile uint32_t ack_mismatch;
} acsi_stats_t;
extern acsi_stats_t g_stats;

void acsi_hw_init(void);

/* network (net.c) - only functional on boards with Wi-Fi */
void net_init(void);                                   /* core0, at start      */
void net_poll(void);                                   /* core0, main loop     */
void net_request_test(void);                           /* console N: reconnect */
uint32_t net_status_text(char *p, uint32_t max);       /* console n            */
uint32_t net_vdrives(void);                            /* TNFS partitions (fixed after start-up) */
uint8_t net_vdrive_letter(uint32_t k);                 /* wanted letter of partition k, 0 = any */
void net_clock(uint8_t *out);                          /* core1: vendor sub 9 (network time) */
uint32_t net_clock_sync_state(void);                   /* 0 off, 1 synchronised, 2 not yet */
void net_clock_keep(void);                             /* before a watchdog reboot */
void net_clock_toggle(void);                           /* console K */

/* media change: one counter per root sector partition (0 = C:, 1-3 = the
   TNFS drives), bumped when the Pico changed that partition under the Atari;
   the driver reads them with vendor sub 10 from hdv_mediach */
extern volatile uint8_t g_part_gen[4];
/* our driver asked for them since the last Atari reset: it reports media
   changes to GEMDOS (AHDI, HDDRIVER and EmuTOS do not) */
extern volatile bool g_mc_driver;
/* removable: TEST UNIT READYs since the Atari reset, and the next one
   answers UNIT ATTENTION after a drive started afresh. A driver that asks it
   again and again (HDDRIVER, every few seconds) reports that as a media
   change; EmuTOS asks it once at boot and never hears of it */
extern volatile uint8_t g_tur_count;
extern volatile bool g_unit_attention;
#define TUR_DRIVER (g_tur_count >= 3)
void disk_changed(int part);                /* bump + keep GEMDOS off until seen */

/* system files on C: (sysfiles.c): number written, -1 on error */
int sysfiles_sync(const uint8_t *image, bool restore_missing);
/* core0 changed the flash disk: core1 must re-read its write block */
void acsi_stage_invalidate(void);

/* ACSI id change from the configuration program (acsi_core1.c) */
void acsi_set_next_id(int id);                         /* from the next reset on */
int acsi_next_id(void);                                /* id stored in flash */
bool net_vread(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf); /* core1: virtual part. */
void net_atari_reset(void);                            /* core1: TNFS trees from the root again */
bool net_vwrite(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf);
void vfat_bootsector(uint8_t *b);

/* SideTNFS configuration protocol over ACSI (config.c) */
void cfgrpc_request(const uint8_t *blk512);   /* core1: vendor sub 5 */
void cfgrpc_response(uint8_t *out512);        /* core1: vendor sub 6 */
void cfgrpc_poll(void);                       /* core0: main loop    */
void net_console_status(void);                         /* core0: 'n' command   */

/* ACSI_NET, STinG network over ACSI (netbridge.c, vendor subs 0x20-0x2f) */
void net_bridge_publish(void);                /* core0: Wi-Fi state for core1 */
void net_info(uint8_t *out512);               /* core1: NET_INFO              */
uint32_t net_ctrl(const uint8_t *in512);      /* core1: NET_CTRL, 0 = stored  */
void net_bridge_reset(void);                  /* core1: Atari reset           */
void net_test_pattern(uint8_t *buf, uint32_t n);  /* core1: NET_TEST          */
uint8_t *net_rx_next(uint8_t *empty512, uint32_t max_sectors, uint32_t *sectors); /* NET_RX */
void net_rx_sent(uint8_t *buf);
uint8_t *net_tx_slot(void);                   /* core1: NET_TX, NULL = full   */
uint32_t net_tx_commit(uint8_t *slot, uint32_t sectors);
void net_bridge_console(void);                /* core0: 'w' command           */
void net_bridge_trace(void);                  /* core0: 'W' frame timeline    */
void core1_main(void);
void sd_console_test(void);                   /* core0: console 'c' (sdcard.c) */
void cfg_save(void);
const char *scsi_opname(uint8_t op);
