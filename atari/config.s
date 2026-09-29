; CONFIG.TOS - ACSI2TNFS configuration program
; Finds the adapter with INQUIRY, shows the firmware info text and offers
; a few settings through the vendor command $11 ('A','T',sub,arg).

        section text

start:  lea     msg_title(pc),a0
        bsr     print

        ; --- find the adapter: INQUIRY on ids 0..7 ---
        moveq   #0,d7
.scan:  lea     cmd(pc),a0
        move.b  d7,d0
        lsl.b   #5,d0
        ori.b   #$12,d0
        move.b  d0,(a0)+
        clr.b   (a0)+
        clr.b   (a0)+
        clr.b   (a0)+
        move.b  #48,(a0)+
        clr.b   (a0)
        bsr     clearbuf
        bsr     do_cmd
        tst.l   d0
        bne.s   .next
        lea     buf+16,a0
        lea     prod(pc),a1
        moveq   #8,d1
.cmp:   cmpm.b  (a0)+,(a1)+
        bne.s   .next
        dbra    d1,.cmp
        lea     id(pc),a0
        move.w  d7,(a0)
        bra     found
.next:  addq.w  #1,d7
        cmp.w   #8,d7
        blt.s   .scan
        lea     msg_none(pc),a0
        bsr     print
        bsr     getkey
        bra     quit

found:
menu:   lea     msg_cls(pc),a0
        bsr     print
        lea     msg_title(pc),a0
        bsr     print
        ; vendor info: $11 'A' 'T' 0 0 0 -> 256 bytes of text
        moveq   #0,d0
        moveq   #0,d1
        bsr     vendor
        tst.l   d0
        bne.s   .err
        clr.b   buf+255
        lea     buf,a0
        bsr     print
        bra.s   .m
.err:   lea     msg_err(pc),a0
        bsr     print
.m:     lea     msg_menu(pc),a0
        bsr     print
        bsr     getkey
        cmp.b   #'1',d0
        beq.s   led
        cmp.b   #'2',d0
        beq.s   setid
        cmp.b   #'q',d0
        beq     quit
        cmp.b   #'Q',d0
        beq     quit
        cmp.b   #27,d0
        beq     quit
        bra     menu

led:    moveq   #1,d0
        moveq   #0,d1
        bsr     vendor
        bra     menu

setid:  lea     msg_askid(pc),a0
        bsr     print
        bsr     getkey
        sub.b   #'0',d0
        bcs     menu
        cmp.b   #7,d0
        bhi     menu
        moveq   #0,d1
        move.b  d0,d1
        moveq   #2,d0
        bsr     vendor
        lea     msg_idok(pc),a0
        bsr     print
        bsr     getkey
        bra     menu

quit:   clr.w   -(sp)
        trap    #1                      ; Pterm0

; vendor command: d0 = sub, d1 = arg (byte 4). Returns d0 = status.
vendor: lea     cmd(pc),a0
        move.w  id(pc),d2
        lsl.b   #5,d2
        ori.b   #$11,d2
        move.b  d2,(a0)
        move.b  #'A',1(a0)
        move.b  #'T',2(a0)
        move.b  d0,3(a0)
        move.b  d1,4(a0)
        clr.b   5(a0)
        bsr     clearbuf
        ; fall through

; run cmd with a 1-sector DMA read into buf, in supervisor mode
do_cmd: pea     super(pc)
        move.w  #38,-(sp)               ; Supexec
        trap    #14
        addq.l  #6,sp
        rts

super:  lea     cmd(pc),a0
        lea     buf,a1
        moveq   #1,d0
        moveq   #0,d1
        bsr     acsi_cmd
        rts

clearbuf:
        lea     buf,a0
        move.w  #511,d0
.c:     clr.b   (a0)+
        dbra    d0,.c
        rts

print:  move.l  a0,-(sp)
        move.w  #9,-(sp)                ; Cconws
        trap    #1
        addq.l  #6,sp
        rts

getkey: move.w  #8,-(sp)                ; Cnecin
        trap    #1
        addq.l  #2,sp
        rts

        include "acsi.inc"

id:     dc.w    0
cmd:    ds.b    6
prod:   dc.b    "ACSI2TNFS"
msg_cls:    dc.b    27,"E",0
msg_title:  dc.b    27,"p  ACSI2TNFS configuration  ",27,"q",13,10,13,10,0
msg_none:   dc.b    "No ACSI2TNFS adapter found on ACSI id 0-7.",13,10
            dc.b    "Press a key.",13,10,0
msg_err:    dc.b    "Adapter did not answer the info command.",13,10,0
msg_menu:   dc.b    13,10,"  1  Blink the led on the Pico",13,10
            dc.b    "  2  Change ACSI id",13,10
            dc.b    "  Q  Quit",13,10,13,10,"Choice: ",0
msg_askid:  dc.b    13,10,"New ACSI id (0-7): ",0
msg_idok:   dc.b    13,10,"Stored. It becomes active after an Atari reset.",13,10
            dc.b    "Press a key.",13,10,0
            even

        section bss
buf:    ds.b    512
