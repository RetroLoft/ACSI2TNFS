#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

/* ---------------------------------------------------------------------------
   Pin map: verified PCB nets (ACSI2TNFS_hardwareoverzicht.md, section 2)
--------------------------------------------------------------------------- */
#define PIN_OE      6   /* U1 /OE, low = databuffer enabled                  */
#define PIN_DIR     7   /* U1 DIR, 1 = Atari -> Pico, 0 = Pico -> Atari      */
#define PIN_D0      8   /* D0..D7 = GP8..GP15                                */
#define PIN_CS      16
#define PIN_RST     17
#define PIN_ACK     18
#define PIN_A1      19
#define PIN_RW      20
#define PIN_IRQ     21  /* high = /IRQ asserted (Q1)                          */
#define PIN_DRQ     22  /* high = /DRQ asserted (Q2)                          */
#define PIN_LED     28  /* PCB status led                                     */
#ifdef CYW43_WL_GPIO_LED_PIN
/* Pico W / Pico 2 W: the onboard led hangs on the Wi-Fi chip and GP25 is its
   SPI chip select, so never touch GP25 there; use the PCB status led. */
#define PIN_LED_PICO PIN_LED
#define BOARD_HAS_WIFI 1
#else
#define PIN_LED_PICO 25 /* onboard led of the Pico (non-W)                    */
#define BOARD_HAS_WIFI 0
#endif

/* bits in a 13-bit PIO sample (GP8..GP20) */
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
#define DISK_SECTORS        2040u
#define CFG_FLASH_OFFSET    0x1FF000u

/* Virtual FAT16 partition backed by the TNFS server (Wi-Fi boards only).
   16 MB: TOS 1.x Rwabs sector numbers are signed 16-bit (512-byte sectors). */
#define VFAT_START          4096u
#define VFAT_SECTORS        32760u
/* One partition per TNFS drive, every VFAT_STRIDE sectors from VFAT_START.
   The root sector has 4 entries: C: plus up to 3 TNFS drives. */
#define VFAT_STRIDE         32768u
#define VDRIVES_MAX         3

/* Root sector: byte holding the ACSI id (patched by the firmware at runtime) */
#define ROOT_ID_OFFSET      2

#define FW_VERSION_STR      "0.4"

enum { MODE_SNIFF = 0, MODE_TARGET = 1 };

typedef struct {
    volatile uint8_t mode;       /* requested mode                        */
    volatile uint8_t acsi_id;    /* our ACSI controller number (0..7)     */
    volatile bool    verbose;    /* log every command in target mode      */
    volatile bool    led_test;   /* blink request from CONFIG.TOS         */
    volatile bool    save_req;   /* core1 asks core0 to store settings    */
    volatile bool    hidden;     /* act as if unplugged: answer only our own
                                    vendor command ($11 'AT'), nothing else */
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
void net_settings_from_atari(const uint8_t *blk512);   /* core1: vendor sub 3  */
void net_request_test(void);                           /* core1: vendor sub 4  */
uint32_t net_status_text(char *p, uint32_t max);       /* core1: info text     */
uint32_t net_vdrives(void);                            /* TNFS partitions (fixed after start-up) */
bool net_vread(uint32_t drive, uint32_t rel, uint32_t n, uint8_t *buf); /* core1: virtual part. */
void vfat_bootsector(uint8_t *b);

/* SideTNFS configuration protocol over ACSI (config.c) */
void cfgrpc_request(const uint8_t *blk512);   /* core1: vendor sub 5 */
void cfgrpc_response(uint8_t *out512);        /* core1: vendor sub 6 */
void cfgrpc_poll(void);                       /* core0: main loop    */
void cfgrpc_debug(const uint8_t *blk512);     /* core1: vendor sub 7 */
void net_console_status(void);                         /* core0: 'n' command   */
void core1_main(void);
void cfg_save(void);
const char *scsi_opname(uint8_t op);
