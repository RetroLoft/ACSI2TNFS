# ACSI2TNFS

A Raspberry Pi Pico 2 W on the **ACSI** (hard disk) port of an Atari. To the Atari it is
a hard disk; behind it are your network and a micro SD card.

It works with every 16-bit Atari that has an ACSI port: **ST, STE, Mega ST, Mega STE
and TT**. The Falcon has no ACSI port (it has SCSI), so ACSI2TNFS cannot be used there.

ACSI2TNFS gives the Atari:

- **Network drives:** folders on a **TNFS** server (a PC, a NAS, a Raspberry Pi) that
  the Atari sees as hard disk partitions, readable and writable.
- **SD card drives:** folders on a micro SD card in the adapter, also as hard disk
  partitions.
- **Internet:** with the STinG TCP/IP stack the Atari gets its own IP address on your
  Wi-Fi network: ping, FTP, web pages.
- **The right time:** the adapter gets the time from the internet and sets the Atari's
  clock.

All of these are optional: use only what you need. A small drive in the Pico's flash,
the **settings drive**, always comes with it: it holds the configuration program and the
tools.

> Status: working on real hardware: ST with TOS 1.02/1.04, Mega STE (TOS 2.06, HDDRIVER),
> TT (TOS 3.06, AHDI) and EmuTOS.

> [!IMPORTANT]
> ## MEGA STE AND TT: SET THE ADAPTER TO THE FIRST FREE ACSI ID
>
> **A new adapter uses ACSI id 6. On a Mega STE or a TT the machine does not see it there.**
>
> - **TT** (tested with AHDI): only ACSI id **0** is asked at start-up. Its own hard disk
>   is SCSI, so use **id 0**.
> - **Mega STE:** the internal hard disk adapter is on id 0, so use **id 1**. With
>   HDDRIVER also switch id 1 on in HDDRUTIL.
> - In general: the lowest id that no other device on the ACSI port uses.
>
> Set it with the USB console (key `0`–`7`) or `ACSITNFS.PRG` → *Config → More*, then
> restart the Atari. See [docs/ADAPTER.md](docs/ADAPTER.md#acsi-id).

## What you need

- The ACSI2TNFS board with a **Raspberry Pi Pico 2 W**: `v2` plugs straight into the
  Atari, `v2-cable` connects with a flat cable to a Lotharek DB19 adapter and has a
  pass-through connector. See [hardware/README.md](hardware/README.md).
- An Atari with an ACSI port and **TOS 1.02 or later**, or EmuTOS. TOS 1.0 does not
  see the adapter.
- For the network functions: a **2.4 GHz** Wi-Fi network.
- For SD card drives: a micro SD card, formatted on a PC as FAT32 (or exFAT).
- A USB cable for the Pico: it powers the adapter and gives a console on your PC.

## Getting started

1. **Firmware.** Hold the Pico's BOOTSEL button while you plug it into a PC, and copy
   `acsi2tnfs.uf2` onto the drive that appears (build it yourself: see
   [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)). The Pico restarts and prepares its
   settings drive.
2. **Connect** the adapter to the Atari's ACSI port and power the Pico over USB.
   Switch the Atari on. Without another hard disk you see `ACSI2TNFS v…` and
   `[OK] Drives installed: C:`.
3. **Show the settings drive on the desktop.** Select the floppy A: icon, *Options →
   Install Disk Drive*, letter `C`, *Install*, then *Options → Save Desktop*.
4. **Configure.** Start `ACSITNFS.PRG` on the settings drive: *Config* for Wi-Fi (network
   name, password, country) with *More* for the clock and the ACSI id, and the drive list
   for TNFS and SD drives (see below). *Save*; the Pico restarts and the Atari with it.

From then on the Atari boots with its drives and the right time.

## The settings drive

The first drive of the adapter lives in the Pico's flash (about 1 MB). Which letter it
gets depends on what else is connected:

- **No other hard disk:** the adapter boots with its own driver and the settings drive
  is **C:**, the boot drive, with your AUTO folder and DESKTOP.INF.
- **You already have a hard disk:** its driver boots and the adapter's drives follow
  after your own partitions. With a hard disk with C: and D:, the settings drive is E:.

On the settings drive:

| File | |
|---|---|
| `ACSITNFS.PRG` | the configuration program: Wi-Fi, TNFS and SD drives, network clock, ACSI id |
| `STINGINS.TOS` | installs STinG for internet on the Atari (below) |
| `NETCLOCK.PRG` | sets the Atari's clock from the network (for the AUTO folder, below) |
| `ACSI_NET.STX` | the STinG driver for the adapter (STINGINS.TOS installs it) |
| `README.TXT` | a short manual |

These files come with the firmware and are read-only. Your own files on the drive stay
when you update the firmware.

## Network drives (TNFS)

A TNFS drive shows a folder on a TNFS server as a hard disk partition. Setting up a TNFS
server is easy: [tnfsd](https://github.com/spectrumero/tnfsd) runs on Linux, macOS,
Windows and a Raspberry Pi; give it a folder and it serves that folder.

TNFS has no passwords and no encryption: run your server **only in your own network**
and do not open it to the internet.

In `ACSITNFS.PRG`, per drive: the server (name or IP address), the folder on the server
and the drive letter. More, and how writing works: [docs/TNFS.md](docs/TNFS.md).

## SD card drives

An SD drive shows a **folder** on the micro SD card in the adapter, e.g. `/ATARI`, as a
hard disk partition. It is not a disk image or a real partition: the adapter translates
the Atari's sector reads and writes into normal files on the card. Put the card in a PC
and you see the same files.

SD drives work without Wi-Fi. With a card in the adapter, TNFS drives also use it as a
write cache: writing to the network gets about six times faster.
More: [docs/SDCARD.md](docs/SDCARD.md).

## Drive letters and hard disk drivers

Next to the settings drive the adapter offers up to **three** TNFS or SD drives.
`ACSITNFS.PRG` has eight slots: the first three that are switched on are used.

Most TOS versions need a hard disk driver to use ACSI drives. Which one loads decides the
drive letters:

| Driver | Letters |
|---|---|
| **The adapter's own driver** (no other hard disk) | the settings drive is C:; TNFS and SD drives get the letters chosen in `ACSITNFS.PRG` (D: to P:). A letter that is taken gives the next free one |
| **AHDI** | assigns letters in order, after your own partitions; the letters in `ACSITNFS.PRG` are not used |
| **HDDRIVER** | assigns letters itself, also in order; HDDRUTIL has its own settings for letters and ACSI ids |
| **EmuTOS** | mounts the adapter with its own ACSI driver as C:, D:, … in order |

With AHDI, HDDRIVER and EmuTOS the adapter's driver does not load, so it does not set the
clock: use `NETCLOCK.PRG` (below). Details per driver and machine:
[docs/ADAPTER.md](docs/ADAPTER.md#booting).

## Internet on the Atari (STinG)

The adapter passes network traffic between the Atari and your Wi-Fi network. The Atari
runs the [STinG](https://github.com/th-otto/STinG) TCP/IP stack (freeware, for TOS); the
driver `ACSI_NET.STX` connects STinG to the adapter.

`STINGINS.TOS` on the settings drive installs everything (STinG 1.26 with TCP.STX 1.41).
Start it and answer its questions:

1. **The drive to install on:** your boot drive, almost always C: (it suggests it).
2. If STinG is already there: **1** changes only the network settings, **2** installs
   everything again. In doubt, choose 2.
3. **Atari IP address:** a **free** address in your network: not the adapter's own, and
   outside the range your router hands out (DHCP).
4. **Subnet mask:** almost always `255.255.255.0`.
5. **Gateway:** your router's IP address. On a Windows PC `ipconfig` shows it as
   *Default Gateway*.
6. **DNS server:** usually the router's address too.

The installer suggests mask, gateway and DNS from the adapter's own Wi-Fi settings.
Confirm, and it installs the files. Restart the Atari: STinG is on the network, without
XControl.

Test with `C:\STING\TOOLS\PING.PRG` to your router. Small STinG programs, e.g. a web
page viewer: [Network tools for the Atari ST](https://github.com/RetroLoft/atari-net-tools).
More, with what the installer writes and troubleshooting: [docs/NETWORK.md](docs/NETWORK.md).

## The clock (NETCLOCK.PRG)

With its own driver the adapter sets the Atari's clock at start-up (switch it on in
`ACSITNFS.PRG` → *Config → More*, with your time zone). With AHDI or HDDRIVER that driver
does not load: copy `NETCLOCK.PRG` into the AUTO folder of your boot drive. It waits for
the adapter's Wi-Fi and the time server (up to 30 s each, ESC skips) and sets the clock.

The time zone is a fixed offset from UTC: change it by hand for summer time.

## Limitations

- **8.3 file names only.** The Atari knows names of at most 8 characters plus a 3-letter
  extension. Longer names on the server or the card are shortened: `LONGFILENAME.TXT`
  shows as `LONGFILE.TXT`, or `LONGFI~1.TXT` when that name is taken. Names that start
  with a dot are hidden.
- **16 MB per drive.** Every TNFS and SD drive is a FAT16 partition of 16 MB, whatever
  the size of the folder behind it. A single file larger than that cannot be shown.
- **The room per drive can run out**, and folders then show up empty: see
  [Empty folders on a network or SD drive](#empty-folders-on-a-network-or-sd-drive).
- **Number of files:** at most 511 entries in the root of a drive and 254 in a folder;
  and about 2400 files and folders in all the folders the Atari has opened since its
  reset, shared by the drives (a drive that is used much takes what the others leave).
- **Changes on the server or the card** made by others (a PC) are seen after the Atari
  reads that folder again: after an Atari reset, or console `N`. Changes made by the
  Atari itself are seen at once.
- **Writing goes in two steps.** The Atari writes sectors; about a second after it
  stops, the adapter turns them into files on the server or card. A large file takes a
  few seconds to arrive on a TNFS server: wait a moment before you switch the adapter
  off.
- **Speed:** reading from a TNFS drive goes at network speed; writing is slower (much
  faster with an SD card as write cache). STinG reaches about 70 KB/s.
- **Drive letters stop at P:** (TOS knows 16 drives).
- **TOS 1.x with several drives:** put `FOLDR100.PRG` (from Atari's AHDI) in your AUTO
  folder, or TOS runs out of folder memory ("use FOLDR100.PRG").
- **Wi-Fi:** 2.4 GHz only (the Pico 2 W's radio).

## Empty folders on a network or SD drive

**Why it happens.** The Atari expects a hard disk with a FAT, a fixed table of where every
file lies. A network or SD drive has no such table: the adapter makes it up while you use
the drive. The first time the Atari opens a folder, the adapter reads that folder from
the server or the card and gives each file in it a place in the drive's 16 MB. That
place stays reserved until the drive starts afresh. The room for one session is limited:

- **16 MB of file space** per drive: every file in every folder you opened counts,
  whether you use it or not;
- **about 2400 files and folders**, shared by all network and SD drives;
- **only the part of the FAT that TOS has not read yet:** TOS keeps the FAT sectors it
  has read in memory, so the adapter cannot add new files there.

**What fills it up quickly**

- **Show Info** (*File → Show Info*) on a network or SD drive. To count the files and
  bytes, TOS opens **every folder** of the drive, down to the last subfolder. On a large
  server this uses up the room of the drive in one go, and every folder that did not
  fit is remembered by TOS as empty. Show Info on a single small folder is fine.
- Programs that ask for the free space of a drive: TOS then reads the whole FAT.
- Browsing through many folders full of large files (disk images, for example).

**What you see.** Folders you open after that are empty or show only part of their
files. The USB console (`n`, and the log) says "some skipped".

**What to do** depends on the hard disk driver:

| Driver | What happens | What you do |
|---|---|---|
| **The adapter's own driver** | after 2 seconds without disk activity the adapter starts the drive afresh and reports a media change; TOS reads the drive again | close the folder and open it again (TOS 2.06: or press Esc) |
| **HDDRIVER** | the same, once console `M` is on: the adapter then reports itself as a removable unit, and HDDRIVER asks for media changes | press `M` on the USB console once (it is stored), then reset the Atari once. Without `M`: reset the Atari |
| **AHDI** | nothing: AHDI does not hear of media changes | reset the Atari. **Leave `M` off:** with AHDI it makes TOS read the whole FAT again and again, and folders stay empty |
| **EmuTOS** (its built-in driver) | nothing: EmuTOS does not hear of media changes | reset the Atari (`M` makes no difference) |

A reset of the Atari always starts every drive afresh, with any driver. A single folder
whose files together are larger than 16 MB always shows only part of its files.

## Documentation

| | |
|---|---|
| [docs/TNFS.md](docs/TNFS.md) | Network drives: TNFS servers, writing, what to keep in mind |
| [docs/SDCARD.md](docs/SDCARD.md) | SD card drives and the write cache |
| [docs/NETWORK.md](docs/NETWORK.md) | Internet with STinG: the installer, IP address, programs, limits |
| [docs/ADAPTER.md](docs/ADAPTER.md) | ACSI id, booting, USB console, system files, troubleshooting |
| [hardware/README.md](hardware/README.md) | The boards `v2` and `v2-cable`, pin map, case |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Building, repository layout, ACSI protocol, test tools |
| `ACSI_NET-ontwerp.md`, `project-notities.md` | Design and project notes (Dutch) |

## Credits

- Configuration program: the ACSI build of
  [SideTNFS-Config](https://github.com/RetroLoft/SideTNFS-Config).
- `ACSI_NET.STX` follows the structure of
  [USB_NET.STX (usbsting)](https://github.com/czietz/usbsting) by Roger Burrows and
  Christian Zietz (GPL-2+), and the frame bridge follows
  [PicoWifi](https://github.com/czietz/picowifi).
- STinG by Peter Rottengatter and Ronald Andersson (freeware); TCP.STX 1.41 by Christian
  Zietz ([chzsoft/sting](https://github.com/chzsoft/sting)). `STINGINS.TOS` carries
  them unchanged (`atari/install/sting/`).
- SD card: [FatFs](http://elm-chan.org/fsw/ff/) by ChaN.

## License

Copyright (C) 2025-2026 Frank Beentjes.

GNU General Public License, version 3 or (at your option) any later version: see
[LICENSE](LICENSE).

- `atari/sting/` (`ACSI_NET.STX`) contains code after USB_NET.STX and is GPL-2.0-or-later
  (`atari/sting/LICENSE`); `atari/sting/include/` holds headers from the STinG developer kit.
- The firmware is built with the Raspberry Pi Pico SDK (BSD-3-Clause), lwIP (BSD),
  FatFs (BSD-style, `third_party/fatfs`) and the `cyw43-driver` for the Wi-Fi chip, which
  Raspberry Pi licenses for use with its own microcontrollers only
  (`lib/cyw43-driver/LICENSE.RP` in the SDK).
