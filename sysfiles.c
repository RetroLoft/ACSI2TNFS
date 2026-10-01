/*
 * ACSI2TNFS - built-in files on the flash disk C:
 *
 * The built-in disk image (disk_seed.h, made by atari/mkdisk.py) holds the
 * system files: README.TXT and ACSITNFS.PRG, stored read-only. The user's own
 * files (DESKTOP.INF, an AUTO folder, ...) live next to them on the same FAT.
 * sysfiles_sync() brings the system files on C: in line with the image
 * without touching anything else: a changed file (new firmware) is replaced
 * in free clusters, and with restore_missing also a deleted one is put back.
 *
 * Runs on core0 while the Atari cannot use C: (at start-up, or with the
 * adapter muted until the next Atari reset): GEMDOS caches the FAT.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "acsi.h"

#define ATTR_RO     0x01
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10

/* ---------------- flash access with a one-block write cache ---------------- */

static uint8_t  wc_buf[FLASH_SECTOR_SIZE];     /* RAM: XIP is off while programming */
static int32_t  wc_blk = -1;
static bool     wc_dirty, wc_failed;

static void wc_commit_cb(void *p)
{
    (void)p;
    uint32_t off = DISK_FLASH_OFFSET + (uint32_t)wc_blk * FLASH_SECTOR_SIZE;
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, wc_buf, FLASH_SECTOR_SIZE);
}

static void wc_flush(void)
{
    if (wc_blk < 0 || !wc_dirty) return;
    wc_dirty = false;
    if (flash_safe_execute(wc_commit_cb, NULL, 1000) != PICO_OK) wc_failed = true;
}

static const uint8_t *flash_sector(uint32_t lba)
{
    if ((int32_t)(lba / 8u) == wc_blk) return wc_buf + (lba % 8u) * 512u;
    return (const uint8_t *)(XIP_BASE + DISK_FLASH_OFFSET + lba * 512u);
}

static uint8_t *flash_sector_rw(uint32_t lba)
{
    uint32_t blk = lba / 8u;
    if ((int32_t)blk != wc_blk) {
        wc_flush();
        memcpy(wc_buf, (const void *)(XIP_BASE + DISK_FLASH_OFFSET + blk * FLASH_SECTOR_SIZE),
               FLASH_SECTOR_SIZE);
        wc_blk = (int32_t)blk;
    }
    wc_dirty = true;
    return wc_buf + (lba % 8u) * 512u;
}

/* ---------------- a FAT volume: the flash disk or the built-in image ---------------- */

typedef struct {
    const uint8_t *img;         /* built-in image, or NULL for the flash disk */
    uint32_t part;              /* partition start (sectors) */
    uint32_t res, nfat, spf, rdsec, rdlen, datrec, spc, ncl;
    bool     fat16;
} vol_t;

static const uint8_t *vsec(const vol_t *v, uint32_t rel)
{
    uint32_t lba = v->part + rel;
    return v->img ? v->img + lba * 512u : flash_sector(lba);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool vol_open(vol_t *v, const uint8_t *img)
{
    v->img = img;
    const uint8_t *root = img ? img : flash_sector(0);
    const uint8_t *pe = root + 0x1c6;                    /* partition 0: our C: */
    v->part = ((uint32_t)pe[4] << 24) | (pe[5] << 16) | (pe[6] << 8) | pe[7];
    const uint8_t *b = vsec(v, 0);
    if (le16(b + 11) != 512 || b[13] == 0) return false;
    v->spc = b[13];
    v->res = le16(b + 14);
    v->nfat = b[16];
    v->rdlen = le16(b + 17) * 32u / 512u;
    uint32_t tot = le16(b + 19);
    v->spf = le16(b + 22);
    v->rdsec = v->res + v->nfat * v->spf;
    v->datrec = v->rdsec + v->rdlen;
    v->ncl = (tot - v->datrec) / v->spc;
    v->fat16 = v->ncl >= 4085;                            /* as the driver decides */
    return true;
}

static uint32_t fat_get(const vol_t *v, uint32_t c)
{
    uint32_t off = v->fat16 ? c * 2 : c * 3 / 2;
    uint8_t b0 = vsec(v, v->res + off / 512)[off % 512];
    uint8_t b1 = vsec(v, v->res + (off + 1) / 512)[(off + 1) % 512];
    uint32_t x = b0 | (b1 << 8);
    if (v->fat16) return x;
    return (c & 1) ? x >> 4 : x & 0xfff;
}

static bool fat_eoc(const vol_t *v, uint32_t c) { return c >= (v->fat16 ? 0xfff8u : 0xff8u) || c < 2; }

/* flash disk only: every FAT copy */
static void fat_set(const vol_t *v, uint32_t c, uint32_t val)
{
    for (uint32_t k = 0; k < v->nfat; k++) {
        uint32_t base = v->part + v->res + k * v->spf;
        uint32_t off = v->fat16 ? c * 2 : c * 3 / 2;
        uint8_t *p0 = flash_sector_rw(base + off / 512);
        uint8_t lo = p0[off % 512];
        uint8_t *p1 = flash_sector_rw(base + (off + 1) / 512);
        uint8_t hi = p1[(off + 1) % 512];
        uint32_t x = lo | (hi << 8);
        if (v->fat16)            x = val & 0xffff;
        else if (c & 1)          x = (x & 0x000f) | ((val & 0xfff) << 4);
        else                     x = (x & 0xf000) | (val & 0xfff);
        flash_sector_rw(base + off / 512)[off % 512] = (uint8_t)x;
        flash_sector_rw(base + (off + 1) / 512)[(off + 1) % 512] = (uint8_t)(x >> 8);
    }
}

/* directory entry i of the root directory */
static const uint8_t *dirent(const vol_t *v, uint32_t i)
{
    return vsec(v, v->rdsec + i / 16) + (i % 16) * 32;
}

static uint32_t clbytes(const vol_t *v) { return v->spc * 512u; }

/* does the file starting at cluster c1 on a equal the one at c2 on b? */
static bool same_content(const vol_t *a, uint32_t c1, const vol_t *b, uint32_t c2, uint32_t size)
{
    while (size) {
        if (fat_eoc(a, c1) || fat_eoc(b, c2)) return false;
        uint32_t n = size < clbytes(a) ? size : clbytes(a);
        for (uint32_t s = 0; s * 512 < n; s++) {
            uint32_t len = n - s * 512 < 512 ? n - s * 512 : 512;
            if (memcmp(vsec(a, a->datrec + (c1 - 2) * a->spc + s),
                       vsec(b, b->datrec + (c2 - 2) * b->spc + s), len)) return false;
        }
        size -= n;
        c1 = fat_get(a, c1);
        c2 = fat_get(b, c2);
    }
    return true;
}

static uint32_t chain_len(const vol_t *v, uint32_t c)
{
    uint32_t n = 0;
    while (!fat_eoc(v, c) && n <= v->ncl) { n++; c = fat_get(v, c); }
    return n;
}

static uint32_t free_clusters(const vol_t *v)
{
    uint32_t n = 0;
    for (uint32_t c = 2; c < v->ncl + 2; c++) if (fat_get(v, c) == 0) n++;
    return n;
}

/* write file `se` (entry of the image) to the flash disk, into entry slot `slot` */
static bool put_file(const vol_t *fl, const vol_t *im, const uint8_t *se, uint32_t slot)
{
    uint32_t size = se[28] | (se[29] << 8) | (se[30] << 16) | ((uint32_t)se[31] << 24);
    uint32_t need = (size + clbytes(fl) - 1) / clbytes(fl);
    uint32_t src = le16(se + 26), first = 0, prev = 0, c = 2;
    for (uint32_t k = 0; k < need; k++) {
        while (c < fl->ncl + 2 && fat_get(fl, c) != 0) c++;
        if (c >= fl->ncl + 2) return false;                 /* counted before: cannot happen */
        for (uint32_t s = 0; s < fl->spc; s++)
            memcpy(flash_sector_rw(fl->part + fl->datrec + (c - 2) * fl->spc + s),
                   vsec(im, im->datrec + (src - 2) * im->spc + s), 512);
        fat_set(fl, c, fl->fat16 ? 0xffff : 0xfff);         /* taken; linked below */
        if (prev) fat_set(fl, prev, c); else first = c;
        prev = c;
        src = fat_get(im, src);
    }
    uint8_t *de = flash_sector_rw(fl->part + fl->rdsec + slot / 16) + (slot % 16) * 32;
    memcpy(de, se, 32);
    de[11] |= ATTR_RO;                                      /* system file: read-only */
    de[26] = (uint8_t)first;
    de[27] = (uint8_t)(first >> 8);
    return true;
}

/* returns the number of files written, -1 on an error */
int sysfiles_sync(const uint8_t *image, bool restore_missing)
{
    vol_t im, fl;
    if (!vol_open(&im, image) || !vol_open(&fl, NULL)) return -1;
    if (im.part != fl.part || im.spc != fl.spc || im.rdsec != fl.rdsec ||
        im.datrec != fl.datrec || im.ncl != fl.ncl) return -1;  /* other layout */

    int written = 0;
    wc_failed = false;
    for (uint32_t i = 0; i < im.rdlen * 16; i++) {
        const uint8_t *se = dirent(&im, i);
        if (se[0] == 0) break;
        if (se[0] == 0xe5 || (se[11] & (ATTR_VOLUME | ATTR_DIR))) continue;
        uint32_t size = se[28] | (se[29] << 8) | (se[30] << 16) | ((uint32_t)se[31] << 24);

        /* the same name on C: */
        int32_t slot = -1, freeslot = -1;
        for (uint32_t j = 0; j < fl.rdlen * 16; j++) {
            const uint8_t *fe = dirent(&fl, j);
            if (fe[0] == 0 || fe[0] == 0xe5) { if (freeslot < 0) freeslot = (int32_t)j; if (fe[0] == 0) break; continue; }
            if (!(fe[11] & ATTR_VOLUME) && !memcmp(fe, se, 11)) { slot = (int32_t)j; break; }
        }
        char name[13];
        snprintf(name, sizeof name, "%.8s.%.3s", (const char *)se, (const char *)se + 8);

        if (slot >= 0) {
            const uint8_t *fe = dirent(&fl, (uint32_t)slot);
            uint32_t fsize = fe[28] | (fe[29] << 8) | (fe[30] << 16) | ((uint32_t)fe[31] << 24);
            if (fsize == size && same_content(&fl, le16(fe + 26), &im, le16(se + 26), size)) {
                if (!(fe[11] & ATTR_RO))                    /* up to date: keep it read-only */
                    flash_sector_rw(fl.part + fl.rdsec + (uint32_t)slot / 16)[((uint32_t)slot % 16) * 32 + 11] |= ATTR_RO;
                continue;
            }
            /* changed (new firmware): enough room once the old version is gone? */
            uint32_t old = chain_len(&fl, le16(fe + 26));
            if (free_clusters(&fl) + old < (size + clbytes(&fl) - 1) / clbytes(&fl)) {
                printf("sysfiles: no room on C: for %s\n", name);
                continue;
            }
            for (uint32_t c = le16(fe + 26), n = 0; !fat_eoc(&fl, c) && n < old; n++) {
                uint32_t next = fat_get(&fl, c);
                fat_set(&fl, c, 0);
                c = next;
            }
        } else {
            if (!restore_missing) continue;                 /* deleted on purpose */
            if (freeslot < 0 || free_clusters(&fl) < (size + clbytes(&fl) - 1) / clbytes(&fl)) {
                printf("sysfiles: no room on C: for %s\n", name);
                continue;
            }
            slot = freeslot;
        }
        if (!put_file(&fl, &im, se, (uint32_t)slot)) return -1;
        printf("sysfiles: %s %s\n", name, restore_missing && slot == freeslot ? "restored" : "updated");
        written++;
    }
    wc_flush();
    wc_blk = -1;
    return wc_failed ? -1 : written;
}
