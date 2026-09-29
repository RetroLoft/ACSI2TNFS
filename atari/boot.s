; ACSI2TNFS root sector (LBA 0) boot code.
; TOS dmaboot loads this sector and calls it when the word sum is $1234.
; The Pico patches the ACSI id at offset 2 and fixes the checksum.
; Loads the driver (LBA 1..DRVSECT) into Malloc'd memory and calls it
; with d0 = ACSI id.

        include "layout.inc"

        org     0
        bra.s   start
acsi_id: dc.b   0                       ; offset 2, patched by the Pico
        dc.b    0
cdb:    dc.b    $08,0,0,1,DRVSECT,0     ; READ(6) lba 1, DRVSECT sectors

start:  movem.l d0-d7/a0-a6,-(sp)
        lea     m_boot(pc),a0
        bsr     puts
        move.l  #DRVSECT*512,-(sp)
        move.w  #$48,-(sp)              ; Malloc
        trap    #1
        addq.l  #6,sp
        move.l  d0,a4
        moveq   #'M',d1
        moveq   #8,d2
        bsr     phex
        move.l  a4,d0
        ble.s   bfail

        lea     cdb(pc),a0
        move.b  acsi_id(pc),d0
        lsl.b   #5,d0
        or.b    d0,(a0)                 ; id into byte 0
        move.l  a4,a1
        moveq   #DRVSECT,d0
        moveq   #0,d1
        bsr     acsi_cmd
        move.l  d0,d3
        moveq   #'R',d1
        moveq   #2,d2
        bsr     phex
        tst.l   d3
        bne.s   free
        moveq   #'J',d0
        bsr     putc

        moveq   #0,d0
        move.b  acsi_id(pc),d0
        jsr     (a4)                    ; driver init
        bra.s   bfail

free:   move.l  a4,-(sp)
        move.w  #$49,-(sp)              ; Mfree
        trap    #1
        addq.l  #6,sp
bfail:   movem.l (sp)+,d0-d7/a0-a6
        rts

; d1 = tag char, d0 = value, d2 = digits
phex:   move.l  d0,d4
        move.l  d1,d0
        bsr.s   putc
        moveq   #8,d5
        sub.w   d2,d5
        lsl.w   #2,d5
        lsl.l   d5,d4
        subq.w  #1,d2
.d:     rol.l   #4,d4
        move.w  d4,d0
        andi.w  #15,d0
        move.b  hexd(pc,d0.w),d0
        bsr.s   putc
        dbra    d2,.d
        moveq   #' ',d0
        bra.s   putc
hexd:   dc.b    "0123456789ABCDEF"
puts:   move.b  (a0)+,d0
        beq.s   .e
        move.l  a0,-(sp)
        bsr.s   putc
        move.l  (sp)+,a0
        bra.s   puts
.e:     rts
putc:   movem.l d0-d2/a0-a2,-(sp)
        andi.w  #$ff,d0
        move.w  d0,-(sp)
        move.w  #2,-(sp)
        move.w  #3,-(sp)                ; Bconout(CON, c)
        trap    #13
        addq.l  #6,sp
        movem.l (sp)+,d0-d2/a0-a2
        rts
m_boot: dc.b    13,10,"ACSI2TNFS boot ",0
        even

        include "acsi.inc"

        if      *>$1c2
        fail    "boot code too large"
        endif
