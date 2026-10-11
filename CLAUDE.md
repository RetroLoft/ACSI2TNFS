# ACSI2TNFS - notes for Claude Code

A Raspberry Pi Pico 2 W that acts as an ACSI device for the Atari ST: a flash disk C:,
up to 3 TNFS network drives (D:-P:), a network clock (NTP) and a network function for
STinG (ACSI_NET.STX). Configured from the Atari with ACSITNFS.PRG. Firmware 0.4.0
(`version.txt`). User docs: `README.md`, `docs/*.md`; developer docs:
`docs/DEVELOPMENT.md`; hardware: `hardware/README.md` (branch `v2`); design and test
reports (Dutch): `project-notities.md`, `ACSI_NET-ontwerp.md`.

## How Frank wants to work

- **Answer in Dutch.** Code, comments, commit messages and repo docs are in English.
- **KISS.** Simple solutions, no speculative features.
- **Ask before committing, pushing, merging or flashing**, and before anything that
  rewrites C: on the Pico. A "ga" or "ja" covers only what was asked.
- **A test report after each phase** of a larger feature, before starting the next one.
- He tests on real hardware himself; tell him exactly what to do on the Atari and what
  to look for, then read the Pico console to check.
- Commit messages end with a `Co-Authored-By` line.

## Building and flashing (Windows)

See `docs/DEVELOPMENT.md`. In short: Pico SDK 2.2.0, `cmake -G Ninja -B build-pico2w
-DPICO_BOARD=pico2_w`, `ninja -C build-pico2w`. On the laptop the toolchain is in
`~/.pico-sdk` and ninja/cmake in `~/.espressif/tools`; on another PC check what is
installed first. The old `build/` folder (if present) is an RP2040 build: don't use it.

- `atari/mkdisk.py` (needs vasm) builds the boot code and driver and writes
  `disk_seed.h`, the C: image inside the firmware. The file dates on C: come from the
  last git commit of `atari/files/`, so `disk_seed.h` changes after such a commit:
  rebuild and commit it separately.
- Atari programs (`atari/sting/` ACSI_NET.STX, `atari/tools/` NETTEST.TTP) build with
  `m68k-atari-mint-gcc` in WSL. Copy new builds into `atari/files/` (= system files on C:).
- **Flashing:** USB console command `B` reboots the Pico into BOOTSEL; copy
  `build-pico2w/acsi2tnfs.uf2` to the RP2350 drive. Frank may have his own serial
  monitor open on the COM port: it must be closed first. If Windows does not see the
  Pico after `B`: hold the BOOTSEL button and replug.
- The console runs at any baud rate (USB). Useful keys: `h` help, `i` info, `v` verbose
  command log, `w` network bridge stats, `W` frame timeline, `n` network status,
  `N` rescan TNFS drives, `R` restore system files on C:, `F` rewrite C:, `H` hidden.
  A short press on the BOOTSEL button also toggles hidden mode (temporary, until a
  board with its own button).

## Things that cost a lot of time (don't repeat)

- **Read timing:** the dout PIO holds each byte ~130 ns after /ACK rises; less and the
  DMA chip sometimes latches the next byte. /DRQ must be streamed by DMA without gaps.
- **Never rewrite C: while the Atari runs** without the media-change path: GEMDOS keeps
  the old FAT cached. The driver reports media change (vendor sub 10) after `R`/`F`.
- **Drive letters stop at P:** (TOS has 16 drives). TOS 1.0 does not see the adapter;
  TOS 1.02/1.04 and EmuTOS work (EmuTOS uses its own ACSI driver, not ours).
- **The floppy shares the ACSI data bus and /IRQ (MFP GPIP5).** Anything on the ACSI side
  can break the floppy. Two real bugs: pull-ups on /IRQ and /DRQ to the board's own 5 V
  (removed), and ACSI_NET.STX taking `flock` with TAS on the high byte only (fixed in
  STX 00.07: whole word, interrupts off). Quick test: boot with hidden mode on (nothing
  loads from C:) or rename C:\AUTO\STING.PRG.
- **TNFS:** tnfsd drops idle sessions after 600 s; the firmware mounts again on status
  0xFF. A drive that fails shows NET_ERR.TXT and stays failed until console `N`.
- **STinG speed** (~70 KB/s with chzsoft TCP.STX 1.41) is limited by STinG polling every
  50 ms, not by the adapter.
- KiCad schematic files are CRLF; scripts that edit them must keep that.
- Shell heredocs mangle backslashes on this setup: write scripts with the Write tool.

## State (2026-10-11)

- `main`: firmware with TNFS read/write, network clock, STinG bridge (ACSI_NET.STX 00.07),
  hidden mode via BOOTSEL, `hardware/acsi-adapter` (DB19 to IDC20 adapter, outline and
  tracks still to do). Firmware still uses the pin map of the old **dev board** (data
  GP8-GP15, /OE GP6, DIR GP7, /IRQ and /DRQ through BC547s, high = active).
- `v2` (pushed, not merged into main): KiCad projects `hardware/v2` (DB19 on the board)
  and `hardware/v2-cable` (flat cable to a Lotharek adapter, pass-through), one circuit
  with a 74LS07 and a **different pin map** (see `hardware/README.md`). Stays off main
  until the firmware supports that pin map. No gerbers yet (Frank uses the KiCad plugin
  Fabrication Toolkit for JLCPCB).
- `acsi-net`: merged into main, kept for history.
- Sibling repos (same parent folder): `atari-net-tools` (URLVIEW.TTP: view, `-n` speed
  test, `-o` save to file) and `SideTNFS-Config` (ACSITNFS.PRG, branch `acsi-transport`,
  not merged).

## To do (agreed, roughly in this order)

1. Firmware board setting for `v2`/`v2-cable`: pin map (data bus reversed on GP6-GP13,
   DIR GP14, /OE GP15), /IRQ and /DRQ low = active, button GP26, status led GP28.
   Preferably one build that reads the board type from flash.
2. Atari TT: ACSI DMA only reaches ST-RAM. The driver needs a bounce buffer for buffers
   above 16 MB, and ACSI_NET.STX, ACSITNFS.PRG and NETTEST.TTP have program flags 7
   (load into TT-RAM): set them to 0. Then test on the TT and other ST models.
3. SD card (SPI0 on GP2-GP5): a PC-formatted card through the same virtual FAT (FatFs).
4. C: robustness: a CHKDSK-like console command, 4 KB write buffering on C:.
5. TNFS read cache; `pun_ptr` check next to AHDI/HDDRIVER; drop debug buffers.
6. First release after the tests, the SD card and the hardware.

## Test setup on the laptop (not on the PC)

The laptop had a TNFS test server (`C:\Users\frank\tnfstest`, patched spectrumero tnfsd,
`start-tnfsd.cmd`) and an HTTP test server (`start-http.cmd`, port 8000) used as F:.
At home Frank has his own TNFS server (192.168.178.10).
