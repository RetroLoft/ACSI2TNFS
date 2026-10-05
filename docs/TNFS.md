# Network drives (TNFS)

[TNFS](https://github.com/spectrumero/tnfsd/blob/master/tnfs-protocol.md) is a small file
protocol made for retro computers. The adapter connects to up to three TNFS folders and
shows each as a normal drive on the Atari.

## Setting up a drive

In `ACSITNFS.PRG` → *Config*, per drive:

- **Server:** host name or IP address of the TNFS server (port 16384 unless set otherwise).
- **Folder:** the folder on the server, e.g. `/` or `/ATARI.ST`.
- **Letter:** D: to P: (TOS knows 16 drives). If the letter is taken, the next free one
  is used and the start-up screen says so.

*Save*; after the restart the drives appear. Install them on the desktop like C:
(*Options → Install Disk Drive*).

## Running a TNFS server

Any TNFS server works. The reference server is
[tnfsd](https://github.com/spectrumero/tnfsd) (Linux, macOS, Windows):

```text
tnfsd /path/to/atari/files          read and write
tnfsd -r /path/to/atari/files       read-only
```

On Windows, build it with MinGW (`make OS=Windows_NT`). Allow it through the firewall for
the private network (UDP and TCP port 16384).

Keep the server folder **out of cloud sync folders** (OneDrive, Dropbox): they may lock
or move files while the adapter writes.

## How it works

The Atari only knows hard disks with sectors and a FAT. The Pico reads the whole folder
tree from the server at start-up and makes a FAT16 partition out of it (up to 16 MB per
drive); file data is fetched from the server when the Atari reads it.

**Writing.** GEMDOS writes sectors: first the data, then the FAT and the directory entry
when the file is closed. The Pico keeps every written sector in a hidden file
`.A2T.TMP` on the server and reads it back from there. About a second after the Atari
stopped writing, the Pico compares the drive with the server and applies new, changed,
renamed and deleted files and folders. A large file can take a while to reach the
server; reads and writes of the Atari go on in between.

## When a drive cannot be used

If there is no Wi-Fi, the server does not answer, its name cannot be found or it refuses
the folder, the drive holds one read-only file, **`NET_ERR.TXT`**. Show it on the
desktop: it gives the reason, the server and the folder. Once the server can be reached,
press `N` on the USB console or restart the adapter: the drive then reads its folders
and the Atari sees them right away.

If the server forgets the adapter's session (tnfsd does that after 10 minutes without
contact, and on a restart), the adapter logs in again by itself.

## What to keep in mind

- **Files added on the server** are seen after the Pico re-reads the tree: at start-up,
  or USB console `N`. Changes made by the Atari itself are seen at once.
- **Names** are shown in 8.3 form: long names on the server are shortened
  (`LONGFILENAME.TXT` → `LONGFILE.TXT`, on a clash `LONGFI~1.TXT`). Names starting with a
  dot are hidden.
- **Size:** at most 16 MB per drive and about 1000 files and folders for all drives
  together; what does not fit is left out (console `n` says "some skipped").
- **Read-only files:** files without write permission on the server show as read-only.
- **`.A2T.TMP`** grows until the Pico restarts. Delete it on the server only while the
  Pico is off.
- **Speed:** writing to the server is slower than reading (every written sector makes two
  trips over the network); copying 300 KB into a network drive takes a few seconds, the
  sync to the server about 7 s more.
- TNFS drives and C: both work while the network function (STinG) is in use.
