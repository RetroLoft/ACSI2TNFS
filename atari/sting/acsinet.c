/*
 * ACSI transport for ACSI_NET.STX, after atari/acsi.inc (Atari ACSI/DMA
 * Integration Guide, chapter 4). Runs in supervisor mode: from the STinG
 * thread (200 Hz timer, interrupts at the level of the interrupted code)
 * or through Supexec.
 *
 * The DMA chip is shared with the floppy and every hard disk driver. Its
 * owner holds flock ($43e). We take it with TAS; when the STinG thread
 * finds it taken (a disk command is in progress underneath us) we do
 * nothing this time, as EtherNEA does.
 */
#include "acsinet.h"

#define DMADATA   ((volatile unsigned short *)0xffff8604L)
#define DMACTRL   ((volatile unsigned short *)0xffff8606L)
#define DMAHI     ((volatile unsigned char *)0xffff8609L)
#define DMAMID    ((volatile unsigned char *)0xffff860bL)
#define DMALO     ((volatile unsigned char *)0xffff860dL)
#define GPIP      ((volatile unsigned char *)0xfffffa01L)
#define FLOCK     ((volatile short *)0x43eL)
#define HZ200     (*(volatile unsigned long *)0x4baL)

static int acsi_id = -1;

static int take_flock(int wait)
{
    unsigned long until = HZ200 + 200;          /* 1 s */
    for (;;) {
        char got;
        __asm__ volatile ("tas 0x43e.w\n\tseq %0" : "=d"(got) : : "cc", "memory");
        if (got) return 1;
        if (!wait || HZ200 > until) return 0;
    }
}

static int wait_irq(unsigned long ticks)
{
    unsigned long until = HZ200 + ticks;
    while (*GPIP & 0x20)
        if (HZ200 > until) return 0;
    return 1;
}

static long command(int id, const unsigned char *cdb, void *buf, int sectors, int write, int wait)
{
    unsigned long a = (unsigned long)buf;
    unsigned short dir = write ? 0x100 : 0x000;
    long r = ST_TIMEOUT;
    int i;

    (void)id;
    if (!take_flock(wait)) return ST_LOCKED;
    *DMALO = (unsigned char)a;
    *DMAMID = (unsigned char)(a >> 8);
    *DMAHI = (unsigned char)(a >> 16);
    *DMACTRL = (0x090 | dir) ^ 0x100;           /* toggle R/W: reset DMA, flush FIFO */
    *DMACTRL = 0x090 | dir;                     /* sector count register */
    *DMADATA = (unsigned short)sectors;
    *DMACTRL = 0x088 | dir;                     /* A1 = 0: first byte */
    for (i = 0; i < 5; i++) {
        *(volatile unsigned long *)DMADATA = (unsigned long)cdb[i] << 16 | (0x08a | dir);
        if (!wait_irq(4)) goto out;             /* 20 ms: the Pico answers in us */
    }
    *(volatile unsigned long *)DMADATA = (unsigned long)cdb[5] << 16 | dir;  /* start DMA */
    if (!wait_irq(20)) goto out;                /* 100 ms */
    *DMACTRL = 0x08a | dir;
    r = *DMADATA & 0xff;
out:
    *DMACTRL = 0x080;                           /* back to the FDC, as TOS leaves it */
    *FLOCK = 0;
    return r;
}

long acsi_net(unsigned char sub, unsigned char arg, void *buf, int sectors, int write, int wait)
{
    unsigned char cdb[6];
    if (acsi_id < 0) return ST_TIMEOUT;
    cdb[0] = (unsigned char)(0x11 | acsi_id << 5);
    cdb[1] = 'A'; cdb[2] = 'T'; cdb[3] = sub; cdb[4] = arg; cdb[5] = 0;
    return command(acsi_id, cdb, buf, sectors, write, wait);
}

int acsi_find(void)
{
    static unsigned char probe[512];
    int id;
    for (id = 0; id < 8; id++) {
        unsigned char cdb[6];
        cdb[0] = (unsigned char)(0x11 | id << 5);
        cdb[1] = 'A'; cdb[2] = 'T'; cdb[3] = 8; cdb[4] = 0; cdb[5] = 0;
        probe[0] = 0;
        if (command(id, cdb, probe, 1, 0, 1) == 0 &&
            probe[0] == 'A' && probe[1] == 'T' && probe[2] == 'L')
            return acsi_id = id;
    }
    return -1;
}
