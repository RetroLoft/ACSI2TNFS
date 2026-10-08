# The adapter in detail

## ACSI id

Every device on the ACSI bus needs its own id (0–7); two devices on one id answer at the
same time and neither works. A new adapter uses **id 6**, as an internal Mega ST disk, a
Megafile or an UltraSatan usually sits on 0.

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
- **AHDI** scans every id and would also mount the adapter's partitions; use the adapter
  next to HDDRIVER with the adapter's id switched off in HDDRUTIL, or hide the adapter
  (below).
- **EmuTOS** mounts the partitions with its own ACSI driver as C:, D:, E:, …; the chosen
  letters and the network clock do not apply there.
- **TOS 1.0** does not see the adapter; TOS 1.02 and later do.

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

Without a PC: a short press on the Pico's **BOOTSEL** button switches hidden mode on or
off. While the adapter is hidden, the Pico's own led blinks slowly (once Wi-Fi has
started). The change counts from the next Atari reset: press the button, reset the
Atari, and the game on the floppy starts by itself.

## System files on C:

`README.TXT`, `ACSITNFS.PRG` and `ACSI_NET.STX` come with the firmware and are
read-only. A new firmware updates them at start-up and keeps every other file on C:.
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
