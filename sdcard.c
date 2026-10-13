/*
 * ACSI2TNFS - micro SD card in SPI mode on SPI0 (GP2 CLK, GP3 MOSI, GP4 MISO,
 * GP5 CS, the pin map of v2; wired the same on the dev board).
 *
 * For now only what the console test 'c' needs: bring the card up, read its
 * CID/CSD and one block. Later the block layer for FatFs (to do: SD card).
 * Runs on core0.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "acsi.h"

#define SD_SPI      spi0
#define PIN_SD_CLK  2
#define PIN_SD_MOSI 3
#define PIN_SD_MISO 4
#define PIN_SD_CS   5

static bool sd_hc;                      /* SDHC/SDXC: block addresses, else bytes */

static uint8_t xfer(uint8_t b)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static void cs(bool on)
{
    gpio_put(PIN_SD_CS, !on);
    xfer(0xff);                         /* one byte with the new CS level */
}

/* command, R1 answer (0xff: no answer) */
static uint8_t cmd(uint8_t c, uint32_t arg, uint8_t crc)
{
    uint8_t f[6] = { (uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                     (uint8_t)(arg >> 8), (uint8_t)arg, crc };
    xfer(0xff);
    spi_write_blocking(SD_SPI, f, 6);
    for (int i = 0; i < 10; i++) {
        uint8_t r = xfer(0xff);
        if (!(r & 0x80)) return r;
    }
    return 0xff;
}

static uint8_t acmd(uint8_t c, uint32_t arg)
{
    cmd(55, 0, 0x65);
    return cmd(c, arg, 0x01);
}

/* data block after a command: wait for the start token 0xfe */
static bool data_in(uint8_t *buf, int n)
{
    absolute_time_t until = make_timeout_time_ms(200);
    uint8_t t;
    while ((t = xfer(0xff)) == 0xff)
        if (time_reached(until)) return false;
    if (t != 0xfe) return false;
    memset(buf, 0xff, (size_t)n);
    spi_write_read_blocking(SD_SPI, buf, buf, (size_t)n);
    xfer(0xff); xfer(0xff);             /* CRC, not checked */
    return true;
}

/* bring the card up; false with the reason in why */
bool sd_init(const char **why)
{
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(PIN_SD_CLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SD_MISO);
    gpio_init(PIN_SD_CS);
    gpio_put(PIN_SD_CS, 1);
    gpio_set_dir(PIN_SD_CS, GPIO_OUT);

    gpio_put(PIN_SD_CS, 1);
    for (int i = 0; i < 10; i++) xfer(0xff);    /* 80 clocks with CS high */

    cs(true);
    uint8_t r = 0xff;
    for (int i = 0; i < 10 && r != 0x01; i++) r = cmd(0, 0, 0x95);   /* GO_IDLE_STATE */
    if (r != 0x01) { cs(false); *why = r == 0xff ? "no answer (no card, or wiring)" : "CMD0 refused"; return false; }

    bool v2 = false;
    if (cmd(8, 0x1aa, 0x87) == 0x01) {         /* SEND_IF_COND: SD v2 */
        uint8_t e[4];
        for (int i = 0; i < 4; i++) e[i] = xfer(0xff);
        if (e[3] != 0xaa) { cs(false); *why = "CMD8 echo wrong"; return false; }
        v2 = true;
    }
    absolute_time_t until = make_timeout_time_ms(1500);
    do r = acmd(41, v2 ? 0x40000000 : 0);       /* SD_SEND_OP_COND, HCS */
    while (r == 0x01 && !time_reached(until));
    if (r != 0x00) { cs(false); *why = "ACMD41: card does not get ready"; return false; }

    sd_hc = false;
    if (v2 && cmd(58, 0, 0x01) == 0x00) {       /* READ_OCR */
        uint8_t o[4];
        for (int i = 0; i < 4; i++) o[i] = xfer(0xff);
        sd_hc = (o[0] & 0x40) != 0;
    }
    if (!sd_hc) cmd(16, 512, 0x01);             /* SET_BLOCKLEN for SDSC */
    cs(false);
    spi_set_baudrate(SD_SPI, 20 * 1000 * 1000);
    return true;
}

bool sd_read(uint32_t lba, uint8_t *buf)
{
    cs(true);
    bool ok = cmd(17, sd_hc ? lba : lba * 512u, 0x01) == 0x00 && data_in(buf, 512);
    cs(false);
    return ok;
}

/* wait until the card is no longer busy (DO high) */
static bool sd_ready(uint32_t ms)
{
    absolute_time_t until = make_timeout_time_ms(ms);
    while (xfer(0xff) != 0xff)
        if (time_reached(until)) return false;
    return true;
}

bool sd_write(uint32_t lba, const uint8_t *buf)
{
    cs(true);
    bool ok = cmd(24, sd_hc ? lba : lba * 512u, 0x01) == 0x00;     /* WRITE_BLOCK */
    if (ok) {
        xfer(0xff);
        xfer(0xfe);                                     /* start token */
        spi_write_blocking(SD_SPI, buf, 512);
        xfer(0xff); xfer(0xff);                         /* CRC, not checked */
        ok = (xfer(0xff) & 0x1f) == 0x05 && sd_ready(500);   /* data accepted, then busy */
    }
    cs(false);
    return ok;
}

static bool sd_reg(uint8_t c, uint8_t *reg16)
{
    cs(true);
    bool ok = cmd(c, 0, 0x01) == 0x00 && data_in(reg16, 16);
    cs(false);
    return ok;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* console 'c': what is in the slot */
void sd_console_test(void)
{
    static uint8_t b[512];
    const char *why = "";
    if (!sd_init(&why)) { printf("SD card: %s\n", why); return; }
    printf("SD card: %s, SPI at %lu kHz\n", sd_hc ? "SDHC/SDXC" : "SDSC",
           (unsigned long)(spi_get_baudrate(SD_SPI) / 1000));

    uint8_t r[16];
    if (sd_reg(10, r))                          /* CID */
        printf("  CID: maker 0x%02x, OEM %c%c, name %.5s, rev %u.%u, serial %08lx, made %u-%02u\n",
               r[0], r[1], r[2], (const char *)r + 3, r[8] >> 4, r[8] & 15,
               (unsigned long)((uint32_t)r[9] << 24 | r[10] << 16 | r[11] << 8 | r[12]),
               2000 + ((r[13] & 15) << 4 | r[14] >> 4), r[14] & 15);
    if (sd_reg(9, r)) {                         /* CSD */
        uint64_t bytes;
        if (r[0] >> 6 == 1) {                   /* CSD v2: (C_SIZE + 1) * 512 KB */
            uint32_t c = (uint32_t)(r[7] & 0x3f) << 16 | r[8] << 8 | r[9];
            bytes = (uint64_t)(c + 1) * 512 * 1024;
        } else {                                /* CSD v1 */
            uint32_t c = (uint32_t)(r[6] & 3) << 10 | r[7] << 2 | r[8] >> 6;
            uint32_t mult = (uint32_t)(r[9] & 3) << 1 | r[10] >> 7;
            bytes = (uint64_t)(c + 1) << (mult + 2) << (r[5] & 15);
        }
        printf("  size: %lu MB\n", (unsigned long)(bytes >> 20));
    }

    if (!sd_read(0, b)) { printf("  block 0: read error\n"); return; }
    printf("  block 0: signature %02x %02x\n", b[510], b[511]);
    bool mbr = b[510] == 0x55 && b[511] == 0xaa && !(b[0] == 0xeb || b[0] == 0xe9);
    if (!mbr) {
        printf("  no partition table: %s\n", b[0] == 0xeb || b[0] == 0xe9 ? "a FAT volume without MBR" : "unknown");
        return;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = b + 0x1be + 16 * i;
        if (!e[4]) continue;
        uint32_t start = le32(e + 8), n = le32(e + 12);
        printf("  partition %d: type 0x%02x, start %lu, %lu MB\n", i + 1, e[4],
               (unsigned long)start, (unsigned long)(n / 2048));
        static uint8_t p[512];
        if (sd_read(start, p) && p[510] == 0x55 && p[511] == 0xaa) {
            bool f32 = !memcmp(p + 82, "FAT32", 5);
            printf("    boot sector: OEM '%.8s', %u bytes/sector, %u sectors/cluster, label '%.11s', %.8s\n",
                   p + 3, p[11] | p[12] << 8, p[13], f32 ? p + 71 : p + 43, f32 ? p + 82 : p + 54);
        }
    }
}

/* ---------------- FatFs disk I/O (one drive: the card) ---------------- */

#include "third_party/fatfs/ff.h"
#include "third_party/fatfs/diskio.h"

static DSTATUS sd_stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv)
{
    const char *why;
    if (pdrv) return STA_NOINIT;
    sd_stat = sd_init(&why) ? 0 : STA_NOINIT | STA_NODISK;
    return sd_stat;
}

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : sd_stat; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (sd_stat & STA_NOINIT)) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++)
        if (!sd_read((uint32_t)sector + i, buff + i * 512u)) return RES_ERROR;
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (sd_stat & STA_NOINIT)) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++)
        if (!sd_write((uint32_t)sector + i, buff + i * 512u)) return RES_ERROR;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)buff;
    if (pdrv) return RES_PARERR;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
