# ACSI2TNFS - notes for Claude Code

A Raspberry Pi Pico 2 W that acts as an ACSI device for the Atari ST: a flash disk C:,
up to 3 virtual drives (D:-P:: TNFS network drives and folders on a micro SD card, which
also caches TNFS writes), a network clock (NTP, NETCLOCK.PRG) and a network function for
STinG (ACSI_NET.STX, installed with STINGINS.TOS). Configured from the Atari with
ACSITNFS.PRG. Firmware 0.4.0 (`version.txt`). User docs: `README.md`, `docs/*.md`;
developer docs: `docs/DEVELOPMENT.md`; hardware: `hardware/README.md`;
design and test reports (Dutch): `project-notities.md`, `ACSI_NET-ontwerp.md`.

## How Frank wants to work

- **Answer in Dutch.** Code, comments, commit messages and repo docs are in English.
- **KISS.** Simple solutions, no speculative features.
- **Ask before committing, pushing, merging or flashing**, and before anything that
  rewrites C: on the Pico. A "ga" or "ja" covers only what was asked.
- **A test report after each phase** of a larger feature, before starting the next one.
- He tests on real hardware himself; tell him exactly what to do on the Atari and what
  to look for, then read the Pico console to check.
- Commit messages end with a `Co-Authored-By` line.

## Building and flashing

See `docs/DEVELOPMENT.md`. In short: Pico SDK 2.2.0, `cmake -G Ninja -B build-pico2w
-DPICO_BOARD=pico2_w`, `ninja -C build-pico2w`. On another PC check what is installed
first. The old `build/` folder (if present) is an RP2040 build: don't use it.

- **Linux PC** (`~/retro/acsi2tnfs`): `PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
  PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1`, picotool in
  `~/.pico-sdk/picotool/2.2.0-a4/picotool/`, `VASM=~/tmp_toolchain_build/vasm/vasmm68k_mot`,
  `m68k-atari-mint-gcc` in `/usr/bin`. The Pico's console is
  `/dev/serial/by-id/usb-Raspberry_Pi_Pico*` (ttyACM number changes after a reset).
  Flash: console `B`, then `picotool load -x build-pico2w/acsi2tnfs.uf2`. In BOOTSEL
  `picotool save -r 0x10100000 0x10200000` reads the flash disk (read-only check of C:).
- **Laptop (Windows):** toolchain in `~/.pico-sdk`, ninja/cmake in `~/.espressif/tools`.
  Console `B`, copy the .uf2 to the RP2350 drive. Frank may have his own serial monitor
  open on the COM port: close it first. Not seen after `B`: hold BOOTSEL and replug.
- `atari/mkdisk.py` (needs vasm) builds the boot code and driver and writes
  `disk_seed.h`, the C: image inside the firmware. The file dates on C: come from the
  last git commit of `atari/files/`, so `disk_seed.h` changes after such a commit:
  rebuild and commit it separately. `.gitattributes` keeps README.TXT CR LF everywhere,
  so the image is the same on every platform.
- Atari programs (`atari/sting/` ACSI_NET.STX and PORTINFO.PRG, `atari/install/`
  STINGINS.TOS, `atari/tools/` NETTEST.TTP) build with `m68k-atari-mint-gcc`. Copy new
  builds into `atari/files/` (= system files on C:). Programs that do ACSI DMA must not
  load into TT-RAM: the Makefiles clear those flags (`m68k-atari-mint-flags`).
- The console runs at any baud rate (USB). Useful keys: `h` help, `i` info, `v` verbose
  command log, `w` network bridge stats, `W` frame timeline, `n` network status (with
  RSSI and TNFS retries), `N` rescan TNFS drives, `R` restore system files on C:, `F`
  rewrite C:, `H` hidden, `s`/`t` sniffer/target mode, `0`-`7` ACSI id, `c` SD card test
  (type, size, partitions), `o` /OE pull-up test (U1 as after a reset). A short press on
  the board's button (GP26) also toggles hidden mode. Leds: D1 (GP0) disk activity, on
  1 s after the last READ/WRITE; D2 (GP28) on, blinking while hidden; the Pico's own led
  (on the Wi-Fi chip) blinks while Wi-Fi connects, on when connected, off otherwise. Not the BOOTSEL button: reading it
  needs flash_safe_execute, which held core1 off ~150 us every 100 ms (the /ACK check of
  big reads then missed ~160 strobes; the data itself was fine).

## Things that cost a lot of time (don't repeat)

- **Read timing:** the dout PIO holds each byte ~130 ns after /ACK rises; less and the
  DMA chip sometimes latches the next byte. /DRQ must be streamed by DMA without gaps.
- **Never rewrite C: while the Atari runs** without the media-change path: GEMDOS keeps
  the old FAT cached. The driver reports media change (vendor sub 10) after `R`/`F`;
  other drivers (HDDRIVER) only see it after a reset (the adapter stays muted till then).
- **C: is FAT16** (4096 clusters of 1 sector, those past the flash marked bad): AHDI and
  HDDRIVER read every GEM partition as FAT16, our driver and EmuTOS go by the cluster
  count. A FAT12 C: was misread by HDDRIVER.
- **System files on C:** a firmware update replaces changed ones at start-up (before the
  console runs); a *new* one only comes with `R`. Don't interrupt a start-up: an erase cut
  off half-way once left FAT 2 empty (TOS reads FAT 2).
- **Drive letters stop at P:** (TOS has 16 drives). TOS 1.0 does not see the adapter;
  TOS 1.02/1.04, 2.06, 3.06 and EmuTOS work (EmuTOS uses its own ACSI driver, not ours).
- **Other drivers and machines:** HDDRIVER (Mega STE) assigns all letters itself, does
  not wait for the network at boot and only scans the ids enabled in HDDRUTIL; the TT
  (TOS 3.06 with AHDI) only asked ACSI id 0. Both work with the adapter. Sniffer mode (`s`) shows which ids a machine asks.
- **The floppy shares the ACSI data bus and /IRQ (MFP GPIP5).** Anything on the ACSI side
  can break the floppy. Two real bugs: pull-ups on /IRQ and /DRQ to the board's own 5 V
  (removed), and ACSI_NET.STX taking `flock` with TAS on the high byte only (fixed in
  STX 00.07: whole word, interrupts off). Quick test: boot with hidden mode on (nothing
  loads from C:) or rename C:\AUTO\STING.PRG.
- **TNFS:** folders are read when the Atari first reads them (lazy); new cluster runs only
  go into FAT sectors the Atari has not read since its reset; an Atari reset starts again
  from the root. tnfsd drops idle sessions after 600 s; the firmware mounts again on
  status 0xFF. A drive that fails shows NET_ERR.TXT until console `N`.
- **SD card** (SPI0: GP2 CLK, GP3 MOSI, GP4 MISO, GP5 CS; FatFs in `third_party/fatfs`,
  FAT32/exFAT, long names as 8.3): an SD drive is a folder of the card through the same
  virtual FAT as TNFS; it comes up before Wi-Fi and also without it; no card or folder:
  SD_ERR.TXT. The write side goes through `fs_*` (TNFS or FatFs per drive). The hidden
  sector file of a TNFS drive is `/.A2TCACHE/T<n>.TMP` on the card when there is one
  (writes ~280 KB/s instead of ~45 KB/s), else `/.A2T.TMP` on the server. Console `N`
  mounts the card again.
- **Frank's tnfsd (retroloft) leaks directory handles:** OPENDIR then answers 0x10
  (EMFILE) and drives look empty. Test OPENDIR from the PC; `sudo systemctl restart
  tnfsd` (or `tnfsd-public`) fixes it (Frank's sudo password needed).
- **Wi-Fi power saving must stay off** (`CYW43_NONE_PM`): in PM2 a reply waits for a
  beacon (60 ms average) and the 512-byte TNFS READs of one Atari read made HDDRIVER
  time out.
- **STinG 1.26 reads DEFAULT.CFG correctly only with CR LF:** with LF only it skips the
  line after every setting. STINGINS.TOS always writes CR LF. ACSI_NET.STX (00.08) takes
  its address from `ACSI_IP`/`ACSI_MASK` there; without them only STNGPORT.CPX in
  XControl sets the port.
- **`-mshort` (STX, installer):** `(uint32)p[2] << 8`, not `p[2] << 8`: an int shift
  sign-extends into the upper bytes.
- **STinG speed** (~70 KB/s with TCP.STX 1.41) is limited by STinG polling every 50 ms,
  not by the adapter.
- KiCad schematic files are CRLF; scripts that edit them must keep that.
- Shell heredocs mangle backslashes on this setup: write scripts with the Write tool.

## State (2026-10-24)

- One branch: `main` (`v2` and `acsi-net` were merged and deleted on 2026-10-24).
- Firmware: SD card drives (read and write) and the SD write cache for TNFS; NETCLOCK.PRG.
  C: as FAT16; TNFS lazy loading with READDIRX,
  NET_WAIT.TXT while connecting; Wi-Fi power saving off; ACSI_NET.STX 00.08 and
  STINGINS.TOS (STinG without XControl); ST-RAM builds for the TT. Tested on the Mega STE
  (TOS 2.06, HDDRIVER, adapter on ACSI id 1) and the TT (TOS 3.06, AHDI, ACSI id 0), and with
  TOS 1.02, 1.04 and EmuTOS (all on the old dev board). Since 2026-10-25 the firmware
  supports only the v2 pin map (data D7..D0 on GP6-GP13, reversed in the PIO with `::`
  and the DMA on byte 3 of the FIFO word; DIR GP14, /OE GP15; /IRQ and /DRQ through a
  74LS07, GPIO output inverted); not yet tested on a v2 board.
- Hardware: KiCad projects `hardware/v2` (DB19 on the board) and `hardware/v2-cable`
  (flat cable to a Lotharek adapter, pass-through), one circuit with a 74LS07 and one
  pin map (see `hardware/README.md`); both with a TF-01A micro SD socket. The dev board
  and v1 are no longer supported (in git history). No gerbers yet (Frank uses
  the KiCad plugin Fabrication Toolkit for JLCPCB). `hardware/v2-cable/case/`:
  3D-printable case (OpenSCAD `case.scad`, STLs; one bottom, two lids).
- The time zone is a fixed UTC offset (ACSITNFS.PRG), no automatic summer time: Frank's
  choice.
- v2 has R5 as a pull-up on /OE (`o` shows it): U1 is off while the Pico is in reset or
  BOOTSEL.
- Sibling repos (same parent folder): `atari-net-tools` (URLVIEW.TTP: view, `-n` speed
  test, `-o` save to file) and `SideTNFS-Config` (ACSITNFS.PRG, branch `acsi-transport`,
  not merged; its build still has TT-RAM program flags: patched on C: only).

## To do (agreed, roughly in this order)

1. Test the v2 firmware on the soldered v2-cable: console first, then sniffer, then
   target (boot from C:, write, TNFS, STinG).
2. TNFS lazy loading phase 2: at the ceiling (16 MB full or all FAT sectors read) a
   media change; for HDDRIVER through UNIT ATTENTION on TEST UNIT READY (tried once).
3. New system files on C: without `R` (sysfiles cannot tell new from deleted on purpose).
4. SD card: still to test on a PC, with exFAT, without Wi-Fi and under HDDRIVER; card
   swap while the Atari runs (now: `N` and a reset).
5. C: robustness: a CHKDSK-like console command.
6. TNFS read cache; `pun_ptr` check next to AHDI/HDDRIVER; drop debug buffers (~100 KB).
7. First release after the tests and the hardware.

## Test setup

At home Frank has his own TNFS servers on `retroloft` (192.168.178.10): the LAN one
(`/srv/samba/retro`, also the share `/mnt/retroloft/retro` on the PC) and the public
read-only one (`retroloft.net`, `/srv/samba/tnfs-public`: never put private files there).
`N:\STX008` (= `Atari.ST/STX008` on the LAN share) holds test builds. The laptop had a
TNFS test server (`C:\Users\frank\tnfstest`) and an HTTP test server (port 8000).
