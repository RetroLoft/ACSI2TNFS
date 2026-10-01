ACSI2TNFS - FLASH HARD DISK
===========================

You are reading this file from a Raspberry Pi Pico that acts as
a hard disk on the ACSI port of your Atari.

What is on this disk?
  README.TXT   this file
  ACSITNFS.PRG configuration program (GEM): Wi-Fi, TNFS drives,
               clock and ACSI device ID

How does it work?
  - The Pico answers ACSI commands on its own device ID
    (a new adapter: ID 6, see "Changing the ACSI ID").
  - The disk (about 1 MB) lives in the flash memory of the Pico.
  - Sector 0 holds a boot loader. TOS runs it at start-up; it
    loads a small driver that installs the GEM partition as
    drive C: and the TNFS drives after it.
  - Writing works too: the Pico erases and programs flash in
    blocks of 4 KB.

Showing drive C: on the desktop
  Select the icon of floppy A:, choose Options / Install Disk
  Drive, change the letter to C and click Install.
  Then use Options / Save Desktop if you like.

Changing the ACSI ID
  Give every device on the ACSI bus its own ID. If two devices
  share an ID they answer at the same time and neither works;
  ACSITNFS.PRG cannot reach the adapter then either.
  - ACSITNFS.PRG: Config > More > ACSI device ID (after Save
    and a restart).
  - USB console of the Pico: keys 0 to 7. Always works, also
    during a conflict: connect the Pico to a PC over USB and
    open its serial port in a terminal program.

README.TXT and ACSITNFS.PRG are read-only. Deleted one anyway?
  Key R on the USB console puts them back; your own files on C:
  stay. Reset the Atari afterwards.

USB console of the Pico (any baud rate, it is USB):
  0-7 = ACSI ID           H = hide/show           K = clock on/off
  R = restore system files on C:                  F = rewrite C:
  i = info                n = network status      B = BOOTSEL

With a Pico W / Pico 2 W: set up Wi-Fi, the TNFS drives and the
clock with ACSITNFS.PRG.
