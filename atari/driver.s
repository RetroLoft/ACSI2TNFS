; ACSI2TNFS minimal hard disk driver (position independent, loaded by boot.s)
;
; entry (offset 0): d0 = ACSI id. Called in supervisor mode during dmaboot.
; Mounts every GEM/BGM partition of the root sector as the next free drive
; letter from C: on, and hooks hdv_bpb / hdv_rw / hdv_mediach.

        include "layout.inc"

HDV_BPB     equ $472
HDV_RW      equ $476
HDV_MEDIACH equ $47e
DRVBITS     equ $4c2
BOOTDEV     equ $446
MAXPART     equ 4
CHUNK       equ 64                      ; sectors per ACSI command

        org     0
        bra.w   init

; ---------------------------------------------------------------------------
; resident data
; ---------------------------------------------------------------------------
        even
id:         dc.w    0
npart:      dc.w    0
firstdrv:   dc.w    0
old_bpb:    dc.l    0
old_rw:     dc.l    0
old_mc:     dc.l    0
pstart:     ds.l    MAXPART             ; physical start sector
pshift:     ds.w    MAXPART             ; log2(logical/physical sector size)
bpbs:       ds.w    9*MAXPART           ; TOS BPBs, 18 bytes each
cdb:        ds.b    6
            even
bounce:     ds.b    512

; ---------------------------------------------------------------------------
; d0.w = BIOS drive -> d1.l = partition index, N set when not ours
; ---------------------------------------------------------------------------
find:   move.w  d0,d1
        sub.w   firstdrv(pc),d1
        bmi.s   .no
        cmp.w   npart(pc),d1
        bge.s   .no
        ext.l   d1
        rts
.no:    moveq   #-1,d1
        rts

; LONG hdv_bpb(WORD dev)
my_bpb: move.w  4(sp),d0
        bsr   find
        bmi.s   .old
        mulu    #18,d1
        lea     bpbs(pc),a0
        add.l   d1,a0
        move.l  a0,d0
        rts
.old:   move.l  old_bpb(pc),a0
        jmp     (a0)

; LONG hdv_mediach(WORD dev)
my_mc:  move.w  4(sp),d0
        bsr   find
        bmi.s   .old
        moveq   #0,d0                   ; never changed
        rts
.old:   move.l  old_mc(pc),a0
        jmp     (a0)

; LONG hdv_rw(WORD rw, void *buf, WORD count, WORD recno, WORD dev, LONG lrecno)
my_rw:  move.w  14(sp),d0
        bsr   find
        bpl.s   .mine
        move.l  old_rw(pc),a0
        jmp     (a0)
.mine:  movem.l d3-d7/a3-a6,-(sp)       ; 36 bytes
        move.w  4+36(sp),d7             ; rw flag, bit 0 = write
        move.l  6+36(sp),a3             ; buffer
        moveq   #0,d6
        move.w  10+36(sp),d6            ; count (logical sectors)
        moveq   #0,d5
        move.w  12+36(sp),d5            ; recno
        cmp.w   #-1,d5
        bne.s   .rec
        move.l  16+36(sp),d5            ; lrecno
.rec:   move.w  d1,d0
        add.w   d0,d0
        lea     pshift(pc),a0
        move.w  (a0,d0.w),d4            ; shift
        lsl.l   d4,d5
        lsl.l   d4,d6                   ; now physical sectors
        add.w   d0,d0
        lea     pstart(pc),a0
        add.l   (a0,d0.w),d5            ; physical lba

.loop:  tst.l   d6
        beq     .ok
        move.l  d6,d4                   ; d4 = sectors this round
        cmp.l   #CHUNK,d4
        bls.s   .n
        moveq   #CHUNK,d4
.n:     move.l  a3,d0
        btst    #0,d0
        beq.s   .direct
        moveq   #1,d4                   ; odd buffer: one sector via bounce
        lea     bounce(pc),a1
        btst    #0,d7
        beq.s   .go
        move.l  a3,a0                   ; write: copy into bounce first
        move.w  #511,d0
.cpw:   move.b  (a0)+,(a1)+
        dbra    d0,.cpw
        lea     bounce(pc),a1
        bra.s   .go
.direct: move.l a3,a1
.go:    lea     cdb(pc),a0
        move.w  id(pc),d0
        lsl.b   #5,d0
        moveq   #$08,d1                 ; READ(6)
        btst    #0,d7
        beq.s   .rd
        moveq   #$0a,d1                 ; WRITE(6)
.rd:    or.b    d1,d0
        move.b  d0,(a0)
        move.l  d5,d0
        swap    d0
        andi.b  #$1f,d0
        move.b  d0,1(a0)
        move.w  d5,d0
        lsr.w   #8,d0
        move.b  d0,2(a0)
        move.b  d5,3(a0)
        move.b  d4,4(a0)
        clr.b   5(a0)
        move.l  a1,-(sp)
        move.w  d4,d0
        moveq   #0,d1
        btst    #0,d7
        sne     d1
        andi.w  #1,d1
        bsr     acsi_cmd
        move.l  (sp)+,a1
        tst.l   d0
        bne.s   .err
        move.l  a3,d0                   ; odd read: copy bounce out
        btst    #0,d0
        beq.s   .adv
        btst    #0,d7
        bne.s   .adv
        move.l  a3,a0
        move.w  #511,d0
.cpr:   move.b  (a1)+,(a0)+
        dbra    d0,.cpr
.adv:   add.l   d4,d5
        sub.l   d4,d6
        move.l  d4,d0
        lsl.l   #8,d0
        add.l   d0,d0
        add.l   d0,a3                   ; buf += n * 512
        bra     .loop
.ok:    moveq   #0,d0
        bra.s   .out
.err:   moveq   #-11,d0                 ; E_READF
        btst    #0,d7
        beq.s   .out
        moveq   #-10,d0                 ; E_WRITF
.out:   movem.l (sp)+,d3-d7/a3-a6
        rts

        include "acsi.inc"

; ===========================================================================
; init (not needed after boot, but kept simple: everything stays resident)
; ===========================================================================
; read one physical sector d0.l into bounce. Returns d0 = status.
rdsec:  lea     cdb(pc),a0
        move.w  id(pc),d1
        lsl.b   #5,d1
        ori.b   #$08,d1
        move.b  d1,(a0)
        move.l  d0,d1
        swap    d1
        andi.b  #$1f,d1
        move.b  d1,1(a0)
        move.w  d0,d1
        lsr.w   #8,d1
        move.b  d1,2(a0)
        move.b  d0,3(a0)
        move.b  #1,4(a0)
        clr.b   5(a0)
        lea     bounce(pc),a1
        moveq   #1,d0
        moveq   #0,d1
        bra     acsi_cmd

; little endian word at (a0,d0) -> d1.l
le16:   moveq   #0,d1
        move.b  1(a0,d0.w),d1
        lsl.w   #8,d1
        move.b  (a0,d0.w),d1
        rts

; print a0 via BIOS Bconout: GEMDOS console calls are not usable during
; dmaboot (no process with standard handles yet)
print:  movem.l d0-d2/a0-a3,-(sp)
        move.l  a0,a3
.nx:    moveq   #0,d0
        move.b  (a3)+,d0
        beq.s   .end
        move.w  d0,-(sp)
        move.w  #2,-(sp)                ; CON
        move.w  #3,-(sp)                ; Bconout
        trap    #13
        addq.l  #6,sp
        bra.s   .nx
.end:   movem.l (sp)+,d0-d2/a0-a3
        rts

init:   movem.l d0-d7/a0-a6,-(sp)
        lea     id(pc),a0
        move.w  d0,(a0)
        lea     msg_hello(pc),a0
        bsr   print

        moveq   #0,d0
        bsr     rdsec
        tst.l   d0
        bne     .fail

        ; first free drive letter from C:
        move.l  DRVBITS.w,d0
        moveq   #2,d1
.free:  btst    d1,d0
        beq.s   .gotd
        addq.w  #1,d1
        cmp.w   #26,d1
        blt.s   .free
        bra     .fail
.gotd:  lea     firstdrv(pc),a0
        move.w  d1,(a0)

        ; collect partitions ($1c6: flag, "GEM"/"BGM", start.l, size.l)
        moveq   #0,d7                   ; partition entry
        moveq   #0,d6                   ; partitions found
        lea     pstart(pc),a5
.pscan: lea     bounce+$1c6(pc),a0
        move.w  d7,d0
        mulu    #12,d0
        add.w   d0,a0
        btst    #0,(a0)
        beq.s   .pnext
        move.l  (a0),d0
        andi.l  #$00ffffff,d0
        cmp.l   #$0047454d,d0           ; "GEM"
        beq.s   .pok
        cmp.l   #$0042474d,d0           ; "BGM"
        bne.s   .pnext
.pok:   move.w  d6,d0
        lsl.w   #2,d0
        move.l  4(a0),(a5,d0.w)
        addq.w  #1,d6
.pnext: addq.w  #1,d7
        cmp.w   #MAXPART,d7
        blt.s   .pscan
        lea     npart(pc),a0
        move.w  d6,(a0)
        beq     .fail

        ; build a BPB per partition from its FAT boot sector
        moveq   #0,d7
.bpb:   move.w  d7,d0
        lsl.w   #2,d0
        move.l  (a5,d0.w),d0
        bsr     rdsec
        tst.l   d0
        bne     .fail
        lea     bounce(pc),a0
        lea     bpbs(pc),a1
        move.w  d7,d0
        mulu    #18,d0
        add.w   d0,a1

        moveq   #$0b,d0
        bsr   le16
        move.w  d1,d2                   ; bytes per sector
        move.w  d1,(a1)                 ; recsiz
        moveq   #0,d3
        move.b  $0d(a0),d3              ; sectors per cluster
        move.w  d3,2(a1)                ; clsiz
        move.w  d2,d0
        mulu    d3,d0
        move.w  d0,4(a1)                ; clsizb
        moveq   #$11,d0
        bsr   le16                    ; root entries
        lsl.l   #5,d1
        divu    d2,d1
        move.w  d1,6(a1)                ; rdlen
        moveq   #$16,d0
        bsr   le16
        move.w  d1,8(a1)                ; fsiz
        move.w  d1,d4
        moveq   #$0e,d0
        bsr   le16                    ; reserved sectors
        move.w  d1,d5
        add.w   d4,d1
        move.w  d1,10(a1)               ; fatrec = 2nd FAT
        moveq   #0,d0
        move.b  $10(a0),d0              ; number of FATs
        mulu    d4,d0
        add.w   d5,d0
        add.w   6(a1),d0
        move.w  d0,12(a1)               ; datrec
        move.w  d0,d5
        moveq   #$13,d0
        bsr     le16                    ; total logical sectors
        sub.w   d5,d1
        divu    d3,d1
        move.w  d1,14(a1)               ; numcl
        move.w  #1,16(a1)               ; bflags: 16-bit FAT

        moveq   #0,d0                   ; shift = log2(bps/512)
        lsr.w   #8,d2
        lsr.w   #1,d2
.sh:    lsr.w   #1,d2
        beq.s   .shd
        addq.w  #1,d0
        bra.s   .sh
.shd:   lea     pshift(pc),a0
        move.w  d7,d1
        add.w   d1,d1
        move.w  d0,(a0,d1.w)

        addq.w  #1,d7
        cmp.w   npart(pc),d7
        blt     .bpb

        ; drive bits and vectors
        move.w  firstdrv(pc),d1
        move.w  npart(pc),d2
        subq.w  #1,d2
        move.l  DRVBITS.w,d0
.bits:  bset    d1,d0
        addq.w  #1,d1
        dbra    d2,.bits
        move.l  d0,DRVBITS.w

        ; boot from the first hard disk partition like AHDI does: TOS runs
        ; the AUTO folder and reads DESKTOP.INF from _bootdev (else A:)
        move.w  firstdrv(pc),BOOTDEV.w

        lea     old_bpb(pc),a0
        move.l  HDV_BPB.w,(a0)
        lea     my_bpb(pc),a1
        move.l  a1,HDV_BPB.w
        lea     old_rw(pc),a0
        move.l  HDV_RW.w,(a0)
        lea     my_rw(pc),a1
        move.l  a1,HDV_RW.w
        lea     old_mc(pc),a0
        move.l  HDV_MEDIACH.w,(a0)
        lea     my_mc(pc),a1
        move.l  a1,HDV_MEDIACH.w

        lea     msg_ok(pc),a0
        move.w  firstdrv(pc),d0
        add.b   #'A',d0
        move.b  d0,msg_drv-msg_ok(a0)
        bsr     print
        movem.l (sp)+,d0-d7/a0-a6
        rts

.fail:  lea     msg_fail(pc),a0
        bsr     print
        movem.l (sp)+,d0-d7/a0-a6
        rts

msg_hello:  dc.b    13,10,27,"p ACSI2TNFS flash disk driver 0.2 ",27,"q",13,10,0
msg_ok:     dc.b    " mounted as drive "
msg_drv:    dc.b    "C:",13,10,0
msg_fail:   dc.b    " disk not usable - driver not installed",13,10,0
            even

        if      *>DRVSECT*512
        fail    "driver too large"
        endif
