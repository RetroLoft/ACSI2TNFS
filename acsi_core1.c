/*
 * ACSI2TNFS - core1: real-time ACSI bus handling
 *
 *  - MODE_SNIFF : fully passive. U1 stays Atari -> Pico, /IRQ and /DRQ are
 *                 never asserted. Every /CS and /ACK strobe is forwarded to
 *                 core0 for decoding.
 *  - MODE_TARGET: ACSI hard disk backed by the Pico flash.
 *
 * All protocol timing that is tighter than a few microseconds lives in PIO
 * (acsi_bus.pio). This file only does the command/status handshake, which the
 * ACSI/DMA Integration Guide allows up to 20 us of latency for.
 */
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/pio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/dma.h"
#include "acsi.h"
#include "acsi_bus.pio.h"

#define FW_VERSION "0.2"

static PIO const pio = ACSI_PIO;
static uint off_cs, off_ack, off_dout, off_din;

acsi_stats_t g_stats;
cmd_log_t g_cmd_log[CMD_LOG_N];
static uint8_t cmd_log_idx;

/* ---------------------------------------------------------------------------
   Event ring (single producer core1, single consumer core0)
--------------------------------------------------------------------------- */
#define EV_N 4096
static acsi_event_t ev_buf[EV_N];
static volatile uint32_t ev_head, ev_tail;
static uint32_t ev_dropped;

void __not_in_flash_func(ev_push)(uint8_t type, uint16_t sample, uint8_t idx)
{
    uint32_t h = ev_head;
    if (h - ev_tail >= EV_N - 1) {
        ev_dropped++;
        return;
    }
    if (ev_dropped) {
        acsi_event_t *d = &ev_buf[h % EV_N];
        d->t_us = time_us_32();
        d->type = EV_DROP;
        d->sample = ev_dropped > 0xffff ? 0xffff : ev_dropped;
        ev_dropped = 0;
        h++;
        if (h - ev_tail >= EV_N - 1) {
            __dmb();
            ev_head = h;
            return;
        }
    }
    acsi_event_t *e = &ev_buf[h % EV_N];
    e->t_us = time_us_32();
    e->type = type;
    e->sample = sample;
    e->idx = idx;
    __dmb();
    ev_head = h + 1;
}

bool ev_pop(acsi_event_t *e)
{
    uint32_t t = ev_tail;
    if (t == ev_head) return false;
    __dmb();
    *e = ev_buf[t % EV_N];
    __dmb();
    ev_tail = t + 1;
    return true;
}

/* ---------------------------------------------------------------------------
   Low level bus control
--------------------------------------------------------------------------- */
static bool bus_driven;

static inline void irq_assert(void)  { pio_sm_exec(pio, SM_CS, pio_encode_set(pio_pins, 1)); }
static inline void irq_release(void) { pio_sm_exec(pio, SM_CS, pio_encode_set(pio_pins, 0)); }
static inline bool reset_active(void) { return !gpio_get(PIN_RST); }

/* Execute "pull; out pins|pindirs, 8" on the (disabled) DOUT state machine. */
static void __not_in_flash_func(dout_exec_out)(uint32_t v, bool pindirs)
{
    pio_sm_put(pio, SM_DOUT, v);
    pio_sm_exec(pio, SM_DOUT, pio_encode_pull(false, true));
    pio_sm_exec(pio, SM_DOUT, pio_encode_out(pindirs ? pio_pindirs : pio_pins, 8));
}

/* Pico drives the ACSI data bus. Sequence avoids contention on either side. */
static void __not_in_flash_func(bus_drive)(void)
{
    if (bus_driven) return;
    gpio_put(PIN_OE, 1);            /* both sides high-Z          */
    gpio_put(PIN_DIR, 0);           /* B (Pico) -> A (ACSI)       */
    dout_exec_out(0xff, true);      /* Pico data pins = outputs   */
    gpio_put(PIN_OE, 0);
    bus_driven = true;
}

/* Back to listening: Atari -> Pico. */
static void __not_in_flash_func(bus_release)(void)
{
    gpio_put(PIN_OE, 1);
    dout_exec_out(0x00, true);      /* Pico data pins = inputs    */
    gpio_put(PIN_DIR, 1);
    gpio_put(PIN_OE, 0);
    bus_driven = false;
}

static void __not_in_flash_func(sm_start)(uint sm, uint off)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(off));
    pio_sm_set_enabled(pio, sm, true);
}

static void __not_in_flash_func(sm_stop)(uint sm)
{
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_set(pio_pins, 0));   /* /DRQ released */
}

/* /ACK samples = the bytes the Atari actually latched */
static uint8_t  ackcap[4608];
static uint32_t ackcap_n;
static const uint8_t *exp_a, *exp_b;  /* DMA-out stream of this command */
static uint32_t exp_alen, exp_blen;
static uint8_t exp_byte(uint32_t i)
{
    if (i < exp_alen) return exp_a[i];
    if (i - exp_alen < exp_blen) return exp_b[i - exp_alen];
    return 0xee;
}

static void __not_in_flash_func(ack_drain)(uint32_t *count)
{
    while (!pio_sm_is_rx_fifo_empty(pio, SM_ACK)) {
        uint32_t v = pio->rxf[SM_ACK];
        if (count) {
            (*count)++;
            if (ackcap_n < sizeof ackcap) ackcap[ackcap_n++] = (uint8_t)v;
        }
    }
}

/* ---------------------------------------------------------------------------
   Transaction state
--------------------------------------------------------------------------- */
enum { RES_OK = 0, RES_TIMEOUT, RES_RESET, RES_PROTO };

static uint8_t  res;             /* result of the last wait              */
static bool     have_pending;    /* a new command start arrived early    */
static uint32_t pending_sample;
static uint32_t acks;            /* /ACK pulses in current data phase    */
static uint8_t  sense_code;      /* ACSI error code for REQUEST SENSE    */
static uint8_t  sense_key, sense_asc;
static int      pending_id = -1; /* new id, applied on next Atari reset  */

static void set_sense(uint8_t acsi_code, uint8_t key, uint8_t asc)
{
    sense_code = acsi_code;
    sense_key = key;
    sense_asc = asc;
}

static bool __not_in_flash_func(cs_wait)(uint32_t *s, uint32_t timeout_us)
{
    uint32_t t0 = time_us_32();
    while (pio_sm_is_rx_fifo_empty(pio, SM_CS)) {
        if (reset_active()) { res = RES_RESET; return false; }
        if (time_us_32() - t0 > timeout_us) { res = RES_TIMEOUT; return false; }
    }
    *s = pio->rxf[SM_CS];
    return true;
}

/*
 * DMA: device -> Atari, as ONE continuous stream (buffer a, then buffer b).
 *
 * The RP2040 DMA keeps the PIO TX FIFO full, so there is never a gap in the
 * data. That matters: /DRQ is switched by a BC547 that releases slowly, and
 * the ST DMA chip then strobes /ACK once or twice more. Mid-stream that just
 * takes the next byte (which is already on the bus); a gap between sectors
 * would instead hand the Atari stale bytes and shift the rest of the data.
 */
static int dch_a = -1, dch_b = -1, dch_la = -1;

/* logic analyser on PIO1: armed from the console, fires at the next write */
#define LA_WORDS 8192
uint32_t la_buf[LA_WORDS];
volatile bool la_armed, la_done;
static uint off_la;

static void la_start(void)
{
    PIO p1 = pio1;
    pio_sm_set_enabled(p1, 0, false);
    pio_sm_clear_fifos(p1, 0);
    pio_sm_restart(p1, 0);
    pio_sm_exec(p1, 0, pio_encode_jmp(off_la));
    dma_channel_config c = dma_channel_get_default_config(dch_la);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(p1, 0, false));
    dma_channel_configure(dch_la, &c, la_buf, &p1->rxf[0], LA_WORDS, true);
    pio_sm_set_enabled(p1, 0, true);
}

static bool __not_in_flash_func(dma_out2)(const uint8_t *a, uint32_t alen,
                                          const uint8_t *b, uint32_t blen)
{
    if (!exp_a) {
        exp_a = a; exp_alen = alen;
        exp_b = b; exp_blen = blen;
    }
    bus_drive();
    sm_start(SM_DOUT, off_dout + acsi_dout_offset_start);

    dma_channel_config cb = dma_channel_get_default_config(dch_b);
    channel_config_set_transfer_data_size(&cb, DMA_SIZE_8);
    channel_config_set_read_increment(&cb, true);
    channel_config_set_write_increment(&cb, false);
    channel_config_set_dreq(&cb, pio_get_dreq(pio, SM_DOUT, true));
    dma_channel_configure(dch_b, &cb, &pio->txf[SM_DOUT], b, blen, false);

    dma_channel_config ca = dma_channel_get_default_config(dch_a);
    channel_config_set_transfer_data_size(&ca, DMA_SIZE_8);
    channel_config_set_read_increment(&ca, true);
    channel_config_set_write_increment(&ca, false);
    channel_config_set_dreq(&ca, pio_get_dreq(pio, SM_DOUT, true));
    if (blen) channel_config_set_chain_to(&ca, dch_b);
    dma_channel_configure(dch_a, &ca, &pio->txf[SM_DOUT], a, alen, true);

    /* done when both channels finished, the FIFO is empty and the SM waits
       on "pull" with /ACK high (the last byte has been taken) */
    uint32_t t0 = time_us_32(), last_acks = acks;
    while (dma_channel_is_busy(dch_a) || dma_channel_is_busy(dch_b) ||
           !(pio_sm_is_tx_fifo_empty(pio, SM_DOUT) &&
             pio->sm[SM_DOUT].addr == off_dout + acsi_dout_offset_start &&
             gpio_get(PIN_ACK))) {
        ack_drain(&acks);
        if (acks != last_acks) { last_acks = acks; t0 = time_us_32(); }
        if (reset_active()) { res = RES_RESET; goto fail; }
        if (time_us_32() - t0 > 500000) { res = RES_TIMEOUT; goto fail; }
    }
    busy_wait_us_32(2);
    ack_drain(&acks);
    sm_stop(SM_DOUT);
    return true;
fail:
    dma_channel_abort(dch_a);
    dma_channel_abort(dch_b);
    ack_drain(&acks);
    sm_stop(SM_DOUT);
    return false;
}

static bool dma_out(const uint8_t *buf, uint32_t len)
{
    return dma_out2(buf, len, NULL, 0);
}

/* DMA: Atari -> device. The DIN state machine must already be running. */
/* copy of everything received in this command, for the /ACK cross-check */
static uint8_t  rxcopy[4608];
#define WBUF_SECTORS 128
static uint8_t  wbuf[WBUF_SECTORS * 512];   /* one complete write transfer */
static uint32_t rx_n;

static bool __not_in_flash_func(dma_in_chunk)(uint8_t *buf, uint32_t len)
{
    uint32_t t0 = time_us_32();
    for (uint32_t i = 0; i < len; ) {
        ack_drain(&acks);
        if (!pio_sm_is_rx_fifo_empty(pio, SM_DIN)) {
            uint8_t v = (uint8_t)pio->rxf[SM_DIN];
            buf[i++] = v;
            if (rx_n < sizeof rxcopy) rxcopy[rx_n++] = v;
            t0 = time_us_32();
            continue;
        }
        if (reset_active()) { res = RES_RESET; return false; }
        if (time_us_32() - t0 > 1000000) { res = RES_TIMEOUT; return false; }
    }
    /* let the sampler catch the last strobe */
    busy_wait_us_32(2);
    ack_drain(&acks);
    return true;
}

/* Status phase: put status on the bus, raise /IRQ, wait for the /CS read. */
static bool __not_in_flash_func(status_phase)(uint8_t st)
{
    uint32_t s;
    dout_exec_out(st, false);
    bus_drive();
    irq_assert();
    if (!cs_wait(&s, 3000000)) {
        irq_release();
        bus_release();
        return false;
    }
    if (!(s & S_RW)) {
        /* host wrote instead of reading: get off the bus at once */
        bus_release();
        if (!(s & S_A1)) { have_pending = true; pending_sample = s; }
        res = RES_PROTO;
        return false;
    }
    uint32_t t0 = time_us_32();
    while (!gpio_get(PIN_CS) && time_us_32() - t0 < 1000) { }
    bus_release();
    irq_release();
    return true;
}

/* ---------------------------------------------------------------------------
   Flash backed disk
--------------------------------------------------------------------------- */
static const uint8_t *disk_ptr(uint32_t lba)
{
    return (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET + lba * 512u);
}

static uint8_t sector_buf[512];
static uint8_t stage[FLASH_SECTOR_SIZE];
static int32_t stage_blk = -1;
static bool    stage_dirty;

static void __not_in_flash_func(flash_commit_cb)(void *p)
{
    uint32_t off = DISK_FLASH_OFFSET + (uint32_t)stage_blk * FLASH_SECTOR_SIZE;
    (void)p;
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, stage, FLASH_SECTOR_SIZE);
}

static bool stage_flush(void)
{
    if (stage_blk < 0 || !stage_dirty) return true;
    stage_dirty = false;
    if (memcmp(stage, disk_ptr((uint32_t)stage_blk * 8u), FLASH_SECTOR_SIZE) == 0)
        return true;
    return flash_safe_execute(flash_commit_cb, NULL, 1000) == PICO_OK;
}

static void stage_select(uint32_t blk)
{
    if ((int32_t)blk == stage_blk) return;
    stage_flush();
    memcpy(stage, disk_ptr(blk * 8u), FLASH_SECTOR_SIZE);
    stage_blk = (int32_t)blk;
}

/* Sector 0 gets our ACSI id and a matching boot checksum (0x1234). */
static const uint8_t *root_sector(void)
{
    const uint8_t *src = disk_ptr(0);
    memcpy(sector_buf, src, 512);
    uint16_t sum = 0;
    for (int i = 0; i < 512; i += 2) sum += (sector_buf[i] << 8) | sector_buf[i + 1];
    if (sum == 0x1234) {
        sector_buf[ROOT_ID_OFFSET] = g_cfg.acsi_id;
        sum = 0;
        for (int i = 0; i < 510; i += 2) sum += (sector_buf[i] << 8) | sector_buf[i + 1];
        uint16_t fix = (uint16_t)(0x1234 - sum);
        sector_buf[510] = fix >> 8;
        sector_buf[511] = fix & 0xff;
    }
    return sector_buf;
}

static uint8_t read_sectors(uint32_t lba, uint32_t n)
{
    if (lba + n > DISK_SECTORS || lba + n < lba) {
        set_sense(0x21, 0x05, 0x21);
        return 0x02;
    }
    bool ok;
    if (lba == 0)       /* patched root sector from RAM, rest straight from flash */
        ok = dma_out2(root_sector(), 512, disk_ptr(1), (n - 1) * 512u);
    else
        ok = dma_out2(disk_ptr(lba), n * 512u, NULL, 0);
    if (!ok) return 0x02;
    g_stats.sectors_read += n;
    return 0x00;
}

static uint8_t write_sectors(uint32_t lba, uint32_t n)
{
    if (lba + n > DISK_SECTORS || lba + n < lba) {
        set_sense(0x21, 0x05, 0x21);
        return 0x02;
    }
    if (n > WBUF_SECTORS) {             /* larger than our RAM buffer */
        set_sense(0x24, 0x05, 0x24);
        return 0x02;
    }
    /* 1. receive the whole transfer into RAM, FIFO emptied by RP2040 DMA */
    const uint32_t len = n * 512u;
    if (la_armed) { la_armed = false; la_start(); la_done = true; }
    /* DMA first, THEN the state machine: the SM must never stall on a full
       FIFO, because the real /DRQ line stays low for ~1 us after we release
       it (slow BC547) and the ST keeps sending bytes during that time. */
    pio_sm_set_enabled(pio, SM_DIN, false);
    pio_sm_clear_fifos(pio, SM_DIN);
    dma_channel_config c = dma_channel_get_default_config(dch_a);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, SM_DIN, false));
    dma_channel_configure(dch_a, &c, wbuf, &pio->rxf[SM_DIN], len, true);
    sm_start(SM_DIN, off_din + acsi_din_offset_start);

    bool ok = true;
    uint32_t t0 = time_us_32(), left = len;
    while (dma_channel_is_busy(dch_a)) {
        ack_drain(&acks);
        uint32_t l = dma_channel_hw_addr(dch_a)->transfer_count;
        if (l != left) { left = l; t0 = time_us_32(); }
        if (reset_active()) { res = RES_RESET; ok = false; break; }
        if (time_us_32() - t0 > 1000000) { res = RES_TIMEOUT; ok = false; break; }
    }
    busy_wait_us_32(2);
    ack_drain(&acks);
    if (!ok) dma_channel_abort(dch_a);
    sm_stop(SM_DIN);
    rx_n = len < sizeof rxcopy ? len : sizeof rxcopy;   /* for the /ACK cross-check */
    memcpy(rxcopy, wbuf, rx_n);
    if (!ok) return 0x02;

    /* 2. only now touch the flash (the Atari waits for the status byte) */
    for (uint32_t i = 0; i < n; i++, lba++) {
        stage_select(lba / 8u);
        memcpy(stage + (lba % 8u) * 512u, wbuf + i * 512u, 512);
        stage_dirty = true;
        g_stats.sectors_written++;
    }
    if (!stage_flush()) {
        set_sense(0x03, 0x03, 0x0c);
        return 0x02;
    }
    return 0x00;
}

/* ---------------------------------------------------------------------------
   Command execution
--------------------------------------------------------------------------- */
static uint8_t reply[512];

static uint8_t send_reply(uint32_t len)
{
    return dma_out(reply, len) ? 0x00 : 0x02;
}

static uint32_t info_text(char *p, uint32_t max)
{
    uint32_t up = time_us_32() / 1000000u;
    int n = snprintf(p, max,
        "ACSI2TNFS firmware " FW_VERSION " (" __DATE__ " " __TIME__ ")\r\n"
        "Board      : Raspberry Pi Pico (RP2040), no Wi-Fi\r\n"
        "ACSI id    : %u%s\r\n"
        "Disk       : %u sectors (%u KB) in Pico flash\r\n"
        "Uptime     : %lu:%02lu:%02lu\r\n"
        "Commands   : %lu\r\n"
        "Sectors    : %lu read, %lu written\r\n"
        "Errors     : %lu (ACK mismatches: %lu)\r\n",
        g_cfg.acsi_id, pending_id >= 0 ? " (change pending, reset Atari)" : "",
        DISK_SECTORS, DISK_SECTORS / 2,
        (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60),
        (unsigned long)g_stats.commands,
        (unsigned long)g_stats.sectors_read, (unsigned long)g_stats.sectors_written,
        (unsigned long)g_stats.errors, (unsigned long)g_stats.ack_mismatch);
    return n < 0 ? 0 : (uint32_t)n;
}

static uint8_t exec_cmd(const uint8_t *cdb, uint8_t cdb_len, uint32_t *bytes)
{
    const bool ext = (cdb[0] & 0x1f) == 0x1f;
    const uint8_t *c = ext ? cdb + 1 : cdb;
    const uint8_t op = ext ? c[0] : (cdb[0] & 0x1f);
    uint32_t len, lba, n;
    (void)cdb_len;

    memset(reply, 0, sizeof reply);
    switch (op) {
    case 0x00:  /* TEST UNIT READY */
    case 0x04:  /* FORMAT UNIT: nothing to do on flash */
    case 0x0b:  /* SEEK */
    case 0x1b:  /* START STOP UNIT */
    case 0x1e:  /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2f:  /* VERIFY(10) */
        set_sense(0, 0, 0);
        return 0x00;

    case 0x03:  /* REQUEST SENSE */
        len = c[4] ? c[4] : 4;
        if (len > sizeof reply) len = sizeof reply;
        if (len <= 4) {
            reply[0] = sense_code;
        } else {
            reply[0] = 0x70;
            reply[2] = sense_key;
            reply[7] = 10;
            reply[12] = sense_asc;
        }
        set_sense(0, 0, 0);
        *bytes = len;
        return send_reply(len);

    case 0x08:  /* READ(6) */
        lba = ((c[1] & 0x1fu) << 16) | (c[2] << 8) | c[3];
        n = c[4] ? c[4] : 256;
        *bytes = n * 512;
        return read_sectors(lba, n);

    case 0x0a:  /* WRITE(6) */
        lba = ((c[1] & 0x1fu) << 16) | (c[2] << 8) | c[3];
        n = c[4] ? c[4] : 256;
        *bytes = n * 512;
        return write_sectors(lba, n);

    case 0x28:  /* READ(10) */
    case 0x2a:  /* WRITE(10) */
        if (!ext) break;
        lba = ((uint32_t)c[2] << 24) | (c[3] << 16) | (c[4] << 8) | c[5];
        n = (c[7] << 8) | c[8];
        *bytes = n * 512;
        return op == 0x28 ? read_sectors(lba, n) : write_sectors(lba, n);

    case 0x25:  /* READ CAPACITY(10) */
        if (!ext) break;
        reply[0] = (DISK_SECTORS - 1) >> 24; reply[1] = (DISK_SECTORS - 1) >> 16;
        reply[2] = (DISK_SECTORS - 1) >> 8;  reply[3] = (DISK_SECTORS - 1) & 0xff;
        reply[6] = 0x02;                        /* 512 byte blocks */
        *bytes = 8;
        return send_reply(8);

    case 0x12:  /* INQUIRY: padded to the allocation length (DMA FIFO!) */
        len = c[4];
        if (len > 255) len = 255;
        reply[0] = 0x00;           /* direct access          */
        reply[2] = 0x01;
        reply[3] = 0x01;
        reply[4] = 31;
        memcpy(reply + 8,  "PICO    ", 8);
        memcpy(reply + 16, "ACSI2TNFS FLASH ", 16);
        memcpy(reply + 32, FW_VERSION "  ", 4);
        *bytes = len;
        return len ? send_reply(len) : 0x00;

    case 0x1a:  /* MODE SENSE(6) */
        len = c[4];
        reply[0] = 11;
        reply[3] = 8;
        reply[5] = DISK_SECTORS >> 16; reply[6] = DISK_SECTORS >> 8; reply[7] = DISK_SECTORS & 0xff;
        reply[10] = 0x02;
        *bytes = len;
        return len ? send_reply(len) : 0x00;

    case 0x15:  /* MODE SELECT(6): accept and ignore parameter list */
        len = c[4];
        if (len) {
            sm_start(SM_DIN, off_din + acsi_din_offset_start);
            bool ok = dma_in_chunk(reply, len);
            sm_stop(SM_DIN);
            if (!ok) return 0x02;
        }
        *bytes = len;
        return 0x00;

    case 0x11:  /* vendor: [0x11|id, 'A', 'T', sub, len/arg, 0] */
        if (ext || c[1] != 'A' || c[2] != 'T') break;
        switch (c[3]) {
        case 0:     /* info text, NUL terminated */
            len = c[4] ? c[4] : 256;
            info_text((char *)reply, sizeof reply);
            *bytes = len;
            return send_reply(len);
        case 1:     /* blink the led */
            g_cfg.led_test = true;
            return 0x00;
        case 2:     /* set ACSI id (applied at next Atari reset) */
            if (c[4] > 7) break;
            pending_id = c[4];
            return 0x00;
        }
        break;
    }
    set_sense(0x20, 0x05, 0x20);    /* invalid command */
    return 0x02;
}

static void __not_in_flash_func(handle_command)(uint32_t s0)
{
    cmd_log_t L;
    memset(&L, 0, sizeof L);
    uint32_t t0 = time_us_32();
    uint8_t len = 6, n = 1;
    uint32_t s;

    L.cdb[0] = S_DATA(s0);
    irq_assert();
    while (n < len) {
        if (!cs_wait(&s, 250000)) {
            irq_release();
            L.result = res;
            goto done;
        }
        if (s & S_RW) { L.result = RES_PROTO; goto done; }
        if (!(s & S_A1)) {              /* host restarted: new command */
            have_pending = true;
            pending_sample = s;
            L.result = RES_PROTO;
            goto done;
        }
        L.cdb[n++] = S_DATA(s);
        if (n == 2 && (L.cdb[0] & 0x1f) == 0x1f) {
            uint8_t grp = L.cdb[1] >> 5;
            len = 1 + ((grp == 1 || grp == 2) ? 10 : grp == 5 ? 12 : 6);
        }
        if (n < len) irq_assert();
    }

    gpio_put(PIN_LED, 1);
    acks = 0;
    ack_drain(NULL);
    ackcap_n = 0;
    exp_a = exp_b = NULL;
    exp_alen = exp_blen = 0;
    rx_n = 0;
    res = RES_OK;
    L.status = exec_cmd(L.cdb, len, &L.bytes);
    /* No /ACK cross-check for writes: the CPU-drained sampler misses strobes
       there (verified with the logic analyser, 'L'/'l'); check writes by
       reading the flash back instead ('x'). */
    L.result = res;
    L.acks = acks;
    L.cap_n = (uint16_t)ackcap_n;
    L.mism = 0xffff;
    if (exp_a) {
        uint32_t tot = exp_alen + exp_blen;
        uint32_t m = tot < ackcap_n ? tot : ackcap_n;
        for (uint32_t i = 0; i < m; i++)
            if (ackcap[i] != exp_byte(i)) { L.mism = (uint16_t)i; break; }
        uint32_t b = L.mism == 0xffff ? (m >= 4 ? m - 4 : 0) : (L.mism >= 2 ? L.mism - 2u : 0);
        for (uint32_t i = 0; i < 8; i++) {
            L.got[i] = b + i < ackcap_n ? ackcap[b + i] : 0xee;
            L.exp[i] = exp_byte(b + i);
        }
    }
    if (res == RES_RESET) goto done;
    if (res != RES_OK) L.status = 0x02;
    if (L.status) {
        g_stats.errors++;
        if (sense_code == 0) set_sense(0x03, 0x04, 0x44);
    }
    L.sense = sense_code;
    if (!status_phase(L.status) && L.result == RES_OK) L.result = res;

done:
    gpio_put(PIN_LED, 0);
    g_stats.commands++;
    if (L.bytes && L.acks != L.bytes && L.result == RES_OK) g_stats.ack_mismatch++;
    L.cdb_len = n;
    L.us = time_us_32() - t0;
    uint8_t idx = cmd_log_idx++ % CMD_LOG_N;
    g_cmd_log[idx] = L;
    if (g_cfg.verbose || L.status || L.result) ev_push(EV_CMD, 0, idx);
}

/* ---------------------------------------------------------------------------
   Mode loops
--------------------------------------------------------------------------- */
static void bus_idle(void)
{
    sm_stop(SM_DOUT);
    sm_stop(SM_DIN);
    irq_release();
    bus_release();
    have_pending = false;
}

static void __not_in_flash_func(sniff_loop)(void)
{
    bool rst = gpio_get(PIN_RST);
    ev_push(EV_RESET, rst, 0);
    while (g_cfg.mode == MODE_SNIFF) {
        while (!pio_sm_is_rx_fifo_empty(pio, SM_CS))
            ev_push(EV_CS, (uint16_t)pio->rxf[SM_CS], 0);
        while (!pio_sm_is_rx_fifo_empty(pio, SM_ACK))
            ev_push(EV_ACK, (uint16_t)pio->rxf[SM_ACK], 0);
        bool r = gpio_get(PIN_RST);
        if (r != rst) {
            rst = r;
            ev_push(EV_RESET, r, 0);
        }
    }
}

static void __not_in_flash_func(target_loop)(void)
{
    bool in_reset = false;
    while (g_cfg.mode == MODE_TARGET) {
        if (reset_active()) {
            if (!in_reset) {
                in_reset = true;
                bus_idle();
                g_stats.resets++;
                ev_push(EV_RESET, 0, 0);
                if (pending_id >= 0) {
                    g_cfg.acsi_id = (uint8_t)pending_id;
                    pending_id = -1;
                    g_cfg.save_req = true;
                }
            }
            continue;
        } else if (in_reset) {
            in_reset = false;
            pio_sm_clear_fifos(pio, SM_CS);
            ev_push(EV_RESET, 1, 0);
        }
        ack_drain(NULL);

        uint32_t s;
        if (have_pending) {
            have_pending = false;
            s = pending_sample;
        } else if (!pio_sm_is_rx_fifo_empty(pio, SM_CS)) {
            s = pio->rxf[SM_CS];
        } else {
            continue;
        }
        if (s & (S_RW | S_A1)) continue;            /* not a command start */
        if ((S_DATA(s) >> 5) != g_cfg.acsi_id) continue;
        handle_command(s);
    }
}

/* ---------------------------------------------------------------------------
   Setup
--------------------------------------------------------------------------- */
void acsi_hw_init(void)
{
    /* buffer control: enabled, Atari -> Pico (same as the resistor defaults) */
    gpio_init(PIN_OE);  gpio_put(PIN_OE, 0);  gpio_set_dir(PIN_OE, GPIO_OUT);
    gpio_init(PIN_DIR); gpio_put(PIN_DIR, 1); gpio_set_dir(PIN_DIR, GPIO_OUT);

    for (uint p = PIN_D0; p <= PIN_RW; p++) {
        gpio_init(p);
        gpio_set_dir(p, GPIO_IN);
        gpio_disable_pulls(p);
    }
    /* IRQ/DRQ transistors: keep off */
    gpio_init(PIN_IRQ); gpio_put(PIN_IRQ, 0); gpio_set_dir(PIN_IRQ, GPIO_OUT);
    gpio_init(PIN_DRQ); gpio_put(PIN_DRQ, 0); gpio_set_dir(PIN_DRQ, GPIO_OUT);

    gpio_init(PIN_LED); gpio_put(PIN_LED, 0); gpio_set_dir(PIN_LED, GPIO_OUT);

    off_cs   = pio_add_program(pio, &acsi_cs_program);
    off_ack  = pio_add_program(pio, &acsi_ack_program);
    off_dout = pio_add_program(pio, &acsi_dout_program);
    off_din  = pio_add_program(pio, &acsi_din_program);

    /* SM_CS: in = GP8.., set = IRQ */
    pio_sm_config c = acsi_cs_program_get_default_config(off_cs);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_set_pins(&c, PIN_IRQ, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, SM_CS, off_cs, &c);

    c = acsi_ack_program_get_default_config(off_ack);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_jmp_pin(&c, PIN_ACK);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, SM_ACK, off_ack, &c);

    c = acsi_dout_program_get_default_config(off_dout);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_set_pins(&c, PIN_DRQ, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_init(pio, SM_DOUT, off_dout, &c);

    c = acsi_din_program_get_default_config(off_din);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_jmp_pin(&c, PIN_ACK);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_set_pins(&c, PIN_DRQ, 1);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    pio_sm_init(pio, SM_DIN, off_din, &c);

    /* pin levels first, then hand IRQ/DRQ and the data pins to PIO */
    pio_sm_set_pins_with_mask(pio, SM_CS, 0, (1u << PIN_IRQ) | (1u << PIN_DRQ) | (0xffu << PIN_D0));
    pio_sm_set_pindirs_with_mask(pio, SM_CS, (1u << PIN_IRQ) | (1u << PIN_DRQ),
                                 (1u << PIN_IRQ) | (1u << PIN_DRQ) | (0xffu << PIN_D0));
    for (uint p = PIN_D0; p < PIN_D0 + 8; p++) pio_gpio_init(pio, p);
    pio_gpio_init(pio, PIN_IRQ);
    pio_gpio_init(pio, PIN_DRQ);
    bus_driven = false;

    dch_a = dma_claim_unused_channel(true);
    dch_b = dma_claim_unused_channel(true);
    dch_la = dma_claim_unused_channel(true);
    off_la = pio_add_program(pio1, &acsi_la_program);
    pio_sm_config lc = acsi_la_program_get_default_config(off_la);
    sm_config_set_in_pins(&lc, PIN_D0);
    sm_config_set_in_shift(&lc, true, true, 32);
    sm_config_set_fifo_join(&lc, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&lc, 2.0f);           /* 62.5 MHz = 16 ns per sample */
    pio_sm_init(pio1, 0, off_la, &lc);

    pio_sm_set_enabled(pio, SM_CS, true);
    pio_sm_set_enabled(pio, SM_ACK, true);
}

void core1_main(void)
{
    flash_safe_execute_core_init();
    for (;;) {
        bus_idle();
        pio_sm_clear_fifos(pio, SM_CS);
        pio_sm_clear_fifos(pio, SM_ACK);
        ev_push(EV_MODE, g_cfg.mode, 0);
        if (g_cfg.mode == MODE_SNIFF) sniff_loop();
        else target_loop();
    }
}

const char *scsi_opname(uint8_t op)
{
    switch (op) {
    case 0x00: return "TEST UNIT READY";
    case 0x03: return "REQUEST SENSE";
    case 0x04: return "FORMAT";
    case 0x08: return "READ(6)";
    case 0x0a: return "WRITE(6)";
    case 0x0b: return "SEEK";
    case 0x11: return "VENDOR(ACSI2TNFS)";
    case 0x12: return "INQUIRY";
    case 0x15: return "MODE SELECT";
    case 0x1a: return "MODE SENSE";
    case 0x1b: return "START/STOP";
    case 0x1e: return "PREVENT/ALLOW";
    case 0x1f: return "ICD EXTENDED";
    case 0x25: return "READ CAPACITY";
    case 0x28: return "READ(10)";
    case 0x2a: return "WRITE(10)";
    case 0x2f: return "VERIFY(10)";
    default:   return "?";
    }
}
