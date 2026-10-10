# Development

## Building

**Firmware:** Pico SDK 2.2.0, CMake and Ninja, board `pico2_w` (the tested board).

```text
cmake -G Ninja -B build-pico2w -DPICO_BOARD=pico2_w
ninja -C build-pico2w            # build-pico2w/acsi2tnfs.uf2
```

The version number lives in `version.txt` only. CMake passes it to the firmware, and
`ninja` runs `atari/mkdisk.py` whenever `version.txt`, an Atari source or a file in
`atari/files/` changes (after adding a file to `atari/files/`, run `cmake` once more).
`mkdisk.py` writes `atari/version.inc`, assembles the boot code and the driver with
[vasm](http://sun.hasenbraten.de/vasm/) (`vasmm68k_mot`, or set `VASM`) and writes the
disk image and `disk_seed.h`:

```text
cd atari
python mkdisk.py          # out/BOOT.BIN, DRV.BIN, disk.img, ../disk_seed.h
```

**Atari programs** (m68k-atari-mint-gcc, e.g. in WSL):

| | |
|---|---|
| `atari/sting/` | `make`: `ACSI_NET.STX` (STinG driver); `-mshort`, no C library |
| `atari/tools/` | `make`: `NETTEST.TTP`, the test tool for the network protocol |
| `ACSITNFS.PRG` | [SideTNFS-Config](https://github.com/RetroLoft/SideTNFS-Config), branch `acsi-transport`, `make acsi` |

Copy new builds of `ACSI_NET.STX` and `ACSITNFS.PRG` into `atari/files/`:
everything there becomes a read-only system file on C:.

## Repository layout

| Path | Contents |
|---|---|
| `acsi2tnfs.c` | core0: USB console, sniffer decoder, settings, disk seeding |
| `acsi_core1.c` | core1: real-time ACSI target, flash disk, command dispatch |
| `acsi_bus.pio` | PIO programs: /CS and /ACK capture, DMA in/out, logic analyser |
| `net.c` | Wi-Fi, NTP clock, TNFS drives (virtual FAT, writing and sync) |
| `netbridge.c` | network function for STinG: frame bridge between ACSI and Wi-Fi |
| `config.c` | configuration protocol for ACSITNFS.PRG |
| `sysfiles.c` | keeps the system files on C: up to date |
| `acsi.h`, `settings.h` | pin map, flash layout, shared types |
| `atari/` | boot code, resident driver (68000 assembler), `mkdisk.py`, `files/` for C: |
| `atari/sting/`, `atari/tools/` | STinG driver, NETTEST |
| `hardware/` | KiCad projects: `v2` (DB19 on the board), `v2-cable` (flat cable, pass-through); see its README |

## ACSI vendor commands

The adapter answers the standard commands (TEST UNIT READY, INQUIRY, READ/WRITE(6),
ICD READ/WRITE(10), …) and one vendor command:

```text
$11|id<<5, 'A', 'T', sub, arg, 0
```

| sub | Direction | |
|---|---|---|
| 5 / 6 | to / from the adapter, 512 bytes | configuration protocol (ACSITNFS.PRG) |
| 8 | from, 512 | drive letters for the driver (`ATL`) |
| 9 | from, 512 | network time (`ATC`) |
| 10 | from, 512 | media change counters (`ATG`) |
| `$20` NET_INFO | from, 512 | network function: version, MAC, IP addresses, counters (`ATN`) |
| `$21` NET_CTRL | to, 512 | bridge on (Atari IP and mask) / off |
| `$22` NET_TX | to, `arg` = 1–3 sectors | one Ethernet frame (8-byte header + frame); status 8 = busy |
| `$23` NET_RX | from, up to `arg` sectors | the next frame for the Atari; one sector when empty |
| `$2F` NET_TEST | from, `arg` sectors | test pattern (short DMA read test) |

An unknown sub gives CHECK CONDITION; that is how programs recognise older firmware. The
design of the network function, with the reasoning and the test reports, is in
`ACSI_NET-ontwerp.md` and `project-notities.md` (Dutch).

## Test and debug tools

- **NETTEST.TTP** (`atari/tools`): finds the adapter, `NET_INFO`, short DMA read test
  (`S`), bridge tests with ARP (`A`, `L`), read test of C: and a network drive (`R`).
  Writes everything also to `NETTEST.LOG` in its folder.
- **USB console:** `v` logs every command with a byte-by-byte /ACK cross-check on reads;
  `L`/`l` arm and dump the built-in PIO logic analyser (16 ns); `s` is a passive ACSI
  bus monitor (sniffer).

## Hardware notes

- The design rules for the boards (no pull-ups on /IRQ and /DRQ, pull-up on /OE, pull-ups
  on the 74LS07 inputs) are in [hardware/README.md](../hardware/README.md).
- The firmware still uses the pin map of the earlier development board (no longer in the
  repo: data on GP8-GP15, /OE GP6, DIR GP7, /IRQ and /DRQ through BC547s, high = active).
  Its BC547 releases /DRQ about 1 µs late; the firmware copes with it, `v2` and `v2-cable`
  use a 74LS07.
- Reads hold each byte about 130 ns after /ACK rises; with less the DMA chip now and then
  latched the next byte.

## References

- Atari ACSI/DMA Integration Guide, 28 June 1991
- P. Putnik, ACSI/DMA experiences: https://atari.8bitchip.info/AcsiDmaExD.html
- TOS 1.x source: https://github.com/th-otto/tos1x
- STinG source: https://github.com/th-otto/STinG
