; ---------------------------------------------------------------------------
; ACSI command with optional DMA, following the Atari ACSI/DMA Integration
; Guide (1991), chapter 4. Must run in supervisor mode.
;
; in:  a0 = 6-byte command block (byte 0 already contains id<<5)
;      a1 = DMA buffer (even, ST-RAM)
;      d0.w = DMA sector count (512-byte units, >= 1)
;      d1.w = 0: device -> Atari (read), 1: Atari -> device (write)
; out: d0.l = status byte (0 = ok), -1 = timeout
; destroys d1-d2/a0-a2
; ---------------------------------------------------------------------------
DMADATA     equ $ffff8604
DMACTRL     equ $ffff8606
DMAHI       equ $ffff8609
DMAMID      equ $ffff860b
DMALO       equ $ffff860d
GPIP        equ $fffffa01
FLOCK       equ $43e
HZ200       equ $4ba

acsi_cmd:
        movem.l d3-d4,-(sp)
        st      FLOCK.w                 ; keep TOS off the DMA chip
        lea     DMADATA.w,a2
        move.l  a1,d2                   ; DMA address: low, mid, high
        move.b  d2,DMALO.w
        lsr.l   #8,d2
        move.b  d2,DMAMID.w
        lsr.l   #8,d2
        move.b  d2,DMAHI.w

        move.w  #$090,d3                ; read
        tst.w   d1
        beq.s   .dir
        move.w  #$190,d3                ; write
.dir:   move.w  d3,d4
        eori.w  #$100,d4
        move.w  d4,2(a2)                ; toggle R/W = reset DMA, flush FIFO
        move.w  d3,2(a2)                ; select sector count register
        move.w  d0,(a2)                 ; sector count

        andi.w  #$100,d3                ; d3 = direction bit only
        move.w  d3,d4
        ori.w   #$88,d4
        move.w  d4,2(a2)                ; A1 = 0: first command byte
        move.w  d3,d4
        ori.w   #$8a,d4                 ; A1 = 1 for everything after

        moveq   #4,d2                   ; bytes 0..4: wait for /IRQ each
.cmd:   moveq   #0,d1
        move.b  (a0)+,d1
        swap    d1
        move.w  d4,d1
        move.l  d1,(a2)                 ; byte + mode in one access (DMA anomaly)
        moveq   #20,d1                  ; 100 ms
        bsr.s   wait_irq
        bmi.s   .tmo
        dbra    d2,.cmd

        moveq   #0,d1                   ; last byte, then start the DMA
        move.b  (a0)+,d1
        swap    d1
        move.w  d3,d1                   ; $0000 read / $0100 write
        move.l  d1,(a2)
        move.w  #2000,d1                ; 10 s (flash erase on writes)
        bsr.s   wait_irq
        bmi.s   .tmo

        move.w  d4,2(a2)                ; A1 = 1, read status byte
        move.w  (a2),d0
        andi.l  #$ff,d0
        bra.s   .out
.tmo:   moveq   #-1,d0
.out:   move.w  #$80,2(a2)              ; back to FDC status (as TOS leaves it)
        sf      FLOCK.w
        movem.l (sp)+,d3-d4
        rts

; d1.l = timeout in 200 Hz ticks. Returns N set on timeout.
wait_irq:
        add.l   HZ200.w,d1
.w:     btst    #5,GPIP.w
        beq.s   .ok
        cmp.l   HZ200.w,d1
        bhi.s   .w
        moveq   #-1,d1
        rts
.ok:    moveq   #0,d1
        rts
