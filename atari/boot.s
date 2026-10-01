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
        move.l  #DRVSECT*512,-(sp)
        move.w  #$48,-(sp)              ; Malloc
        trap    #1
        addq.l  #6,sp
        move.l  d0,a4
        tst.l   d0
        ble.s   nogo

        lea     cdb(pc),a0
        move.b  acsi_id(pc),d0
        lsl.b   #5,d0
        or.b    d0,(a0)                 ; id into byte 0
        move.l  a4,a1
        moveq   #DRVSECT,d0
        moveq   #0,d1
        bsr     acsi_cmd
        tst.l   d0
        bne.s   free

        moveq   #0,d0
        move.b  acsi_id(pc),d0
        jsr     (a4)                    ; driver init
        bra.s   bfail

free:   move.l  a4,-(sp)
        move.w  #$49,-(sp)              ; Mfree
        trap    #1
        addq.l  #6,sp
nogo:   lea     m_fail(pc),a0           ; only a failure is shown here,
.put:   moveq   #0,d0                   ; the driver prints the banner
        move.b  (a0)+,d0
        beq.s   .done
        move.l  a0,-(sp)
        move.w  d0,-(sp)
        move.w  #2,-(sp)
        move.w  #3,-(sp)                ; Bconout(CON, c)
        trap    #13
        addq.l  #6,sp
        move.l  (sp)+,a0
        bra.s   .put
.done:  move.l  $4ba.w,d0               ; pause 1 s (hz_200) so it can be read
        add.l   #200,d0
.wait:  cmp.l   $4ba.w,d0
        bhi.s   .wait
bfail:  movem.l (sp)+,d0-d7/a0-a6
        rts
m_fail: dc.b    13,10,"ACSI2TNFS: driver could not be loaded",13,10,0
        even

        include "acsi.inc"

        if      *>$1c2
        fail    "boot code too large"
        endif
