# ACSI_NET – STinG-netwerk via ACSI2TNFS (onderzoek en ontwerp)

Status: onderzoek en ontwerp, nog geen code. Datum: 4 oktober 2026.

Doel: een STinG-netwerkinterface via de ACSI-poort, als uitbreiding naast de bestaande disk- en TNFS-functie. Eerste mijlpaal: **PING vanaf een gewone ST via STinG → ACSI_NET.STX → ACSI2TNFS → Wi-Fi.**

```text
Atari-applicatie (PING, Litchi, CAB, ...)
  ↓
STinG (IP, ICMP, UDP, TCP, DNS)
  ↓
ACSI_NET.STX   ← nieuw: Ethernet-frames + ARP, praat ACSI
  ↓  vendor-commando's 0x11 'A' 'T' 0x2x
ACSI2TNFS firmware (core1: ACSI, core0: Wi-Fi/lwIP)   ← nieuw: frame-brug
  ↓
Wi-Fi (CYW43) → LAN / internet
```

Kernbeslissingen in één oogopslag:

1. **Frames, geen sockets.** De Pico doet geen TCP/IP namens de Atari. Hij brugt Ethernet-frames, net als PicoWifi. STinG doet IP, TCP en UDP, en de STX doet ARP, net als bij usbsting en EtherNEA.
2. **Eén MAC, twee IP's.** De Atari gebruikt het MAC-adres van de Pico. De Pico verdeelt binnenkomende frames op IP-adres: frames voor het IP van de Atari gaan naar de Atari, de rest naar lwIP (TNFS, NTP, DHCP).
3. **Transport via het bestaande vendor-commando** `[0x11|id<<5, 'A', 'T', sub, arg, 0]` met nieuwe subs 0x20–0x23. De disk-commando's veranderen niet.
4. **Core1 wacht nooit op core0.** Er komen wachtrijen voor RX en TX. Netwerkcommando's zijn even snel als de bestaande vendor-commando's (~0,5–1,5 ms) en raken het diskpad niet.
5. **De STX polt vanuit de STinG-thread en neemt de bus alleen met `tas flock`.** Is de bus bezet, dan slaat hij die beurt over, net als EtherNEA.

---

## A. Bevindingen over de huidige ACSI2TNFS-code

### A.1 Indeling over de cores

| Waar | Wat | Bestand |
|---|---|---|
| core1 | Realtime ACSI-target: commando ontvangen, uitvoeren, DMA, status | `acsi_core1.c`, `acsi_bus.pio` |
| core0 | USB-console, instellingen, Wi-Fi, lwIP, TNFS, NTP, configuratie-RPC, sysfiles | `acsi2tnfs.c`, `net.c`, `config.c`, `sysfiles.c` |
| Atari | Bootsector, residente driver (hdv_bpb/rw/mediach), ACSITNFS.PRG | `atari/boot.s`, `atari/driver.s`, `atari/acsi.inc` |

lwIP draait als `pico_cyw43_arch_lwip_threadsafe_background`. De cyw43-driver en lwIP worden dus vanuit een async context (IRQ-gestuurd, op core0) bediend, los van de hoofdlus van core0.

### A.2 Afhandeling van ACSI-commando's (core1)

- `target_loop` wacht op /CS met A1=0 (eerste commandobyte) en filtert op het eigen ID. In verborgen modus (`g_cfg.hidden`) en bij `mute_until_reset` gaat **alleen opcode 0x11** door. Netwerkcommando's als 0x11-sub werken dus ook als de adapter verborgen is.
- `handle_command` leest de CDB (6 bytes; ICD-uitgebreid 0x1F → 10/12 bytes). Na elke byte volgt /IRQ, dan `exec_cmd()`, dan `status_phase()`.
- `exec_cmd` verdeelt per opcode: TEST UNIT READY, REQUEST SENSE, INQUIRY, MODE SENSE/SELECT, READ CAPACITY, READ/WRITE(6) en (ICD) READ/WRITE(10), en **0x11 vendor**:

| sub | Richting | Inhoud |
|---|---|---|
| 5 | Atari → Pico, 512 B | configuratieverzoek (ACSITNFS.PRG) |
| 6 | Pico → Atari, 512 B | configuratieantwoord |
| 8 | Pico → Atari, 512 B | `ATL` gewenste driveletters |
| 9 | Pico → Atari, 512 B | `ATC` netwerktijd |
| 10 | Pico → Atari, 512 B | `ATG` media-change-tellers |
| anders | – | status 0x02, sense "invalid command" |

  Een onbekende sub geeft dus netjes CHECK CONDITION. Daarop rust de achterwaartse compatibiliteit (zie D.6).
- **DMA naar de Atari:** `dma_out2(a, alen, b, blen)` stroomt één of twee buffers zonder gat via RP-DMA naar de DOUT-statemachine. Dat is nodig vanwege de trage /DRQ (BC547) en de "volgende byte"-timing, gefikst met 50 ns hold.
- **DMA van de Atari:** `dma_in(buf, len)` start eerst RP-DMA en dan de DIN-SM. De hele overdracht gaat naar RAM.
- **Buffers:** `wbuf` (256 sectoren = 128 KB, voor disk-I/O), `reply[1024]` (vendor- en kleine antwoorden), `rxcopy`.
- **Logging:** elk commando gaat naar `g_cmd_log`. Met `verbose` gaat er ook een regel naar de console. Bij 20 netwerkpolls per seconde zou dat de console overspoelen; zie E.
- **TNFS-lezen en -schrijven** gaan nu via een handshake: core1 zet `vreq_*`, core0 (`net_poll`) doet het netwerkwerk, core1 wacht maximaal 9 s. Netwerkframes moeten **niet** op deze manier, anders wacht core1 op core0, die misschien seconden in een TNFS-sync zit.

### A.3 Netwerk op de Pico (core0)

- `net.c`: Wi-Fi-join (`wifi_connect`), DHCP of statisch, DNS, NTP via een eigen UDP-pcb, TNFS via eigen UDP/TCP-pcb's. De TNFS-requests lopen synchroon in de hoofdlus met `sleep_ms`-polls.
- Het CYW43-MAC is het enige MAC dat de Pico als Wi-Fi-station mag gebruiken (zie B.2).
- lwIP-opties (`lwipopts.h`): `MEM_SIZE 8000`, `PBUF_POOL_SIZE 24`, `LWIP_RAW 1`.
- **RAM:** ~432 KB van de 520 KB in gebruik. Grote debugbuffers: `la_buf` 32 KB, `ackcap` 33 KB, `ev_buf` 32 KB. Ruimte voor netwerkwachtrijen is er (~18 KB nodig), en die debugbuffers kunnen later kleiner.

### A.4 Atari-kant

- `atari/acsi.inc` `acsi_cmd`: zet `flock` (`st`), programmeert de DMA, stuurt 6 CDB-bytes met /IRQ-wacht (100 ms per byte, 10 s na de laatste) en leest de status. Daarna DMA-mode `$80` en `flock` vrij. Draait in supervisor-modus en gebruikt `_hz_200` voor timeouts.
- `driver.s` gebruikt dit voor disk-I/O en voor sub 8/9/10. `flock` wordt dus netjes gezet tijdens elk ACSI-commando. Precies wat een netwerkdriver nodig heeft om de bus veilig te delen.

---

## B. Bevindingen uit de referentieprojecten

### B.1 STinG 1.26 (th-otto/STinG)

- **Portdrivers werken op IP-niveau.** STinG zet uitgaande `IP_DGRAM`'s in `port->send` en roept `driver->send(port)` aan. De driver zet ontvangen datagrammen in `port->receive` (via `KRmalloc`). Een Ethernet-driver moet zelf **ARP** en de Ethernet-header doen. `IP_DGRAM` levert `hdr`, `options` en `pkt_data` los aan, plus `ip_gateway` (next hop).
- **Polling vanuit een interrupt.** `my_200_Hz` (`sting/thread.s`) telt `fraction` ticks af (standaard 10 → **elke 50 ms**; DEFAULT.CFG `THREADING` in milliseconden, STinG deelt door 5). Daarna draait `poll_ports` **in supervisor-modus, op het interruptniveau van de onderbroken code**, en alleen als die onder IPL 4 zat. `poll_ports` roept per poort `driver->receive` en daarna het verzenden aan. Gevolgen:
  - De STX kan midden in een diskcommando van GEMDOS terechtkomen. Hij **moet** `flock` controleren.
  - Tijdens de poll loopt `_hz_200` door (Timer C, IPL 6), dus timeouts op `_hz_200` werken.
  - Elke milliseconde in de poll is CPU-tijd die de voorgrond mist. Werk per poll begrenzen.
- Er is geen DHCP. IP, masker, gateway (ROUTE.TAB) en DNS stel je zelf in (STNGPORT.CPX, STNGPROT.CPX).

### B.2 PicoWifi (czietz/picowifi) – Pico-kant

- **Geen lwIP op de Pico.** De firmware overschrijft `cyw43_cb_process_ethernet` en zet elk binnenkomend frame in een wachtrij naar USB (16 × 1600 B). Uitgaande frames gaan rechtstreeks via `cyw43_send_ethernet()`.
- **De Atari gebruikt het MAC-adres van de Pico** (doorgegeven als USB-serienummer). Een Wi-Fi-station kan geen vreemde MAC's versturen (3-adresmodus). MAC-delen is dus de enige brug zonder NAT, en het werkt.
- `cyw43_wifi_pm(CYW43_PERFORMANCE_PM)`: geen energiebesparing, dus lage latentie. Dat is belangrijk voor ping en TCP.
- Wachtrijen tussen de cores met `pico/util/queue`; frame-overloop wordt stil gedropt.
- **Verschil met ons:** bij PicoWifi is de Atari de enige IP-gebruiker van het MAC. Bij ons heeft de Pico zelf ook een IP (TNFS/NTP). Daarom is er een **IP-filter** nodig (C.3).

### B.3 USBSTinG (czietz/usbsting, `USB_NET.STX`) – Atari-kant

- Een nette, moderne Ethernet-portdriver in C (m68k-atari-mint-gcc, `-mshort`, `-nostartfiles`). Hij stamt af van Roger Burrows' SCSILINK-driver (DaynaPort). **GPL-2 of later.**
- **Lagen:** `usbsting.c` (STinG-koppeling, `send_dgrams`/`receive_dgrams`, ARP, opbouw van `IP_DGRAM`) + `arpcache.c`. Alle hardware zit achter **twee functies**: `write_device(buf, len)` en `read_device(ENET_PACKET*)`. Dat is precies het punt waar ACSI in kan.
- `send_dgrams`: bouwt per datagram een volledig Ethernet-frame in een statische buffer (`ENET_PACKET op`) en verstuurt. Zonder ARP-antwoord komt het datagram in een `arpwait`-rij.
- `receive_dgrams`: `while (read_device() > 0)`, dan IP → `process_ip` (KRmalloc + kopie), ARP → `process_arp`. IP-broadcastframes worden genegeerd.
- Grootte: `USB_NET.STX` ≈ 12,7 KB.
- Let op: `allocmem` gebruikt `Mxalloc(…, 3)` (bij voorkeur TT-RAM). Voor ACSI-DMA moet het **ST-RAM** zijn (modus 0).

### B.4 EtherNEA / EtherNEC (Thomas Redelberger)

- Een NE2000 aan de ACSI-bus, maar **zonder DMA**: registertoegang via losse ACSI-bytes. Ook polling, ook vanuit de STinG-thread.
- **Busarbitrage (BUSENEAF.I):** `tas flock` en `tas flock+1`; is de bus bezet, dan **doet de driver deze beurt niets** en probeert het bij de volgende poll opnieuw. Na afloop `clr.w flock` en DMA-mode `$80`. Het advies uit ARCHITEC.TXT geldt ook voor ons: de harddiskdriver moet `flock` netjes gebruiken, anders dreigt dataverlies.
- Geen interrupt: de MFP-ingang voor ACSI-/IRQ is flankgevoelig en wordt gedeeld met de disk, dus interrupts zijn onpraktisch. Polling is de juiste keus.
- Over DMA: "transfers in veelvouden van 512 bytes (512/1024/1536)". Dat bevestigt de keuze voor 3 sectoren per frame. Bij STinG is dubbel bufferen nodig, omdat het datagram niet aaneengesloten is (header, options en data los).
- ARP in de STX (`ENESTNG.C`), cache van 32 entries; dezelfde opzet als bij usbsting.
- ENEAF.STX ≈ 6 KB (assembler). Een C-STX op basis van usbsting wordt ~10–12 KB.

### B.5 DaynaPort SCSI/Link (commandoset)

- Read(08): het antwoord begint met **lengte (2 B) + vlag (4 B, "meer pakketten")**, daarna het frame. Write(0A): één frame. Aparte commando's voor MAC, statistieken en aan/uit.
- Dit model nemen we over: **één frame per commando, met een "er wacht nog meer"-teller in de header**, zodat de driver in één poll kan doorlezen.

### B.6 Nieuwere TCP.STX en de 1040STf

- PicoWifi raadt het bijgewerkte `TCP.STX` van chzsoft aan (compatibiliteitsprobleem met sommige hosts). Dat geldt voor ons net zo.
- Op een 1 MB 1040STf werkt STinG 1.26 als je ongebruikte STX'en weglaat en `ALLOCMEM` verlaagt (50–60 KB). Onze STX kost ~12 KB code + ~4 KB buffers, en dat past.

---

## C. Voorgestelde architectuur

### C.1 Vergelijking van de opties voor de Pico-kant

| Optie | Hoe | Plus | Min |
|---|---|---|---|
| **1. Frame-brug, gedeeld MAC, IP-filter** (voorstel) | Atari krijgt eigen IP op het Pico-MAC; Pico verdeelt frames op IP | Atari volwaardig in het LAN (inkomende verbindingen, FTP-server); STX = beproefd usbsting-model; Pico doet weinig | Twee IP's op één MAC; IP van de Atari vast instellen (later DHCP) |
| 2. NAT/router op de Pico | Atari in een virtueel subnet, Pico routeert/NAT | Geen IP in het LAN nodig | lwIP heeft geen NAT; inkomend verkeer vraagt portforwarding; veel code |
| 3. Socket-proxy (Pico doet TCP) | STX vertaalt STinG-calls naar Pico-sockets | Atari ontlast | Gaat tegen de eis in (STinG doet TCP/IP); grote, foutgevoelige laag |
| 4. IP-niveau (Pico doet ARP) | STX stuurt kale IP-datagrammen; Pico maakt Ethernet | Kleinere STX | Pico moet proxy-ARP en next-hop doen; afwijkend van alle referenties |

**Keuze: optie 1.** Die sluit aan bij PicoWifi (Pico) en usbsting/EtherNEA (Atari), en de Pico hoeft niets van TCP/IP te weten behalve een filter.

### C.2 Datastromen

```text
Wi-Fi RX:  CYW43 → cyw43_cb_process_ethernet → pbuf → netif->input
                                                         │
                                         net_input_filter (nieuw)
                         ┌───────────────┬───────────────┴──────────────┐
                    voor Atari      voor beide (ARP-reply,        voor Pico
                         │           ARP-broadcast?)                    │
                  RX-ring (core0 → core1)                       ethernet_input (lwIP)
                         │
            core1: NET_RX → DMA → Atari → ACSI_NET.STX → STinG

Atari TX:  STinG → ACSI_NET.STX → NET_TX → core1: DMA in → TX-ring (core1 → core0)
                         │
          async-worker op core0 (lwIP-context) → cyw43_send_ethernet()
```

### C.3 Filterregels (Pico, `net_input_filter`)

Alleen actief als de Atari het netwerk heeft aangezet (NET_CTRL enable, met zijn IP). Anders gaat alles ongewijzigd naar lwIP: **zonder STX is er geen gedragsverandering.**

| Binnenkomend frame | Naar |
|---|---|
| IPv4, doel-IP = IP van de Atari | Atari |
| ARP, doel-IP (TPA) = IP van de Atari | Atari |
| ARP-reply aan ons MAC, TPA = IP van de Pico | lwIP |
| ARP-request (broadcast) voor een ander IP | lwIP (de Atari hoeft dat niet te weten) |
| IPv4-broadcast/multicast | lwIP (usbsting negeert IP-broadcast toch) |
| Overig (IPv6, …) | lwIP |

**Uitgaand van de Atari:** het bron-MAC moet ons MAC zijn (de Pico controleert dat). Frames naar het **eigen IP van de Pico** kunnen niet via Wi-Fi terugkomen. In fase 1 worden ze genegeerd; later eventueel via een lokale lus naar lwIP (zie E).

### C.4 Pico: wachtrijen en threads

- **RX-ring** (core0 vult, core1 leegt): 8 × 1536 B = 12 KB, lock-free single-producer/single-consumer met geheugenbarrières. Vol → frame droppen, teller ophogen.
- **TX-ring** (core1 vult, core0 leegt): 4 × 1536 B = 6 KB. Vol → NET_TX geeft status BUSY (0x08), de STX houdt het datagram vast.
- **TX-verzending** door een `async_when_pending_worker` in de cyw43-async context: core1 roept `async_context_set_work_pending()` aan. De worker draait in de lwIP/cyw43-context (lock wordt vastgehouden) en roept `cyw43_send_ethernet()`. Daardoor gaat TX ook door als de hoofdlus van core0 seconden in een TNFS-sync zit.
- **core1** handelt NET_RX/NET_TX volledig zelf af: geen `vreq`-handshake, geen wachten op core0. De duur van het commando is alleen de ACSI-overdracht.
- **Bij /RESET** (bestaande handler op core1): netwerkbrug uit, ringen leeg. De Atari is weg, dus frames voor zijn IP worden niet meer opgespaard; lwIP krijgt weer alles.
- `cyw43_wifi_pm(CYW43_PERFORMANCE_PM)` zodra de brug aanstaat (of altijd), voor lage latentie.

### C.5 Atari: ACSI_NET.STX

- Gebaseerd op **usbsting** (`usbsting.c`, `arpcache.c`, STinG-headers), met de USB-laag vervangen door `acsinet.c` / `acsinet.S`:
  - `write_device(buf,len)` → NET_TX
  - `read_device(pkt)` → NET_RX
  - `open_device` → NET_INFO + NET_CTRL enable (IP en masker van de poort)
  - `close_device` → NET_CTRL disable
- **Busarbitrage per poll:** `tas flock` (en `flock+1`, zoals EtherNEA). Bezet → deze poll niets doen. Na afloop DMA-mode `$80` en `flock` vrij, net als `acsi_cmd`.
- **Werk per poll begrensd:** maximaal bijvoorbeeld 4 frames RX en 4 TX, en stoppen als de pending-teller 0 is.
- **Korte timeouts** (bijv. 20 ms per commandobyte, 100 ms totaal). Bij een timeout: poort-statistiek ophogen, deze poll stoppen, `flock` vrijgeven.
- **Buffers in ST-RAM** (`Mxalloc(…, 0)`, of BSS op een ST): TX-frame 1536 B, RX-frame 1536 B, op een even adres.
- MAC = MAC van de Pico (uit NET_INFO). `CTL_ETHER_SET_MAC` wordt geweigerd.
- Poortnaam bijvoorbeeld **"ACSI2TNFS"**. Detectie: zoek het ACSI-ID via de `_bootdev`/driver-data of probeer ID 0–7 met NET_INFO.

---

## D. Voorgesteld ACSI-netwerkprotocol (versie 1)

### D.1 Waarom het bestaande vendor-commando

- Opcode 0x11 met `'A' 'T'` is al ons herkenbare "protocol-ID". Het komt door de verborgen modus heen, en een onbekende sub geeft netjes CHECK CONDITION.
- De disk-opcodes (READ/WRITE(6/10)) blijven onaangeroerd: geen kans dat een netwerkframe als sector wordt gezien.
- Een uitgebreid ICD-commando (0x1F) is niet nodig: 6 bytes CDB is genoeg, en niet elke host-DMA-routine kan ICD.
- Versie en capabilities staan in het NET_INFO-antwoord (magic `ATN` + versie). Zo kunnen STX en firmware los van elkaar evolueren.

### D.2 CDB

```text
byte 0: 0x11 | (id << 5)
byte 1: 'A'
byte 2: 'T'
byte 3: sub            0x20..0x2F = netwerk
byte 4: arg            aantal sectoren (TX), maximum aantal sectoren (RX)
byte 5: 0              (gereserveerd, later vlaggen)
```

### D.3 Commando's

| sub | Naam | Richting, sectoren | Inhoud |
|---|---|---|---|
| 0x20 | NET_INFO | Pico → Atari, 1 | `"ATN"`, protocolversie (1), capabilities, toestand (Wi-Fi, brug aan), MAC[6], IP Pico, IP/masker van de Atari (zoals ingesteld), MTU (1500), maximaal aantal sectoren RX/TX, ringgroottes, tellers (rx/tx/drop), firmwareversie-string |
| 0x21 | NET_CTRL | Atari → Pico, 1 | `"ATN"`, versie, opdracht: 1 = brug aan (IP, masker), 0 = uit. Status 0 = ok; 0x02 + sense bij een fout (bijv. IP gelijk aan dat van de Pico, geen Wi-Fi) |
| 0x22 | NET_TX | Atari → Pico, `arg` = 1..3 | Header + frame (D.4). Status 0 = aangenomen, **0x08 BUSY** = TX-ring vol (later opnieuw), 0x02 = fout (lengte) |
| 0x23 | NET_RX | Pico → Atari, `arg` = maximum (3) | Header + 0 of 1 frame. **De Pico stuurt zo weinig sectoren als nodig:** 1 als er niets is, anders ⌈(8+len)/512⌉ |

### D.4 Frameformaat (in de DMA-buffer, big-endian zoals de 68000)

```text
offset 0  u16  frame_len       0 = geen frame (alleen bij RX)
offset 2  u16  pending         RX: frames die daarna nog wachten; TX: 0
offset 4  u16  dropped         RX: teller (laagste 16 bits) gedropte frames
offset 6  u16  flags           0 (gereserveerd: checksum, batch)
offset 8  ..   Ethernet-frame  dest MAC, src MAC, type, payload (zonder FCS)
```

8 + 1514 = 1522 B past in **3 sectoren (1536 B)**. De rest van de laatste sector is opvulling. Altijd hele sectoren: de DMA-chip van de ST werkt met een FIFO van 16 bytes, dus een halve sector kan in de FIFO blijven hangen.

### D.5 Ruimte voor later (capabilities)

- **Batching:** meerdere kleine frames (ARP, TCP-ACK) in één NET_RX/NET_TX van 1–N sectoren. Header per frame, een `nframes`-veld in `flags`. Dat scheelt commando-overhead bij veel kleine pakketten.
- **Checksum** over header en frame (16 bit) als vlag. IP/ICMP/UDP/TCP hebben al end-to-end-checksums, dus voor fase 1 niet nodig.
- **DHCP namens de Atari** (NET_INFO levert dan IP, masker, gateway en DNS).

### D.6 Achterwaartse compatibiliteit

| Combinatie | Gedrag |
|---|---|
| Nieuwe firmware, geen STX | Brug staat uit; filter geeft alles aan lwIP; er verandert niets |
| Nieuwe firmware, oude driver/ACSITNFS.PRG | Die kennen sub 0x2x niet en gebruiken ze ook niet; er verandert niets |
| Oude firmware, nieuwe STX | NET_INFO → status 0x02 → STX meldt "ACSI2TNFS zonder netwerkfunctie" en installeert zich niet |
| Andere protocolversie | STX controleert versie en capabilities in NET_INFO; onbekende hogere versie: alleen v1-functies gebruiken |

---

## E. Risico's en mogelijke problemen

| # | Risico | Gevolg | Aanpak |
|---|---|---|---|
| 1 | **flock-discipline van andere drivers** | Een driver die `flock` niet zet, kan midden in een STX-poll de DMA overnemen → corrupte disk-I/O | Onze driver, AHDI en HDDRIVER zetten `flock`. Documenteren (zoals EtherNEA). STX alleen met `tas` |
| 2 | **Poll in interruptcontext** | Lange ACSI-commando's houden de voorgrond vast | Werk per poll begrenzen; korte timeouts; geen BIOS/GEMDOS in de poll |
| 3 | **Korte DMA-read** (1 sector sturen terwijl de Atari er 3 programmeerde) | Werkt naar verwachting (hele sectoren, status beëindigt), maar **nog niet getest** | Als eerste testen in fase 1. Valt het tegen: NET_RX altijd 3 sectoren, of eerst een statusvraag |
| 4 | **Twee IP's op één MAC** | Sommige routers/AP's (ARP-spoofing-bescherming, mesh, client isolation) vinden dat raar | Gebruikelijke en legitieme situatie (IP-aliassen). Testen op het eigen netwerk; documenteren |
| 5 | **DHCP voor de Atari** | STinG heeft geen DHCP; IP handmatig, conflict met het DHCP-bereik mogelijk | Fase 1: vast IP buiten het DHCP-bereik. Later: Pico vraagt een lease aan met een eigen client-ID |
| 6 | **Atari → IP van de Pico** | Komt niet terug via Wi-Fi | Fase 1: niet ondersteund (gedocumenteerd). Later: lokale lus in het filter |
| 7 | **Wi-Fi-energiebesparing** | Ping-RTT 100+ ms, trage TCP | `CYW43_PERFORMANCE_PM` |
| 8 | **Console-logging** | 20 polls/s overspoelen USB en `ev_buf` | Net-subs niet loggen in verbose; aparte teller en console-toets voor netstatistiek |
| 9 | **RAM op de Pico** | ~88 KB vrij; ringen ~18 KB | Past. Eventueel `la_buf`/`ackcap`/`ev_buf` verkleinen (stond al op de lijst) |
| 10 | **TNFS-verkeer en netwerk tegelijk** | Wi-Fi-airtime wordt gedeeld; een TNFS-sync blokkeert de hoofdlus van core0 | RX/TX via de async context, los van de hoofdlus. Netwerkthroughput daalt tijdens een grote sync, maar loopt niet vast |
| 11 | **ACSI-datafouten** | Geen FCS over ACSI | IP-/ICMP-/UDP-/TCP-checksums vangen het op; later optionele checksum (D.5) |
| 12 | **TT (68030-cache, TT-RAM)** | DMA-buffer in TT-RAM onmogelijk; verouderde cache na DMA-read | Buffers met `Mxalloc(…, 0)`; op de TT cache leegmaken na NET_RX. Doel is eerst de gewone ST |
| 13 | **Licentie** | usbsting en EtherNEA zijn GPL | ACSI_NET.STX als GPL-2+-programma (eigen map, eigen LICENSE). De firmware blijft los |
| 14 | **TCP.STX-bug** | Sommige servers geven problemen | Bijgewerkte TCP.STX van chzsoft aanbevelen |
| 15 | **Atari-reset met STinG actief** | Ringen houden oude frames | Brug uit en ringen leeg bij /RESET (core1) |

Throughput, ruwe schatting: een NET_RX/NET_TX van 3 sectoren kost ~1,2–1,5 ms ACSI (gemeten: 4 KB in 2,8 ms). Het plafond is ruim 500 KB/s, maar in de praktijk begrenst STinG op een 8 MHz 68000 het tot enkele tientallen KB/s, en de pollinterval (50 ms standaard, 10 ms met THREADING = 10) bepaalt de latentie. Elke lege poll kost ~0,5 ms, dus ~1 % CPU bij 50 ms.

---

## F. Gefaseerd implementatieplan

Aangepast ten opzichte van het voorstel: RX en TX worden samen getest (een frame versturen zonder iets terug te ontvangen is lastig te controleren), en er komt een klein testprogramma **vóór** STinG, zodat transportproblemen niet verward raken met STinG-configuratie.

| Fase | Inhoud | Klaar als |
|---|---|---|
| **1. Transport en detectie** | Firmware: NET_INFO + NET_CTRL (alleen opslaan, nog geen filter), netstatistiek op de console, net-subs niet in verbose. Atari: `NETTEST.TTP` (C, m68k-atari-mint-gcc) die het adapter-ID vindt, NET_INFO toont en de korte DMA-read test (risico 3) | NETTEST toont MAC, IP van de Pico en versie; oude firmware geeft een nette melding |
| **2. Frame-brug op de Pico** | Filter in `netif->input`, RX- en TX-ring, async-worker voor TX, NET_TX/NET_RX op core1, reset-afhandeling, `PERFORMANCE_PM`. NETTEST: ARP-request naar de router sturen en het ARP-antwoord ontvangen; een ARP-request van de laptop (`arping`) zien binnenkomen | ARP heen en terug; TNFS-drives werken ongestoord (kopieertest zoals eerder) |
| **3. ACSI_NET.STX-skelet** | usbsting-structuur overnemen, USB eruit, `acsinet.S` (ACSI met `tas flock`, korte timeouts), installeren in STinG, poort "ACSI2TNFS" zichtbaar in STNGPORT.CPX, aan/uit → NET_CTRL | Poort zichtbaar en activeerbaar; geen invloed op disk-I/O (kopiëren tijdens actieve STinG) |
| **4. ARP en ICMP: eerste PING** | `send_dgrams`/`receive_dgrams` + ARP-cache actief; ROUTE.TAB | `ping <atari-ip>` vanaf de laptop werkt; PING.PRG op de Atari naar de router werkt |
| **5. UDP en TCP** | DNS via RESOLVE.STX, een FTP-client (Litchi) of CAB; bijgewerkte TCP.STX; stresstest met gelijktijdige disk- en TNFS-I/O | Bestand downloaden en tegelijk van C: en een TNFS-drive lezen zonder fouten |
| **6. Verfijning** | Batching (D.5), THREADING-advies, gebruik op een 1 MB 1040STf, IP van de Atari instellen via ACSITNFS.PRG of DHCP namens de Atari, ACSI_NET.STX als systeembestand, documentatie | Gebruiksklaar voor anderen |

Elke fase laat de bestaande functie intact: zonder actieve STX is alleen de netstatistiek op de console nieuw.

---

## G. Bestanden die er uiteindelijk bij komen of veranderen

### Firmware (Pico)

| Bestand | Wijziging |
|---|---|
| `netbridge.c` (nieuw) | RX/TX-ringen, `net_input_filter`, TX-worker, enable/disable, statistiek |
| `acsi_core1.c` | `case 0x20..0x23` in de bestaande `0x11`-switch; ring legen bij /RESET; net-subs niet in verbose-log |
| `net.c` | Na de Wi-Fi-join het filter installeren (`netif->input` omleiden), MAC/IP beschikbaar maken voor NET_INFO, eventueel `cyw43_wifi_pm` |
| `acsi.h` | Prototypes en statistiekstruct |
| `acsi2tnfs.c` | Console-toets voor netstatistiek (bijv. `w`) |
| `CMakeLists.txt` | `netbridge.c` toevoegen |
| `lwipopts.h` | Waarschijnlijk niets; eventueel `PBUF_POOL_SIZE` |
| `config.c` | Pas in fase 6: IP van de Atari instelbaar via ACSITNFS.PRG |

### Atari

| Bestand | Inhoud |
|---|---|
| `atari/sting/` (nieuw, GPL-2+) | `acsi_net.c` (op basis van `usbsting.c`), `arpcache.c/.h` (usbsting), `acsinet.S` (ACSI-commando met `tas flock`, afgeleid van `atari/acsi.inc`), `include/port.h`, `include/transprt.h`, `Makefile` (m68k-atari-mint-gcc `-mshort`), `LICENSE` |
| `atari/tools/nettest.c` (nieuw) | Testprogramma voor fase 1–2 |
| `atari/files/` | Later `ACSI_NET.STX` als systeembestand op C: (fase 6) |
| `atari/driver.s`, `atari/acsi.inc` | Geen wijziging nodig: zetten `flock` al |

### Documentatie

| Bestand | Wijziging |
|---|---|
| `README.md`, `atari/files/README.TXT` | Netwerkfunctie, STinG-installatie, ROUTE.TAB-voorbeeld, beperkingen |
| `project-notities.md` | Testresultaten per fase |
| Dit document | Bijwerken als ontwerpkeuzes veranderen |

---

## Referenties

- STinG 1.26: https://github.com/th-otto/STinG (`sting/thread.s`: 200 Hz-thread; `sting/kernel.c`: `poll_ports`)
- PicoWifi: https://github.com/czietz/picowifi (`picowifi.c`)
- USBSTinG: https://github.com/czietz/usbsting (`driver/usbsting.c`, `driver/arpcache.c`)
- EtherNEA/EtherNEC: https://web222.webclient5.de/prj/atari/etherne/index.htm (`SRC/ARCHITEC.TXT`, `SRC/ENESTNG.C`, `SRC/BUSENEAF.I`)
- DaynaPort SCSI/Link: https://github.com/piscsi/piscsi/wiki/Dayna-Port-Command-Set
- Bijgewerkte TCP.STX: https://www.chzsoft.de/storage/TCP.STX
- STinG op een 1040STf: https://chebucto.ns.ca/Services/PDA/stfsting.txt
