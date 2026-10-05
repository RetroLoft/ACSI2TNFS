# Internet on the Atari with STinG

The adapter's Pico is on your Wi-Fi network. With the STinG TCP/IP stack and the driver
`ACSI_NET.STX` the Atari gets its own IP address there: it can ping, use FTP, fetch web
pages and be reached from other computers.

```text
program (PING, gapFTP, CAB, ...)
  -> STinG (IP, TCP, UDP, DNS)
  -> ACSI_NET.STX (Ethernet frames, ARP)
  -> ACSI -> Pico -> Wi-Fi -> your router -> internet
```

The disk functions keep working at the same time: C: and the network drives.

## 1. What you need

| | Where |
|---|---|
| STinG 1.26 | [sting126.lzh](https://chebucto.ns.ca/Services/PDA/sting126.lzh) from the [Chebucto Atari page](https://chebucto.ns.ca/Services/PDA/AtariSTComm.shtml) |
| XControl 1.31 | [xctl131.zip](https://chebucto.ns.ca/Services/PDA/xctl131.zip) (same page) |
| `ACSI_NET.STX` | on drive C: of the adapter (read-only system file) |
| Firmware with the network function | 0.4.0 or later from the `acsi-net` branch |

`.lzh` archives can be unpacked on the Atari with LHarc, or on a PC with 7-Zip.

Older firmware: `ACSI_NET.STX` says *firmware without network function, not installed*
at start-up. After a firmware update the new files may be missing on C:: press `R` on
the USB console (see [ADAPTER.md](ADAPTER.md)).

## 2. Choose an IP address for the Atari

The Atari uses the **same MAC address as the Pico** but its **own IP address**. STinG has
no DHCP, so you pick one yourself:

- It must be **free**: no other device may use it.
- Take one **outside the range your router hands out** (DHCP). A FRITZ!Box hands out
  `.20`–`.200` by default, so for `192.168.178.x` pick e.g. `192.168.178.210`.
- Not the Pico's own address (the USB console shows it with `n`).

### The four addresses you need

| | What it is | Example | Where to find it |
|---|---|---|---|
| **Atari IP address** | the Atari's own address | `192.168.178.210` | you choose it (above) |
| **Subnet mask** | which part of an address says "same network" | `255.255.255.0` | PC: `ipconfig` (Windows) or the network settings, *Subnet Mask* |
| **Router** (gateway) | the way to the internet; also your name server | `192.168.178.1` | PC: `ipconfig`, *Default Gateway* |
| **Network address** | the name of your whole network, not a device | `192.168.178.0` | your IP address with the last number set to `0` (with mask `255.255.255.0`) |

Most home networks use mask `255.255.255.0`: all devices share the first three numbers
(`192.168.178.`), only the last one differs. The network address is then those three
numbers with `.0`: for `192.168.1.x` it is `192.168.1.0`, for `10.0.0.x` it is `10.0.0.0`.

The examples below use these values. Use your own.

## 3. Install STinG

From the STinG archive:

| File | Goes to |
|---|---|
| `AUTO\STING.PRG`, `AUTO\STING.INF` | `C:\AUTO` |
| `STING\TCP.STX`, `UDP.STX`, `RESOLVE.STX`, `DEFAULT.CFG`, `ROUTE.TAB`, `CACHE.DNS` | a folder `C:\STING` |
| `C:\ACSI_NET.STX` (from the adapter) | `C:\STING` as well |
| `TOOLS\PING.PRG`, `PING.RSC` | anywhere, e.g. `C:\STING\TOOLS` |

`STING.INF` contains the path of the STING folder (`C:\STING\`). The folder may also be
on a network drive (e.g. `F:\STING\`): the adapter's drives are there before the AUTO
folder runs. Leave out `SERIAL.STX` unless you use a modem.

## 4. Install XControl

- `XCONTROL.ACC` in the **root** of C: (accessories load from there only).
- The CPX modules in `C:\CPX`: from XControl at least `CONFIG.CPX`, from STinG
  `STING.CPX`, `STNGPORT.CPX` and `STNGPROT.CPX`.

Restart. At start-up STinG loads its modules; you should see

```text
ACSI_NET.STX 00.04: port ACSI2TNFS installed
```

## 5. Configure

**Port.** *Desk → Control Panel → STinG Port Setup*:

- port **ACSI2TNFS**
- IP address: your free address (`192.168.178.210`)
- subnet mask: `255.255.255.0`
- **Active** ticked, *Save*

When the port becomes active, the Pico switches its bridge on (USB console `w`:
*bridge on*).

**Routes.** `C:\STING\ROUTE.TAB` is STinG's signpost: for every packet it looks up
which way it has to go. Each line is *network, mask, port, gateway*, separated by tabs;
lines starting with `#` are comments. Replace the example line from the archive
(`... Modem 1 ...`) by these two:

```text
192.168.178.0	255.255.255.0	ACSI2TNFS	0.0.0.0
0.0.0.0		0.0.0.0		ACSI2TNFS	192.168.178.1
```

- **Line 1, your own network:** every address that starts with `192.168.178.` (your PC,
  the router, a NAS) is sent **directly**; gateway `0.0.0.0` means "no stop in between".
  Fill in your network address and subnet mask.
- **Line 2, everything else** (the internet): network `0.0.0.0` with mask `0.0.0.0`
  matches any address. It goes **via your router**: fill in the router's address as
  the gateway.

The order matters: STinG takes the first line that matches, so the general line comes
last.

**Name server.** The name server turns names like `info.cern.ch` into IP addresses
(DNS). At home that is almost always your router. In `C:\STING\DEFAULT.CFG`:

```text
NAMESERVER  = 192.168.178.1
THREADING   = 50
```

`THREADING` is in milliseconds: how often STinG polls the adapter. 50 is STinG's default
and costs about 1.5 % of the CPU; smaller values give a quicker response and cost more.

Restart once more, or use *STinG Port Setup* to activate the port.

## 6. Try it

- **Ping the router:** `PING.PRG`, host `192.168.178.1` (the default `127.0.0.1` is the
  Atari itself and does not use the network).
- **Ping the Atari from a PC:** `ping 192.168.178.210`. STinG answers by itself.
- **A web page:** URLVIEW from [Network tools for the Atari ST](https://github.com/RetroLoft/atari-net-tools).
- **FTP:** e.g. [gapFTP](https://atariuptodate.de/en/905/gapftp) (17 KB, command line):
  `GAPFTP.TTP ftp.funet.fi`.

## Speed and limits

- Measured over the local network: about **57 KB/s** with STinG's own `TCP.STX` 1.35
  and **69 KB/s** with the newer `TCP.STX` 1.41 by chzsoft
  ([github.com/chzsoft/sting](https://github.com/chzsoft/sting)); just replace
  `C:\STING\TCP.STX` with it. Recommended. The limit is how often STinG fetches the
  frames (every 50 ms), not the adapter.
- Saving is slower: with FTP about **24 KB/s** into a network drive and **11 KB/s**
  onto C: (C: is flash: every small write erases a 4 KB block).
- The Atari **cannot reach the Pico's own IP address** (the bridge drops such frames:
  they could not come back over Wi-Fi). Everything else on the network and the internet
  works.
- Some routers or mesh systems may not like two IP addresses on one MAC address. Home
  routers normally have no problem with it.
- STinG and XControl take memory and a timer interrupt. Some games do not start with
  them loaded (seen with Crystal Castles, also without `ACSI_NET.STX`): start those
  without STinG.

## Troubleshooting

| Symptom | Check |
|---|---|
| *no ACSI2TNFS found* at start-up | Adapter on, ACSI cable, ACSI id clash (see [ADAPTER.md](ADAPTER.md)) |
| *firmware without network function* | Firmware too old |
| *cannot pass frames (no Wi-Fi?)* | Wi-Fi not set up or not connected: `ACSITNFS.PRG`, console `n` |
| Port cannot be activated | IP address equal to the Pico's, or not a valid address/mask |
| Ping to the router fails | `ROUTE.TAB`, port active, console `w` (frames counted?) |
| Names do not resolve | `NAMESERVER` in `DEFAULT.CFG`, ping the router first |
| Download hangs at once | The target drive is full (Show Info on the drive) |

The USB console key `w` shows the bridge: on/off, the Atari's IP address and counters of
frames to and from the Atari (dropped, busy, errors).
