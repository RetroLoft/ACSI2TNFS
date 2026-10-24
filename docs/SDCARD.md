# SD card drives

The adapter has a micro SD card slot. The card serves two purposes:

- **SD drives:** a folder on the card shows up on the Atari as a hard disk partition.
- **Write cache for TNFS drives:** with a card in the slot, writing to a network drive
  is about six times faster.

## The card

Format the card on a PC as **FAT32**, or **exFAT** for cards of 64 GB and more. The
Atari never sees the card's own file system: it only sees the drives the adapter makes
from it, so the card can be much larger than anything TOS can handle.

## Setting up a drive

1. Make a folder on the card on a PC, e.g. `ATARI`, and put your Atari files in it.
2. In `ACSITNFS.PRG` → *Drives*, add a drive of the type **SD**, with the folder
   (`/ATARI`) and a drive letter.
3. *Save*; after the restart the drive is there. Install it on the desktop
   (*Options → Install Disk Drive*).

An SD drive is not a disk image or a partition: the adapter translates the Atari's
sector reads and writes into normal files and folders on the card. Take the card out and
you can read and change the files on any PC.

SD drives come up right after start-up, before Wi-Fi, and also work without Wi-Fi.

## What to keep in mind

- **The limits of a TNFS drive apply** (see [Limitations](../README.md#limitations)):
  8.3 names (long names on the card are shortened), 16 MB per drive, and the FAT that
  fills up during one session.
- **No card, or a card the adapter cannot read:** the drive holds one read-only file,
  `SD_ERR.TXT`, with the reason.
- **Changing the card while the Atari runs:** after putting a card back, press `N` on
  the USB console and reset the Atari. Best: switch off, change the card, switch on.
- **Changes made on a PC** are seen when the Atari reads the folder again: after an Atari
  reset.
- **Hidden file `.A2T.TMP`** in the drive's folder: the adapter keeps the sectors the
  Atari wrote there before it turns them into files. Leave it alone.
- **Writing** is fast: the Atari's writes reach the card within a second.

## Write cache for TNFS drives

A TNFS drive keeps every sector the Atari writes in a hidden file before it sends the
files to the server. Without a card that file is `.A2T.TMP` on the server; with a card
it is `/.A2TCACHE/T<n>.TMP` on the card. The Atari then writes at about 280 KB/s instead
of 45 KB/s, and the files reach the server faster.

This needs nothing to set up: the adapter uses the card when it finds one. The USB
console (`n`) shows "TNFS writes: cached on the SD card". The folder `/.A2TCACHE` may be
deleted while the adapter is off.
