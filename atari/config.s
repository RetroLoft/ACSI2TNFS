; CONFIG.TOS - ACSI2TNFS configuration program
; Finds the adapter with INQUIRY, shows the firmware info text and offers
; settings through the vendor command $11 ('A','T',sub,arg):
;   sub 0  info text (1024 bytes)         sub 3  network settings (512 bytes out)
;   sub 1  blink led                      sub 4  connect Wi-Fi + test TNFS
;   sub 2  set ACSI id

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
        bsr     rd_1
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
        bra     menu
.next:  addq.w  #1,d7
        cmp.w   #8,d7
        blt.s   .scan
        lea     msg_none(pc),a0
        bsr     print
        bsr     getkey
        bra     quit

; --- main menu -------------------------------------------------------------
menu:   bsr     show_info
        lea     msg_menu(pc),a0
        bsr     print
        bsr     getkey
        cmp.b   #'1',d0
        beq.s   led
        cmp.b   #'2',d0
        beq     setid
        cmp.b   #'3',d0
        beq     netcfg
        cmp.b   #'4',d0
        beq     nettest
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

; --- option 3: network settings ----------------------------------------------
netcfg: lea     nblk,a0
        move.w  #511,d0
.clr:   clr.b   (a0)+
        dbra    d0,.clr
        lea     nblk,a3
        lea     msg_keep(pc),a0
        bsr     print
        lea     msg_ssid(pc),a0
        moveq   #32,d0
        bsr     askfield
        lea     msg_pass(pc),a0
        moveq   #63,d0
        bsr     askfield
        lea     msg_srv(pc),a0
        moveq   #63,d0
        bsr     askfield
        lea     msg_path(pc),a0
        moveq   #95,d0
        bsr     askfield

        lea     cmd(pc),a0              ; vendor sub 3, 512 bytes Atari -> Pico
        bsr     vend_hdr
        move.b  #3,3(a0)
        lea     dbuf(pc),a1
        move.l  #nblk,(a1)
        lea     dsect(pc),a1
        move.w  #1,(a1)
        lea     ddir(pc),a1
        move.w  #1,(a1)
        bsr     do_cmd
        lea     msg_sent(pc),a0
        tst.l   d0
        beq.s   .ok
        lea     msg_sfail(pc),a0
.ok:    bsr     print
        bsr     getkey
        bra     menu

; a0 = prompt, d0 = max length; appends the answer + NUL at a3
askfield:
        move.w  d0,-(sp)
        bsr     print
        move.w  (sp)+,d0
        lea     line,a0
        move.b  d0,(a0)
        clr.b   1(a0)
        pea     (a0)
        move.w  #10,-(sp)               ; Cconrs
        trap    #1
        addq.l  #6,sp
        lea     line,a0
        moveq   #0,d0
        move.b  1(a0),d0
        addq.l  #2,a0
        bra.s   .cnt
.cp:    move.b  (a0)+,(a3)+
.cnt:   dbra    d0,.cp
        clr.b   (a3)+
        lea     msg_crlf(pc),a0
        bra     print

; --- option 4: connect and test, refresh the status until a key --------------
nettest:
        moveq   #4,d0
        moveq   #0,d1
        bsr     vendor
.loop:  bsr     show_info
        lea     msg_test(pc),a0
        bsr     print
        moveq   #49,d7                  ; ~1 s (50 VBLs at 50 Hz)
.wait:  move.w  #37,-(sp)               ; Vsync
        trap    #14
        addq.l  #2,sp
        move.w  #11,-(sp)               ; Cconis
        trap    #1
        addq.l  #2,sp
        tst.w   d0
        bne.s   .key
        dbra    d7,.wait
        bra.s   .loop
.key:   bsr     getkey
        bra     menu

; --- helpers -----------------------------------------------------------------

; clear screen, title, info text
show_info:
        lea     msg_cls(pc),a0
        bsr     print
        lea     msg_title(pc),a0
        bsr     print
        moveq   #0,d0
        moveq   #0,d1
        bsr     vendor                  ; info: 1024 bytes into buf
        tst.l   d0
        bne.s   .err
        clr.b   buf+1023
        lea     buf,a0
        bra     print
.err:   lea     msg_err(pc),a0
        bra     print

; fill cmd with [$11|id, 'A', 'T', ?, ?, 0]; returns a0 = cmd
vend_hdr:
        lea     cmd(pc),a0
        move.w  id(pc),d2
        lsl.b   #5,d2
        ori.b   #$11,d2
        move.b  d2,(a0)
        move.b  #'A',1(a0)
        move.b  #'T',2(a0)
        clr.b   5(a0)
        rts

; vendor command, reading up to 1024 bytes into buf: d0 = sub, d1 = arg
vendor: move.b  d0,-(sp)
        bsr     vend_hdr
        move.b  (sp)+,3(a0)
        move.b  d1,4(a0)
        bsr     clearbuf
        lea     dsect(pc),a1
        move.w  #2,(a1)
        bsr.s   rd_buf
        bra.s   do_cmd

; set up a DMA read into buf: rd_1 = 1 sector, rd_buf = keep dsect
rd_1:   lea     dsect(pc),a1
        move.w  #1,(a1)
rd_buf: lea     ddir(pc),a1
        clr.w   (a1)
        lea     dbuf(pc),a1
        move.l  #buf,(a1)
        rts

; run cmd in supervisor mode with DMA buffer dbuf, dsect sectors, ddir
do_cmd: pea     super(pc)
        move.w  #38,-(sp)               ; Supexec
        trap    #14
        addq.l  #6,sp
        rts

super:  lea     cmd(pc),a0
        move.l  dbuf(pc),a1
        move.w  dsect(pc),d0
        move.w  ddir(pc),d1
        bsr     acsi_cmd
        rts

clearbuf:
        lea     buf,a0
        move.w  #1023,d0
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

quit:   clr.w   -(sp)
        trap    #1                      ; Pterm0

        include "acsi.inc"

id:     dc.w    0
dsect:  dc.w    1
ddir:   dc.w    0
dbuf:   dc.l    0
cmd:    ds.b    6
prod:   dc.b    "ACSI2TNFS"
msg_cls:    dc.b    27,"E",0
msg_title:  dc.b    27,"p  ACSI2TNFS configuration  ",27,"q",13,10,13,10,0
msg_none:   dc.b    "No ACSI2TNFS adapter found on ACSI id 0-7.",13,10
            dc.b    "Press a key.",13,10,0
msg_err:    dc.b    "Adapter did not answer the info command.",13,10,0
msg_menu:   dc.b    13,10,"  1  Blink the led",13,10
            dc.b    "  2  Change ACSI id",13,10
            dc.b    "  3  Wi-Fi and TNFS settings",13,10
            dc.b    "  4  Connect and test the TNFS server",13,10
            dc.b    "  Q  Quit",13,10,13,10,"Choice: ",0
msg_askid:  dc.b    13,10,"New ACSI id (0-7): ",0
msg_idok:   dc.b    13,10,"Stored. It becomes active after an Atari reset.",13,10
            dc.b    "Press a key.",13,10,0
msg_keep:   dc.b    13,10,13,10,"Press Return to keep the current value.",13,10,0
msg_ssid:   dc.b    "Wi-Fi network (SSID) : ",0
msg_pass:   dc.b    "Wi-Fi password       : ",0
msg_srv:    dc.b    "TNFS server (IP/name): ",0
msg_path:   dc.b    "TNFS path            : ",0
msg_crlf:   dc.b    13,10,0
msg_sent:   dc.b    13,10,"Settings stored in the adapter. Press a key.",13,10,0
msg_sfail:  dc.b    13,10,"Adapter did not accept the settings. Press a key.",13,10,0
msg_test:   dc.b    13,10,"Testing... the status above refreshes every second.",13,10
            dc.b    "Press a key to return to the menu.",13,10,0
            even

        section bss
buf:    ds.b    1024
nblk:   ds.b    512
line:   ds.b    100
