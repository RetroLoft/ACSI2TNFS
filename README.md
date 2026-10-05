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

> Status: working on real hardware (ST with TOS 1.02/1.04 and EmuTOS). The network
> function (STinG) is new and still being refined.

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
  id 0. Every device on the ACSI bus needs its own id.
- **Drive letters** for network drives: D: to P: (TOS knows 16 drives).
- **TOS 1.x with several drives:** put `FOLDR100.PRG` (from Atari's AHDI) in `C:\AUTO`,
  or TOS runs out of folder memory ("use FOLDR100.PRG").
- **Read-only files on C:** `README.TXT`, `ACSITNFS.PRG`, `ACSI_NET.STX` and
  `URLVIEW.TTP` come with the firmware. Your own files (DESKTOP.INF, an AUTO folder, …)
  stay when you update the firmware.

More: [docs/ADAPTER.md](docs/ADAPTER.md) (USB console, ACSI id, hidden mode, C:,
troubleshooting) and [docs/TNFS.md](docs/TNFS.md) (network drives, running a TNFS server).

## Internet on the Atari (STinG)

The adapter passes network traffic between the Atari and your Wi-Fi network. The Atari
runs the [STinG](https://github.com/th-otto/STinG) TCP/IP stack (free, for TOS); the
driver `ACSI_NET.STX` on C: connects STinG to the adapter.

**You need:** STinG 1.26 and XControl 1.31 (both free):

| | Download |
|---|---|
| STinG 1.26 | [sting126.lzh](https://chebucto.ns.ca/Services/PDA/sting126.lzh) ([page](https://chebucto.ns.ca/Services/PDA/AtariSTComm.shtml)) |
| XControl 1.31 (control panel for the CPX modules) | [xctl131.zip](https://chebucto.ns.ca/Services/PDA/xctl131.zip) |
| `ACSI_NET.STX` | on C: of the adapter |

**In short:**

1. Install STinG: `STING.PRG` and `STING.INF` in `C:\AUTO`, the `STING` folder with
   `TCP.STX`, `UDP.STX`, `RESOLVE.STX`, `DEFAULT.CFG` and `ROUTE.TAB`.
2. Copy `C:\ACSI_NET.STX` into the `STING` folder.
3. Install XControl (`XCONTROL.ACC` in the root of C:, the STinG CPX modules in `C:\CPX`)
   and restart.
4. *Desk → Control Panel → STinG Port Setup*: port **ACSI2TNFS**, a **free IP address**
   of your network, the subnet mask, **Active**, *Save*.
5. Tell STinG where packets go and who looks up names. With a home network
   `192.168.178.x` and the router on `192.168.178.1` (use your own numbers; on a PC
   `ipconfig` shows them as *Default Gateway* and *Subnet Mask*):
   - `C:\STING\ROUTE.TAB`, instead of the example line (fields separated by tabs):
     ```text
     192.168.178.0	255.255.255.0	ACSI2TNFS	0.0.0.0
     0.0.0.0		0.0.0.0		ACSI2TNFS	192.168.178.1
     ```
     Line 1: your own network, directly. Line 2: everything else via the router.
   - `C:\STING\DEFAULT.CFG`: `NAMESERVER  = 192.168.178.1` (your router).

Test with `PING.PRG` (STinG tools) to your router, or show a web page with
`C:\URLVIEW.TTP` (e.g. `info.cern.ch`; http only, not https).

Step by step, with example files and troubleshooting: [docs/NETWORK.md](docs/NETWORK.md).

## Documentation

| | |
|---|---|
| [docs/NETWORK.md](docs/NETWORK.md) | Internet with STinG: installation, IP address, ROUTE.TAB, programs, limits |
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
- STinG by Peter Rottengatter and Ronald Andersson.
