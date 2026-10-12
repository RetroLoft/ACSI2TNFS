# STINGINS.TOS: STinG installer for ACSI2TNFS

`STINGINS.TOS` installs STinG for the adapter's network function, or changes only its IP
settings when STinG is there already (see `docs/NETWORK.md`). It carries the files below
(`mkpayload.py` turns them into `payload.c`) and is a system file on the adapter's C:.

```text
make            # STINGINS.TOS (m68k-atari-mint-gcc, -mshort, no C library)
cp STINGINS.TOS ../files/
```

## The files it carries (`sting/`)

Unchanged, as distributed; STinG is freeware ("released as FREEWARE for use and
distribution, but not as Public Domain", Peter Rottengatter):

| File | Version | From |
|---|---|---|
| `STING.PRG` | STinG 1.26 (01.06) | Peter Rottengatter, Ronald Andersson; [th-otto/STinG](https://github.com/th-otto/STinG) |
| `TCP.STX` | 01.41 | Christian Zietz, [chzsoft/sting](https://github.com/chzsoft/sting) (merged into th-otto/STinG) |
| `UDP.STX` | 01.46 | STinG 1.26 |
| `RESOLVE.STX` | 01.08 | STinG 1.26 |
| `PING.PRG`, `PING.RSC` | | STinG 1.26 tools |
| `DEFAULT.CFG` | | STinG 1.26 template, CR LF; the installer fills in `ACSI_IP`, `ACSI_MASK`, `NAMESERVER` |

`ACSI_NET.STX` comes from `../sting/` (this project, GPL-2.0-or-later).

## Notes

- STinG 1.26 reads `DEFAULT.CFG` correctly only with CR LF: with LF only it skips the line
  after every setting (`init_cfg` overwrites the line end it then looks for). The
  installer always writes CR LF, and `.gitattributes` keeps these files as they are.
- Compiled with `-mshort` (16-bit `int`, as STinG and `acsinet.c` expect): shifts of bytes
  into 32-bit values need a cast first, `(uint32)p[2] << 8`, or they sign-extend.
