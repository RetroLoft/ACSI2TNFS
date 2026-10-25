# The adapter in detail

## ACSI id

Every device on the ACSI bus needs its own id (0–7); two devices on one id answer at the
same time and neither works. A new adapter uses **id 6**, as an internal Mega ST disk, a
Megafile or an UltraSatan usually sits on 0.

**Mega STE and TT: use the first free id**, or the machine does not see the adapter:

| Machine | Id | Why |
|---|---|---|
| TT (TOS 3.06, AHDI) | **0** | only id 0 was asked at start-up; the TT's own disk is SCSI |
| Mega STE | **1** | the internal hard disk adapter is on id 0; with HDDRIVER switch id 1 on in HDDRUTIL |
| ST, STE, Mega ST | 6 (or any free id) | our boot code and AHDI/HDDRIVER ask every id |

Not sure which ids a machine asks? Console `s` (sniffer) logs every ACSI command with its
id; `t` goes back.

To change it:

- `ACSITNFS.PRG` → *Config → More → ACSI device ID*, then *Save* and restart.
- USB console: keys `0`–`7`. This always works, also during an id clash when the Atari
  cannot reach the adapter.

## Booting

The adapter's boot sector loads a small resident driver. It installs C: and the network
drives, sets the clock and makes C: the boot drive, so the AUTO folder and the desktop
come from C:.

- Another hard disk on a **lower** ACSI id boots first and keeps C: and the boot drive;
  the adapter's drives get the next free letters.
- **AHDI** and **HDDRIVER** mount the adapter's partitions themselves, with letters in
  order after your own partitions; the letters chosen in `ACSITNFS.PRG` only apply with
  the adapter's own driver. HDDRIVER only asks the ids switched on in HDDRUTIL, and does
  not wait for the network at start-up: TNFS drives show `NET_WAIT.TXT` until the
  adapter is connected. To keep the adapter out of the way, switch its id off in HDDRUTIL
  or hide it (below).
- **EmuTOS** mounts the partitions with its own ACSI driver as C:, D:, E:, …; the chosen
  letters and the network clock do not apply there.
- **TOS 1.0** does not see the adapter; TOS 1.02 and later do.
- **The clock** is set by the adapter's driver. Where that driver does not load (HDDRIVER
  on a Mega STE, a TT booting from its own disk), put `NETCLOCK.PRG` from C: in the AUTO
  folder: it asks the adapter for the network time and sets the clock.

## USB console

Connect the Pico to a PC and open its serial port in a terminal program (any speed).

| Key | |
|---|---|
| `i` | information: mode, id, command counters |
| `n` / `N` | network status / reconnect and re-read the TNFS drives |
| `w` | Atari network (STinG bridge): state, IP address, frame counters |
| `0`–`7` | ACSI id |
| `H` | hide / show the adapter (see below) |
| `K` | network clock on / off |
| `M` | removable unit on / off: with **HDDRIVER** a full network or SD drive then starts afresh without a reset. Leave it off with **AHDI** (its folders would stay empty) |
| `R` | put the system files back on C: (also new ones after a firmware update) |
| `F` | write C: anew: **all your files on C: are lost** |
| `B` | restart the Pico in BOOTSEL mode (for a firmware update) |
| `v` | verbose: log every ACSI command |
| `s` / `t` | sniffer mode (passive bus monitor) / target mode (normal) |
| `h` | help |

After `R` or `F` the driver tells GEMDOS that C: changed; no Atari reset needed.

## Hidden mode

With `H` the adapter ignores everything except its own configuration commands until it is
switched on again. The Atari then boots as if there were no adapter (e.g. a game that
must boot from floppy, or another hard disk driver). `ACSITNFS.PRG` still works.

Without a PC: a short press on the board's button switches hidden mode on or off. The
status led (D2) is on while the adapter is visible and blinks while it is hidden. The
change counts from the next Atari reset: press the button, reset the Atari, and the game
on the floppy starts by itself.

The other led (D1) flashes when the Atari reads or writes, like the led of a hard disk.
The Pico's own led shows Wi-Fi: blinking while it connects, on when connected, off when
it could not connect or Wi-Fi is not set up.

## System files on C:

`README.TXT`, `ACSITNFS.PRG`, `ACSI_NET.STX`, `STINGINS.TOS` and `NETCLOCK.PRG` come
with the firmware and are read-only. A new firmware updates them at start-up and keeps every other file on C:.
A system file that was deleted, or one that is new in the firmware, comes on C: with `R`.

## Troubleshooting

| Symptom | What to do |
|---|---|
| Atari does not see the adapter | ACSI cable, Pico powered, id clash (console `0`–`7`), TOS 1.0 |
| *Disk not usable, driver not loaded* | Console `R`; if C: is damaged, back up and `F` |
| C: full although files were deleted | A reset during a write can leave lost clusters on C:. Copy your files elsewhere, `F`, copy them back |
| "use FOLDR100.PRG" / folders do not open | TOS 1.x: `FOLDR100.PRG` in `C:\AUTO` |
| Drive letter not as configured | Letters above P: are not possible; a taken letter gives the next free one |
| Clock not set | Network clock on (`K`, ACSITNFS.PRG), Wi-Fi connected; ESC skips the wait at start-up |
| Network drive shows only `NET_ERR.TXT` | The reason is in the file (Show). Fix it, then console `N` or restart |
| Network drive empty | Still connecting at start-up, or console `n`: server reachable? folder correct? |
| Drive shows only `NET_WAIT.TXT` | The adapter is still connecting: open the drive again in a moment |
| SD drive shows only `SD_ERR.TXT` | No card, not FAT32/exFAT, or the folder is missing; fix it, console `N` and reset the Atari |
| Folders empty or only partly shown | The room of the drive is used up, often by *Show Info*, which opens every folder. Our driver: wait 2 s, close and open the folder again. HDDRIVER: the same with console `M` on, else reset. AHDI: reset (see [Empty folders](../README.md#empty-folders-on-a-network-or-sd-drive)) |
