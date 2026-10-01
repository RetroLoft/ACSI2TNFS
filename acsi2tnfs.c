/*
 * ACSI2TNFS - core0: USB console, sniffer decoder, settings
 *
 * USB serial commands:
 *   s  sniffer mode (passive)        t  target mode (flash hard disk)
 *   0..7 ACSI id                     v  verbose command log on/off
 *   i  info / statistics             B  reboot into BOOTSEL (flashing)
 *   h  help
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "acsi.h"
#include "disk_seed.h"

extern uint32_t la_buf[];
extern volatile bool la_armed, la_done;
#define LA_WORDS 8192

/* defaults for a new adapter (nothing in flash yet): hard disk on ACSI id 6.
   Not 0: an internal Mega ST disk, a Megafile or an UltraSatan usually sits
   there, and two devices on one id answer at the same time. */
acsi_cfg_t g_cfg = { .mode = MODE_TARGET, .acsi_id = ACSI_DEFAULT_ID, .verbose = true };

/* ---------------------------------------------------------------------------
   Settings in the last flash sector
--------------------------------------------------------------------------- */
#define CFG_MAGIC 0x41435349u   /* "ACSI" */
typedef struct { uint32_t magic; uint8_t mode, id, verbose, hidden; } cfg_flash_t;

/* settings sector layout: [0] cfg_flash_t, [256] network settings (net.c) */
#define CFG_NET_OFFSET 256
#define CFG_BYTES      4096   /* whole settings sector */
const void *net_settings_blob(uint32_t *len);
void net_settings_load(const void *blob);

static void cfg_load(void)
{
    const cfg_flash_t *f = (const cfg_flash_t *)(XIP_BASE + CFG_FLASH_OFFSET);
    net_settings_load((const uint8_t *)f + CFG_NET_OFFSET);   /* defaults when blank */
    if (f->magic != CFG_MAGIC) return;
    if (f->mode <= MODE_TARGET) g_cfg.mode = f->mode;
    if (f->id <= 7) g_cfg.acsi_id = f->id;
    g_cfg.verbose = f->verbose != 0;
    g_cfg.hidden = f->hidden == 1;      /* was padding (0) before */
}

static void cfg_write_cb(void *p)
{
    flash_range_erase(CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CFG_FLASH_OFFSET, (const uint8_t *)p, CFG_BYTES);
}

void cfg_save(void)
{
    static uint8_t page[CFG_BYTES];        /* RAM: XIP is off while programming */
    /* the id of the next boot: a change asked for by the configuration
       program is stored now but used only from the next reset */
    cfg_flash_t f = { CFG_MAGIC, g_cfg.mode, (uint8_t)acsi_next_id(), g_cfg.verbose, g_cfg.hidden };
    uint32_t nlen;
    const void *net = net_settings_blob(&nlen);
    memset(page, 0xff, sizeof page);
    memcpy(page, &f, sizeof f);
    memcpy(page + CFG_NET_OFFSET, net, nlen);
    int r = flash_safe_execute(cfg_write_cb, page, 1000);
    if (r != PICO_OK) printf("!! settings not saved (%d)\n", r);
}

/* ---------------------------------------------------------------------------
   Disk seeding: write the built-in disk image (disk_seed.h) into flash
--------------------------------------------------------------------------- */
/* the source must be in RAM: XIP is off while the flash is programmed */
static uint8_t seed_buf[FLASH_SECTOR_SIZE];

static void seed_cb(void *p)
{
    uint32_t blk = (uint32_t)(uintptr_t)p;
    flash_range_erase(DISK_FLASH_OFFSET + blk * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    flash_range_program(DISK_FLASH_OFFSET + blk * FLASH_SECTOR_SIZE, seed_buf, FLASH_SECTOR_SIZE);
}

static bool disk_seeded(void)
{
    return memcmp((const void *)(XIP_BASE + DISK_FLASH_OFFSET), disk_seed, 16 * 512) == 0;  /* boot + driver */
}

/* Same partition layout as the built-in image (root sector partition table
   and the C: boot sector's BPB)? Then only boot code and driver changed and
   the files on C: can stay. */
static bool disk_same_layout(void)
{
    const uint8_t *f = (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET);
    return memcmp(f + 0x1c2, disk_seed + 0x1c2, 0x1fe - 0x1c2) == 0 &&
           memcmp(f + 16 * 512 + 11, disk_seed + 16 * 512 + 11, 0x3e - 11) == 0;
}

/* all = false: only sectors 0-15 (root sector and driver, flash blocks 0-1) */
static void disk_seed_write_part(bool all)
{
    uint32_t nblk = all ? DISK_SEED_BLOCKS : 2;
    if (all) printf("writing built-in disk image (%lu KB)...\n", (unsigned long)nblk * 4);
    else     printf("updating boot code and driver, files on C: are kept...\n");
    for (uint32_t b = 0; b < nblk; b++) {
        memcpy(seed_buf, disk_seed + b * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        int r = flash_safe_execute(seed_cb, (void *)(uintptr_t)b, 1000);
        if (r != PICO_OK) { printf("!! flash write failed at block %lu (%d)\n", (unsigned long)b, r); return; }
    }
    int bad = memcmp((const void *)(XIP_BASE + DISK_FLASH_OFFSET), disk_seed, nblk * FLASH_SECTOR_SIZE);
    printf("disk image %s\n", bad ? "VERIFY FAILED" : "written and verified");
}

static void disk_seed_write(void) { disk_seed_write_part(true); }

/* ---------------------------------------------------------------------------
   Sniffer decoder
--------------------------------------------------------------------------- */
static uint32_t t_origin;
static uint8_t  cmd[16];
static int      cmd_n;
static uint32_t cmd_t;
static uint32_t ack_n, ack_rw;
static uint8_t  ack_first[16];
static uint32_t last_ev_t;

static void ts(uint32_t t)
{
    uint32_t d = t - t_origin;
    printf("[%4lu.%06lu] ", (unsigned long)(d / 1000000), (unsigned long)(d % 1000000));
}

static void flush_acks(void)
{
    if (!ack_n) return;
    printf("               DMA %s %lu bytes:", ack_rw ? "device->ST" : "ST->device",
           (unsigned long)ack_n);
    for (uint32_t i = 0; i < ack_n && i < 16; i++) printf(" %02x", ack_first[i]);
    if (ack_n > 16) printf(" ...");
    printf("\n");
    ack_n = 0;
}

static void flush_cmd(void)
{
    if (!cmd_n) return;
    uint8_t op = cmd[0] & 0x1f;
    ts(cmd_t);
    printf("ID%u %-17s CDB:", cmd[0] >> 5,
           op == 0x1f && cmd_n > 1 ? scsi_opname(cmd[1]) : scsi_opname(op));
    for (int i = 0; i < cmd_n; i++) printf(" %02x", cmd[i]);
    if (op == 0x08 || op == 0x0a) {
        if (cmd_n >= 5)
            printf("  lba=%lu n=%u", (unsigned long)(((cmd[1] & 0x1f) << 16) | (cmd[2] << 8) | cmd[3]), cmd[4]);
    }
    if (cmd_n == 1) printf("  (only 1st byte: no device answered)");
    printf("\n");
    cmd_n = 0;
}

static void sniff_event(const acsi_event_t *e)
{
    uint16_t s = e->sample;
    switch (e->type) {
    case EV_CS:
        if (!(s & S_RW) && !(s & S_A1)) {           /* command start */
            flush_acks();
            flush_cmd();
            cmd[0] = S_DATA(s);
            cmd_n = 1;
            cmd_t = e->t_us;
        } else if (!(s & S_RW)) {                  /* further command byte */
            if (cmd_n && cmd_n < (int)sizeof cmd) cmd[cmd_n++] = S_DATA(s);
            else { ts(e->t_us); printf("CS write A1=1 %02x (no command start)\n", S_DATA(s)); }
        } else {                                   /* status read */
            flush_cmd();
            flush_acks();
            ts(e->t_us);
            printf("               STATUS read: %02x\n", S_DATA(s));
        }
        break;
    case EV_ACK:
        flush_cmd();
        if (ack_n < 16) ack_first[ack_n] = S_DATA(s);
        ack_rw = (s & S_RW) != 0;
        ack_n++;
        break;
    case EV_RESET:
        flush_cmd(); flush_acks();
        ts(e->t_us);
        printf("/RESET %s\n", s ? "released (high)" : "ASSERTED (low)");
        break;
    }
}

/* ---------------------------------------------------------------------------
   Target log
--------------------------------------------------------------------------- */
static void target_event(const acsi_event_t *e)
{
    static const char *resname[] = { "ok", "TIMEOUT", "RESET", "PROTOCOL" };
    if (e->type == EV_RESET) {
        ts(e->t_us);
        printf("/RESET %s\n", e->sample ? "released" : "asserted");
        return;
    }
    if (e->type != EV_CMD) return;
    const cmd_log_t *L = &g_cmd_log[e->idx];
    uint8_t op = L->cdb[0] & 0x1f;
    ts(e->t_us);
    printf("ID%u %-15s", L->cdb[0] >> 5, op == 0x1f ? scsi_opname(L->cdb[1]) : scsi_opname(op));
    for (int i = 0; i < L->cdb_len; i++) printf(" %02x", L->cdb[i]);
    printf(" | st=%02x", L->status);
    if (L->status) printf(" sense=%02x", L->sense);
    if (L->bytes) printf(" data=%lu acks=%lu", (unsigned long)L->bytes, (unsigned long)L->acks);
    printf(" %s %luus\n", resname[L->result & 3], (unsigned long)L->us);
    if (L->bytes && L->cap_n) {
        if (L->mism == 0xffff) printf("      ACK data matches (%u samples); tail got", L->cap_n);
        else printf("      ACK data MISMATCH at byte %u (%u samples); got", L->mism, L->cap_n);
        for (int i = 0; i < 8; i++) printf(" %02x", L->got[i]);
        printf(" exp");
        for (int i = 0; i < 8; i++) printf(" %02x", L->exp[i]);
        printf("\n");
    }
}

/* ---------------------------------------------------------------------------
   Console
--------------------------------------------------------------------------- */
static void help(void)
{
    printf("\nACSI2TNFS console. mode=%s id=%u verbose=%s%s\n",
           g_cfg.mode == MODE_SNIFF ? "SNIFF" : "TARGET", g_cfg.acsi_id, g_cfg.verbose ? "on" : "off",
           g_cfg.hidden ? "  ** HIDDEN from the Atari **" : "");
    printf("  s=sniffer  t=target  0-7=ACSI id  v=verbose  H=hide/show  K=clock  i=info  B=bootsel  h=help\n\n");
}

static void info(void)
{
    printf("mode=%s id=%u cmds=%lu rd=%lu wr=%lu err=%lu resets=%lu ackmismatch=%lu\n",
           g_cfg.mode == MODE_SNIFF ? "SNIFF" : "TARGET", g_cfg.acsi_id,
           (unsigned long)g_stats.commands, (unsigned long)g_stats.sectors_read,
           (unsigned long)g_stats.sectors_written, (unsigned long)g_stats.errors,
           (unsigned long)g_stats.resets, (unsigned long)g_stats.ack_mismatch);
    printf("pins now: CS=%d RST=%d ACK=%d A1=%d RW=%d data=%02lx\n",
           gpio_get(PIN_CS), gpio_get(PIN_RST), gpio_get(PIN_ACK), gpio_get(PIN_A1),
           gpio_get(PIN_RW), (unsigned long)((gpio_get_all() >> PIN_D0) & 0xff));
}

static void console(int ch)
{
    switch (ch) {
    case 's': g_cfg.mode = MODE_SNIFF;  cfg_save(); printf("-> SNIFF mode\n"); break;
    case 't': g_cfg.mode = MODE_TARGET; cfg_save(); printf("-> TARGET mode, id %u\n", g_cfg.acsi_id); break;
    case 'v': g_cfg.verbose = !g_cfg.verbose; cfg_save(); printf("verbose %s\n", g_cfg.verbose ? "on" : "off"); break;
    case 'H':
        g_cfg.hidden = !g_cfg.hidden;
        cfg_save();
        printf(g_cfg.hidden ? "-> HIDDEN: the Atari sees no device on id %u from its next boot, reset it now\n"
                            : "-> VISIBLE again on id %u, reset the Atari to boot from it\n", g_cfg.acsi_id);
        break;
    case 'K':
        net_clock_toggle();
        break;
    case 'i': info(); break;
    case 'n': net_console_status(); break;
    case 'N': printf("network test requested\n"); net_request_test(); break;
    case 'F':
        disk_seed_write();
        /* the Atari still has the old FAT and directories cached: keep it
           off the bus until it resets, or its next write corrupts C: */
        g_cfg.mute_until_reset = true;
        printf("** RESET THE ATARI NOW: the adapter answers nothing until then **\n");
        break;
    case 'L': la_done = false; la_armed = true; printf("logic analyser armed (next WRITE)\n"); break;
    case 'l': {
        if (!la_done) { printf("no capture\n"); break; }
        const uint16_t *smp = (const uint16_t *)la_buf;
        uint16_t prev = 0xffff;
        int lines = 0;
        printf("LA t(16ns) D7-0 CS RST ACK A1 RW IRQo DRQo\n");
        for (uint32_t i = 0; i < LA_WORDS * 2 && lines < 4000; i++) {
            uint16_t v = smp[i] & 0x7fff;
            if (v != prev) {
                printf("LA %5lu %02x %d %d %d %d %d %d %d\n", (unsigned long)i, v & 0xff,
                       (v >> 8) & 1, (v >> 9) & 1, (v >> 10) & 1, (v >> 11) & 1, (v >> 12) & 1,
                       (v >> 13) & 1, (v >> 14) & 1);
                prev = v;
                lines++;
            }
        }
        printf("LA end\n");
        break;
    }
    case 'x': {
        static const uint32_t secs[] = { 17, 23, 29, 51, 52 };
        for (int k = 0; k < 5; k++) {
            const uint8_t *p = (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET + secs[k] * 512u);
            printf("== sector %lu", (unsigned long)secs[k]);
            for (int i = 0; i < 512; i++) {
                if (i % 32 == 0) printf("\n%03x:", i);
                printf(" %02x", p[i]);
            }
            printf("\n");
        }
        break;
    }
    case 'd': {
        static const uint32_t secs[] = { 0, 1, 16 };
        for (int k = 0; k < 3; k++) {
            const uint8_t *p = (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET + secs[k] * 512u);
            printf("sector %lu @%08lx:", (unsigned long)secs[k], (unsigned long)(uintptr_t)p);
            for (int i = 0; i < 16; i++) printf(" %02x", p[i]);
            printf(" ...");
            for (int i = 496; i < 512; i++) printf(" %02x", p[i]);
            printf("\n");
        }
        break;
    }
    case 'B': printf("rebooting to BOOTSEL\n"); sleep_ms(100); reset_usb_boot(0, 0); break;
    case 'h': case '?': help(); break;
    default:
        if (ch >= '0' && ch <= '7') {
            g_cfg.acsi_id = (uint8_t)(ch - '0');
            cfg_save();
            printf("ACSI id = %u\n", g_cfg.acsi_id);
        }
    }
}

int main(void)
{
#if PICO_RP2350
    /* the PIO timing was validated on the Atari at 125 MHz (RP2040 default) */
    set_sys_clock_khz(125000, true);
#endif
    stdio_init_all();
    cfg_load();
    acsi_hw_init();
    net_init();

    gpio_init(PIN_LED_PICO);
    gpio_set_dir(PIN_LED_PICO, GPIO_OUT);

    flash_safe_execute_core_init();
    multicore_launch_core1(core1_main);
    t_origin = time_us_32();
    sleep_ms(50);                       /* core1 registers as flash lockout victim */
    bool need_seed = !disk_seeded();
    if (need_seed) disk_seed_write_part(!disk_same_layout());

    uint32_t last_hb = 0, led_until = 0;
    bool was_connected = false;
    for (;;) {
        acsi_event_t e;
        int budget = 256;
        while (budget-- && ev_pop(&e)) {
            last_ev_t = time_us_32();
            if (e.type == EV_DROP) { printf("!! %u events dropped\n", e.sample); continue; }
            if (e.type == EV_MODE) {
                flush_cmd(); flush_acks();
                printf("== mode %s, ACSI id %u ==\n", e.sample ? "TARGET" : "SNIFF", g_cfg.acsi_id);
                continue;
            }
            if (g_cfg.mode == MODE_SNIFF) sniff_event(&e);
            else target_event(&e);
        }
        uint32_t now = time_us_32();
        if ((cmd_n || ack_n) && now - last_ev_t > 20000) { flush_cmd(); flush_acks(); }

        bool conn = stdio_usb_connected();
        if (conn && !was_connected) {
            sleep_ms(50);
            help();
            if (need_seed) { printf("(disk image was missing and has been written at startup)\n"); need_seed = false; }
        }
        was_connected = conn;

        net_poll();
        cfgrpc_poll();

        int ch = getchar_timeout_us(0);
        if (ch >= 0) console(ch);

        if (g_cfg.save_req) { g_cfg.save_req = false; cfg_save(); }
        if (g_cfg.led_test) { g_cfg.led_test = false; led_until = now + 3000000; }
        if ((int32_t)(led_until - now) > 0) gpio_put(PIN_LED_PICO, (now / 150000) & 1);
        else if (!BOARD_HAS_WIFI && now - last_hb > 500000) {  /* W: led shared with core1 */
            last_hb = now;
            gpio_put(PIN_LED_PICO, g_cfg.mode == MODE_TARGET ? 1 : !gpio_get(PIN_LED_PICO));
        }
    }
}
