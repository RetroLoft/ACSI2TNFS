# ACSI2TNFS

A Raspberry Pi Pico 2 W on the **ACSI** (hard disk) port of an Atari ST, STE, Mega ST
or TT. To the Atari it is a hard disk; behind it is Wi-Fi.

- **Drive C:** a small hard disk (about 1 MB) in the Pico's flash. The Atari can boot
  from it.
- **Network drives:** up to three folders on a **TNFS** server (a PC, a NAS or the
  internet) show up as normal drives, readable and writable.
- **Clock:** the Pico gets the time from the internet and sets the Atari's clock at
  start-up.
- **Internet:** with the STinG TCP/IP stack the Atari gets its own IP address on your
  Wi-Fi network: ping, FTP, web pages.

No software to install for the disk part: the adapter brings its own driver. A GEM
program, **ACSITNFS.PRG**, sets up Wi-Fi, network drives, clock and the ACSI id.

> Status: working on real hardware: ST with TOS 1.02/1.04, Mega STE (TOS 2.06, HDDRIVER),
> TT (TOS 3.06) and EmuTOS. The network function (STinG) is new and still being refined.

> [!IMPORTANT]
> ## MEGA STE AND TT: SET THE ADAPTER TO THE FIRST FREE ACSI ID
>
> **A new adapter uses ACSI id 6. On a Mega STE or a TT the machine does not see it there.**
>
> - **TT:** TOS 3.06 only asks ACSI id **0** at start-up. Its own hard disk is SCSI, so
>   use **id 0**.
> - **Mega STE:** the internal hard disk adapter is on id 0, so use **id 1**. With
>   HDDRIVER also switch id 1 on in HDDRUTIL.
> - In general: the lowest id that no other device on the ACSI port uses.
>
> Set it with the USB console (key `0`–`7`) or `ACSITNFS.PRG` → *Config → More*, then
> restart the Atari. See [docs/ADAPTER.md](docs/ADAPTER.md#acsi-id).

## What you need

- The ACSI2TNFS board with a **Raspberry Pi Pico 2 W** (`hardware/`: `v2` plugs straight
  into the Atari, `dev` connects with a ribbon cable and has a pass-through connector).
- An Atari with an ACSI port and **TOS 1.02 or later**, or EmuTOS. TOS 1.0 does not
  see the adapter.
- A 2.4 GHz Wi-Fi network.
- A USB cable for the Pico: it powers the adapter and gives a console on your PC.

## Getting started

1. **Firmware.** Hold the Pico's BOOTSEL button while you plug it into a PC, and copy
   `acsi2tnfs.uf2` onto the drive that appears (build it yourself: see
   [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)). The Pico restarts and prepares drive C:.
2. **Connect** the adapter to the Atari's ACSI port and power the Pico over USB.
   Switch the Atari on. You see `ACSI2TNFS v…` and `[OK] Drives installed: C:`.
3. **Show C: on the desktop.** Select the floppy A: icon, *Options → Install Disk Drive*,
   letter `C`, *Install*, then *Options → Save Desktop*. C: holds `README.TXT` and the
   programs below.
4. **Set up Wi-Fi and network drives.** Start `C:\ACSITNFS.PRG`: *Config* for Wi-Fi
   (network name, password, country) with *More* for the clock and the ACSI id, and the
   drive list for the TNFS drives (server, folder, drive letter). *Save*; the Pico
   restarts and the Atari with it.

From then on the Atari boots with C:, the network drives and the right time.

### Good to know

- **ACSI id.** A new adapter uses ACSI id **6**, so it does not clash with a hard disk on
  id 0. Every device on the ACSI bus needs its own id. **Mega STE and TT: the first free
  id** (see above).
- **Drive letters** for network drives: D: to P: (TOS knows 16 drives).
- **TOS 1.x with several drives:** put `FOLDR100.PRG` (from Atari's AHDI) in `C:\AUTO`,
  or TOS runs out of folder memory ("use FOLDR100.PRG").
- **Clock without our driver:** with HDDRIVER (Mega STE) or on a TT booting from its own
  disk the adapter's driver does not load and does not set the clock. Copy
  `C:\NETCLOCK.PRG` into your AUTO folder: it waits for the adapter's Wi-Fi and time
  server (up to 30 s each, ESC skips) and sets the clock.
- **Read-only files on C:** `README.TXT`, `ACSITNFS.PRG`, `ACSI_NET.STX`,
  `STINGINS.TOS` and `NETCLOCK.PRG` come with the firmware. Your own files (DESKTOP.INF, an AUTO folder, …)
  stay when you update the firmware.

More: [docs/ADAPTER.md](docs/ADAPTER.md) (USB console, ACSI id, hidden mode, C:,
troubleshooting) and [docs/TNFS.md](docs/TNFS.md) (network drives, running a TNFS server).

## Internet on the Atari (STinG)

The adapter passes network traffic between the Atari and your Wi-Fi network. The Atari
runs the [STinG](https://github.com/th-otto/STinG) TCP/IP stack (freeware, for TOS); the
driver `ACSI_NET.STX` connects STinG to the adapter.

**Install:** start `C:\STINGINS.TOS` on the adapter's drive (with HDDRIVER as your hard
disk driver this is the adapter's letter, e.g. `K:\STINGINS.TOS`). It carries STinG 1.26
with TCP.STX 1.41 and asks for:

- the drive to install on (the boot drive by default);
- a **free IP address** for the Atari in your network: not the adapter's own, and
  outside the range your router hands out;
- subnet mask, router and name server: it suggests the adapter's own values.

Restart, and STinG is on the network; XControl is not needed. Start `STINGINS.TOS`
again to change only the IP settings: your other STinG settings stay.

Test with `C:\STING\TOOLS\PING.PRG` to your router. Small STinG programs, e.g. a web
page viewer: [Network tools for the Atari ST](https://github.com/RetroLoft/atari-net-tools).

More, with what the installer writes and troubleshooting: [docs/NETWORK.md](docs/NETWORK.md).

## Documentation

| | |
|---|---|
| [docs/NETWORK.md](docs/NETWORK.md) | Internet with STinG: the installer, IP address, programs, limits |
| [docs/TNFS.md](docs/TNFS.md) | Network drives: TNFS servers, writing, what to keep in mind |
| [docs/ADAPTER.md](docs/ADAPTER.md) | ACSI id, USB console, system files on C:, TOS versions, troubleshooting |
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

## License

Copyright (C) 2025-2026 Frank Beentjes.

GNU General Public License, version 3 or (at your option) any later version: see
[LICENSE](LICENSE).

- `atari/sting/` (`ACSI_NET.STX`) contains code after USB_NET.STX and is GPL-2.0-or-later
  (`atari/sting/LICENSE`); `atari/sting/include/` holds headers from the STinG developer kit.
- The firmware is built with the Raspberry Pi Pico SDK (BSD-3-Clause), lwIP (BSD) and the
  `cyw43-driver` for the Wi-Fi chip, which Raspberry Pi licenses for use with its own
  microcontrollers only (`lib/cyw43-driver/LICENSE.RP` in the SDK).
