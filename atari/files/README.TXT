ACSI2TNFS - THE SETTINGS DRIVE
==============================

You are reading this file from a Raspberry Pi Pico 2 W on the
ACSI port of your Atari. To the Atari it is a hard disk; behind
it are your network and a micro SD card. It gives the Atari:
  - network drives: folders on a TNFS server (PC, NAS, ...)
  - SD card drives: folders on the micro SD card in the adapter
  - internet with STinG, and the time from the internet
All of these are optional.

What is on this drive?
  README.TXT   this file
  ACSITNFS.PRG configuration program (GEM): Wi-Fi, TNFS and SD
               drives, network clock and ACSI device ID
  STINGINS.TOS installs STinG for internet (see below)
  ACSI_NET.STX the STinG driver for the adapter
  NETCLOCK.PRG sets the clock from the network, for the AUTO
               folder when AHDI or HDDRIVER boots (see below)
  These files are read-only. Your own files on this drive stay
  when the firmware is updated. Deleted one anyway, or one
  missing after an update? Key R on the USB console puts it back.

Which letter has this drive?
  - No other hard disk: the adapter boots with its own driver
    and this drive is C:, the boot drive (AUTO, DESKTOP.INF).
  - With a hard disk of your own its driver boots, and the
    adapter's drives follow your partitions: with C: and D: of
    your own, this drive is E:.
  To show C: on the desktop: select floppy A:, Options /
  Install Disk Drive, letter C, Install, then Options / Save
  Desktop.

MEGA STE AND TT: SET THE ACSI ID TO THE FIRST FREE ONE!
  A new adapter uses ID 6: a Mega STE or a TT does not see it.
  - TT: ID 0 (with AHDI only ID 0 was asked; its disk is SCSI).
  - Mega STE: ID 1 (the internal disk adapter is on ID 0);
    with HDDRIVER also switch ID 1 on in HDDRUTIL.
  Every device on the ACSI bus needs its own ID. Change it in
  ACSITNFS.PRG (Config > More > ACSI device ID, then Save and
  restart) or with keys 0-7 on the USB console; the console
  always works, also when two devices share an ID.

TNFS and SD drives
  Up to three drives next to this one, TNFS and SD together
  (ACSITNFS.PRG has eight slots: the first three switched on
  are used).
  - TNFS: a server name or IP address and a folder on it.
    TNFS has no passwords: use it only in your own network.
  - SD: a folder on the card, e.g. /ATARI. Format the card on
    a PC as FAT32 (or exFAT). The files on the card are normal
    files: a PC reads them too. SD drives work without Wi-Fi.
    With a card in the slot, TNFS drives also use it as a write
    cache: writing to the network is about six times faster.
  A drive with only NET_WAIT.TXT is still connecting: open it
  again in a moment. NET_ERR.TXT or SD_ERR.TXT tells what is
  wrong.

Drive letters and hard disk drivers
  - The adapter's own driver: the letters chosen in
    ACSITNFS.PRG, D: to P: (a taken letter: the next free one).
  - AHDI and HDDRIVER: letters in order after your own
    partitions. HDDRIVER only asks the IDs switched on in
    HDDRUTIL.
  - EmuTOS mounts the drives itself as C:, D:, E:, ...
  - TOS 1.x with several drives: put FOLDR100.PRG (AHDI) in
    the AUTO folder, or GEMDOS runs out of folder memory.
  - TOS 1.0 does not see the adapter.

The clock
  The adapter's own driver sets the clock at start-up (switch
  it on in ACSITNFS.PRG, Config > More). With AHDI or HDDRIVER
  copy NETCLOCK.PRG into the AUTO folder of your boot drive.
  The time zone is a fixed offset: change it for summer time.

Internet with STinG
  STINGINS.TOS installs STinG 1.26 (freeware, P. Rottengatter
  and R. Andersson) with TCP.STX 1.41 (C. Zietz) and the driver.
  1. Start STINGINS.TOS and answer:
     - the drive: your boot drive, almost always C:
     - if STinG is there already: 1 = network settings only,
       2 = install everything again (in doubt: 2)
     - a free IP address for the Atari: not the adapter's and
       not one your router hands out
     - subnet mask (almost always 255.255.255.0), gateway (your
       router) and DNS server (usually the router too): it
       suggests them
  2. Restart. XControl is not needed.

Limitations of TNFS and SD drives
  - 8.3 names: longer names are shortened (LONGFILENAME.TXT
    shows as LONGFILE.TXT or LONGFI~1.TXT). Names that start
    with a dot are hidden.
  - 16 MB per drive, whatever the size of the folder behind it.
  - The FAT can fill up: a folder gets its place in the 16 MB
    when the Atari first opens it. When the room is used up,
    or TOS has read the whole FAT (free space, Show Info),
    folders opened after that show only part of their files.
    With the adapter's own driver the drive starts afresh by
    itself after 2 s: close the folder and open it again.
    With AHDI or HDDRIVER: reset the Atari.
  - At most 511 entries in the root and 254 in a folder.
  - Changes made on a PC are seen after an Atari reset.
  - The Atari writes sectors; about a second later the adapter
    turns them into files. Wait a few seconds after copying to
    a TNFS drive before you switch the adapter off.

USB console of the Pico (any baud rate, it is USB)
  0-7 = ACSI ID     H = hide/show       K = clock on/off
  i = info          n = network status  N = reconnect drives
  R = restore system files              F = rewrite this drive
  B = BOOTSEL (firmware update)         h = help
The button on the board: a short press hides or shows the
adapter (like H). Reset the Atari after pressing it.
Leds: D1 flashes on disk activity; D2 is on, and blinks while
the adapter is hidden. The Pico's own led: Wi-Fi connecting
(blinks), connected (on), no Wi-Fi (off).

More: github.com/RetroLoft/ACSI2TNFS
