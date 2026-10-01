; ACSI2TNFS minimal hard disk driver (position independent, loaded by boot.s)
;
; entry (offset 0): d0 = ACSI id. Called in supervisor mode during dmaboot.
; Mounts every GEM/BGM partition of the root sector as the next free drive
; letter from C: on, and hooks hdv_bpb / hdv_rw / hdv_mediach.

        include "layout.inc"
        include "version.inc"           ; VERSION, from ../version.txt

HDV_BPB     equ $472
HDV_RW      equ $476
HDV_MEDIACH equ $47e
DRVBITS     equ $4c2
BOOTDEV     equ $446
RESVALID    equ $426
RESVECTOR   equ $42a
HZ_200      equ $4ba
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
skipclk:    dc.w    0                   ; no clock this boot (ESC / time-out)
gens:       ds.b    MAXPART             ; media change counters last seen
pchg:       ds.b    MAXPART             ; a change not yet reported to GEMDOS
old_bpb:    dc.l    0
old_rw:     dc.l    0
old_mc:     dc.l    0
old_resvalid: dc.l   0
old_resvec: dc.l    0
pstart:     ds.l    MAXPART             ; physical start sector
pshift:     ds.w    MAXPART             ; log2(logical/physical sector size)
bpbs:       ds.w    9*MAXPART           ; TOS BPBs, 18 bytes each
pdrv:       ds.w    MAXPART             ; BIOS drive number per partition
cdb:        ds.b    6
            even
bounce:     ds.b    512

; ---------------------------------------------------------------------------
; d0.w = BIOS drive -> d1.l = partition index, N set when not ours
; ---------------------------------------------------------------------------
find:   moveq   #0,d1
        lea     pdrv(pc),a1
.f:     cmp.w   npart(pc),d1
        bge.s   .no
        move.w  d1,d2
        add.w   d2,d2
        cmp.w   (a1,d2.w),d0
        beq.s   .yes
        addq.w  #1,d1
        bra.s   .f
.yes:   ext.l   d1
        rts
.no:    moveq   #-1,d1
        rts

; LONG hdv_bpb(WORD dev)
my_bpb: move.w  4(sp),d0
        bsr   find
        bmi.s   .old
        lea     pchg(pc),a0             ; GEMDOS logs the drive in again:
        clr.b   (a0,d1.w)               ; a media change has been taken in
        mulu    #18,d1
        lea     bpbs(pc),a0
        add.l   d1,a0
        move.l  a0,d0
        rts
.old:   move.l  old_bpb(pc),a0
        jmp     (a0)

; LONG hdv_mediach(WORD dev)
; The adapter counts changes it made to a partition under the Atari (C:
; rewritten or its system files restored, a TNFS drive rescanned). Asked on
; every call (about 0.5 ms). A new count is reported as "changed" until
; GEMDOS fetches the BPB again: on "changed" it drops the drive's buffers
; (also dirty ones, so a stale FAT is never written), logs the drive in
; again and restarts the call. It only acts on it at a buffer access, so
; the flag must not be cleared on the first call.
my_mc:  move.w  4(sp),d0
        bsr   find
        bmi.s   .old
        move.w  d1,-(sp)                ; partition index
        bsr     query_gen
        move.w  (sp)+,d1
        lea     pchg(pc),a0
        tst.b   (a0,d1.w)
        beq.s   .same
        moveq   #2,d0                   ; media changed, until GEMDOS asks
        rts                             ; for the BPB again (my_bpb)
.same:  moveq   #0,d0                   ; not changed
        rts
.old:   move.l  old_mc(pc),a0
        jmp     (a0)

; vendor sub 10: "ATG", count, one counter per partition entry -> pchg
query_gen:
        movem.l d2-d4/a2,-(sp)
        lea     cdb(pc),a0
        move.w  id(pc),d1
        lsl.b   #5,d1
        ori.b   #$11,d1
        move.b  d1,(a0)
        move.b  #'A',1(a0)
        move.b  #'T',2(a0)
        move.b  #10,3(a0)
        clr.b   4(a0)
        clr.b   5(a0)
        lea     bounce(pc),a1
        clr.l   (a1)
        moveq   #1,d0
        moveq   #0,d1
        bsr     acsi_cmd
        lea     bounce(pc),a2
        move.l  (a2),d0
        clr.b   d0
        cmp.l   #$41544700,d0           ; "ATG": older firmware never changes
        bne.s   .q
        lea     gens(pc),a0
        lea     pchg(pc),a1
        moveq   #0,d2
.g:     move.b  4(a2,d2.w),d0
        cmp.b   (a0,d2.w),d0
        beq.s   .gs
        move.b  d0,(a0,d2.w)
        st      (a1,d2.w)
.gs:    addq.w  #1,d2
        cmp.w   #MAXPART,d2
        blt.s   .g
.q:     movem.l (sp)+,d2-d4/a2
        rts

; resvector: TOS calls this early in every warm reset, before GEMDOS starts.
; _bootdev still says C: from this session and GEMDOS would take it as its
; current drive even when the adapter is hidden (no driver, no C:) on the
; next boot. Put A: back, as after a cold boot, then hand the vector back to
; whoever had it before us and continue at a6 (TOS re-tests resvalid there).
; No stack is available here.
my_reset:
        clr.w   BOOTDEV.w
        move.l  old_resvalid(pc),RESVALID.w
        move.l  old_resvec(pc),RESVECTOR.w
        jmp     (a6)

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

; ---------------------------------------------------------------------------
; Network time: the adapter keeps the time (NTP) and hands it out with vendor
; sub 9: "ATC", state (0 valid, 1 Wi-Fi, 2 none, 3 off, 4 time server), year.w,
; month, day, hour, minute, second. While the adapter is still connecting we
; wait up to 30 s with a countdown (Esc skips), in two steps: Wi-Fi (state 1)
; and the time server (state 4); without time we say so and pause 1 s.
; ---------------------------------------------------------------------------
; d0.w = 1: wait while the adapter connects to Wi-Fi
;        2: wait for the time server, then set the clock
set_clock:
        move.w  d0,a3                   ; phase
        lea     skipclk(pc),a0
        tst.w   (a0)
        bne     .done                   ; ESC or time-out earlier: no clock
        moveq   #0,d5                   ; step on screen: 0 none, 1 Wi-Fi, 4 time
        moveq   #-1,d7                  ; seconds shown on this line
.ask:   lea     cdb(pc),a0
        move.w  id(pc),d1
        lsl.b   #5,d1
        ori.b   #$11,d1
        move.b  d1,(a0)
        move.b  #'A',1(a0)
        move.b  #'T',2(a0)
        move.b  #9,3(a0)
        clr.b   4(a0)
        clr.b   5(a0)
        lea     bounce(pc),a1
        clr.l   (a1)
        moveq   #1,d0
        moveq   #0,d1
        bsr     acsi_cmd
        lea     bounce(pc),a2
        move.l  (a2),d0
        clr.b   d0
        cmp.l   #$41544300,d0           ; "ATC": older firmware says nothing
        bne     .done
        moveq   #0,d0
        move.b  3(a2),d0
        cmp.w   #3,d0
        beq     .done                   ; switched off
        cmp.w   #1,a3
        bne     .ph2
        cmp.w   #1,d0                   ; phase 1: only while connecting
        beq     .wait
        tst.w   d5
        beq     .done
        cmp.w   #2,d0
        beq     .none                   ; Wi-Fi failed while we waited
        lea     msg_okmark(pc),a0       ; Wi-Fi is up
        bra     print
.ph2:   tst.w   d0
        beq     .set                    ; time valid
        cmp.w   #2,d0
        beq     .none                   ; no Wi-Fi / no time server
.wait:  cmp.w   d5,d0                   ; a new step: own line, own 30 s
        beq     .count
        tst.w   d5
        beq     .step
        lea     msg_okmark(pc),a0       ; previous step done
        bsr     print
.step:  move.w  d0,d5
        lea     msg_wifi(pc),a0
        cmp.w   #1,d5
        beq     .step2
        lea     msg_time(pc),a0
.step2: bsr     print
        lea     msg_esc(pc),a0          ; hint two lines down, cursor back
        bsr     print
        moveq   #-1,d7
        move.l  HZ_200.w,d6             ; start of this step
.count: move.l  HZ_200.w,d0
        sub.l   d6,d0
        divu    #200,d0
        moveq   #30,d1
        sub.w   d0,d1                   ; seconds left
        ble     .tmo
        cmp.w   d7,d1
        beq     .key
        tst.w   d7
        bmi     .num
        lea     msg_bs(pc),a0           ; back over the two digits
        bsr     print
.num:   move.w  d1,d7
        move.w  d1,d0
        lea     msg_num(pc),a0
        bsr     two
        lea     msg_num(pc),a0
        bsr     print
.key:   move.w  #2,-(sp)
        move.w  #1,-(sp)                ; Bconstat(CON)
        trap    #13
        addq.l  #4,sp
        tst.w   d0
        beq     .pause
        move.w  #2,-(sp)
        move.w  #2,-(sp)                ; Bconin(CON)
        trap    #13
        addq.l  #4,sp
        cmp.b   #27,d0                  ; ESC: no clock this boot
        bne     .pause
        lea     skipclk(pc),a0
        st      (a0)
        lea     msg_cancel(pc),a0
        bra     print
.pause: moveq   #50,d0                  ; 0.25 s
        bsr     delay
        bra     .ask

.tmo:   lea     skipclk(pc),a0          ; this step timed out: no clock
        st      (a0)
        lea     msg_komark(pc),a0
        bra     .fail1
.none:  lea     skipclk(pc),a0          ; the adapter gave up: say it once
        st      (a0)
        lea     msg_komark(pc),a0
        tst.w   d5
        bne     .fail1
        lea     msg_notime(pc),a0       ; nothing on screen yet
.fail1: bsr     print
        move.l  #200,d0                 ; 1 s, so the message can be read
        bra     delay

.set:   tst.w   d5
        beq     .set2
        lea     msg_okmark(pc),a0       ; last step done
        bsr     print
.set2:  moveq   #0,d0                   ; GEMDOS date: (year-1980)<<9 | month<<5 | day
        move.w  4(a2),d0
        sub.w   #1980,d0
        lsl.w   #4,d0
        or.b    6(a2),d0
        lsl.w   #5,d0
        or.b    7(a2),d0
        move.w  d0,d3
        moveq   #0,d0                   ; time: hour<<11 | minute<<5 | second/2
        move.b  8(a2),d0
        lsl.w   #6,d0
        or.b    9(a2),d0
        lsl.w   #5,d0
        moveq   #0,d1
        move.b  10(a2),d1
        lsr.w   #1,d1
        or.w    d1,d0
        move.w  d0,d4
        move.w  d3,-(sp)
        move.w  #$2b,-(sp)              ; Tsetdate
        trap    #1
        addq.l  #4,sp
        move.w  d4,-(sp)
        move.w  #$2d,-(sp)              ; Tsettime
        trap    #1
        addq.l  #4,sp
        move.w  d4,-(sp)                ; Settime: keyboard / battery clock
        move.w  d3,-(sp)
        move.w  #22,-(sp)
        trap    #14
        addq.l  #6,sp
        lea     bounce(pc),a2           ; the traps may change a0-a2
        lea     msg_clk_d(pc),a0        ; "Clock set to DD-MM-YYYY HH:MM"
        move.b  7(a2),d0
        bsr     two
        addq.l  #1,a0
        move.b  6(a2),d0
        bsr     two
        addq.l  #1,a0
        moveq   #0,d0
        move.w  4(a2),d0
        divu    #100,d0
        move.l  d0,d1
        bsr     two                     ; century
        swap    d1
        move.b  d1,d0
        bsr     two
        addq.l  #1,a0
        move.b  8(a2),d0
        bsr     two
        addq.l  #1,a0
        move.b  9(a2),d0
        bsr     two
        lea     msg_clock(pc),a0
        bsr     print
.done:  rts

; d0.b (0..99) -> two digits at (a0)+
two:    andi.l  #$ff,d0
        divu    #10,d0
        add.b   #'0',d0
        move.b  d0,(a0)+
        swap    d0
        add.b   #'0',d0
        move.b  d0,(a0)+
        rts

; wait d0.l ticks of 5 ms
delay:  add.l   HZ_200.w,d0
.w:     cmp.l   HZ_200.w,d0
        bhi.s   .w
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
        moveq   #0,d0                   ; bflags: FAT type from the cluster
        cmp.w   #4085,d1                ; count, as EmuTOS and DOS decide it
        blo.s   .f12
        moveq   #1,d0                   ; 16-bit FAT
.f12:   move.w  d0,16(a1)

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

        ; drive letters: the first free one from C: for the flash disk; for
        ; the TNFS partitions the letter the adapter asks for (vendor sub 8,
        ; set in the configuration program) when it is free, else the next
        ; free one
        lea     cdb(pc),a0
        move.w  id(pc),d1
        lsl.b   #5,d1
        ori.b   #$11,d1
        move.b  d1,(a0)
        move.b  #'A',1(a0)
        move.b  #'T',2(a0)
        move.b  #8,3(a0)
        clr.b   4(a0)
        clr.b   5(a0)
        lea     bounce(pc),a1
        clr.l   (a1)
        moveq   #1,d0
        moveq   #0,d1
        bsr     acsi_cmd
        lea     bounce(pc),a2
        cmp.l   #$41544c04,(a2)         ; "ATL", 4 entries
        beq.s   .lok
        clr.l   4(a2)                   ; no answer: no wishes
.lok:   move.l  DRVBITS.w,d0
        lea     pdrv(pc),a1
        lea     msg_drv(pc),a3
        moveq   #0,d7
        moveq   #0,d6                   ; a wanted letter was taken
.let:   moveq   #0,d1
        move.b  4(a2,d7.w),d1           ; wanted letter or 0
        sub.b   #'A',d1
        cmp.w   #3,d1                   ; D: .. Z: only
        blt.s   .auto
        cmp.w   #26,d1
        bge.s   .auto
        btst    d1,d0
        beq.s   .got
        moveq   #1,d6
.auto:  moveq   #2,d1
.fr:    btst    d1,d0
        beq.s   .got
        addq.w  #1,d1
        cmp.w   #26,d1
        blt.s   .fr
        bra     .fail
.got:   bset    d1,d0
        move.w  d7,d2
        add.w   d2,d2
        move.w  d1,(a1,d2.w)
        tst.w   d7
        beq.s   .first
        move.b  #',',(a3)+
        move.b  #' ',(a3)+
.first: add.b   #'A',d1
        move.b  d1,(a3)+
        move.b  #':',(a3)+
        addq.w  #1,d7
        cmp.w   npart(pc),d7
        blt.s   .let
        move.b  #13,(a3)+
        move.b  #10,(a3)+
        clr.b   (a3)
        move.l  d0,DRVBITS.w
        lea     firstdrv(pc),a0
        move.w  pdrv(pc),(a0)

        ; boot from our first partition, like HDDRIVER does. GEMDOS already
        ; picked its current drive from _bootdev before the floppy and hard
        ; disk boot (xsetdrv(bootdev) in osinit; _bootdev is still A: after
        ; a cold boot), and the AUTO folder and the desktop inherit that
        ; drive, so set both.
        move.w  firstdrv(pc),BOOTDEV.w
        move.w  firstdrv(pc),-(sp)
        move.w  #$0e,-(sp)              ; Dsetdrv
        trap    #1
        addq.l  #4,sp

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
        lea     old_resvalid(pc),a0
        move.l  RESVALID.w,(a0)
        lea     old_resvec(pc),a0
        move.l  RESVECTOR.w,(a0)
        lea     my_reset(pc),a1
        move.l  a1,RESVECTOR.w
        move.l  #$31415926,RESVALID.w

        move.w  d6,-(sp)                ; a wanted letter was taken
        moveq   #1,d0                   ; Wi-Fi first (only shown when waiting)
        bsr     set_clock
        lea     msg_ok(pc),a0
        bsr     print
        tst.w   (sp)+
        beq.s   .lfree
        lea     msg_taken(pc),a0
        bsr     print
        move.l  #200,d0                 ; 1 s: noteworthy, let it be read
        bsr     delay
.lfree: moveq   #2,d0                   ; then the time server and the clock
        bsr     set_clock
        bsr     query_gen               ; media change counters: start values
        lea     pchg(pc),a0
        clr.l   (a0)                    ; nothing to report yet (MAXPART = 4)
        movem.l (sp)+,d0-d7/a0-a6
        rts

.fail:  lea     msg_fail(pc),a0
        bsr     print
        move.l  #200,d0                 ; 1 s, so the message can be read
        bsr     delay
        movem.l (sp)+,d0-d7/a0-a6
        rts

msg_hello:  dc.b    13,10,27,"pACSI2TNFS v"
            VERSION
            dc.b    27,"q",13,10
            dc.b    "http://retroloft.net",13,10,13,10,0
msg_ok:     dc.b    "[OK] Drives installed: "
msg_drv:    ds.b    20                  ; "C:, D:, G:, N:",13,10,0
msg_fail:   dc.b    "[KO] Disk not usable, driver not loaded",13,10,0
msg_taken:  dc.b    "[--] Drive letter taken, used next free",13,10,0
; status lines as in SideTNFS: "[..] step... NN", the mark is overwritten
; with [OK] / [KO] / [--] when the step ends
; save cursor, hint two lines down, back to the step line (VT52 ESC j/k)
msg_esc:    dc.b    27,"j",13,10,13,10,"Press [ESC] to skip",27,"k",0
msg_wifi:   dc.b    "[..] Wi-Fi... ",0
msg_time:   dc.b    "[..] Time server... ",0
msg_num:    dc.b    "30",0
msg_bs:     dc.b    8,8,0
; end of a step: mark, next line, clear the hint below (ESC J)
msg_okmark: dc.b    13,"[OK]",13,10,27,"J",0
msg_komark: dc.b    13,"[KO]",13,10,27,"J",0
msg_cancel: dc.b    13,"[--]",13,10,27,"J",0
msg_notime: dc.b    "[KO] No network time, clock not set",13,10,0
msg_clock:  dc.b    "[OK] Date and time: "
msg_clk_d:  dc.b    "DD-MM-YYYY HH:MM",13,10,0
            even

        if      *>DRVSECT*512
        fail    "driver too large"
        endif
