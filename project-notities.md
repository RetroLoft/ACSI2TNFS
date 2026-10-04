# ACSI2TNFS – projectnotities

## Hardware: /DRQ (Q2, BC547) laat te traag los

Probleem: na het loslaten blijft /DRQ ongeveer 1 µs laag, omdat de BC547 verzadigd is. De firmware vangt het op, maar elektrisch valt het buiten de specificatie.

Mogelijke wijzigingen, in volgorde van voorkeur:

1. **100 pF parallel aan R2** (4k7 naar de basis van Q2). Eventueel tot 220 pF; een 0805 kan bovenop R2.
2. **Schottky-diode** (BAT85/BAT43): anode aan de basis, kathode aan de collector van Q2. Doet hetzelfde, nog robuuster.
3. **Volgende PCB:** een open-drain buffer (74LVC1G07 / 74LS07) in plaats van de transistoren. **v1 testen?** (v1 heeft al een 74LS07)

Niet doen: de BC547 vervangen door een ander bipolair type (zelfde probleem) of door een 2N7000/BS170 (drempel te hoog voor 3,3 V).

R5 (IRQ) hoeft niet; daar is de timing ruim.

**Controle na de wijziging:** in de Pico-log geeft een read nu `acks=514`. Na een geslaagde wijziging moet dat `acks=512` zijn.

## Hardware: U1 (databus) moet uit staan zolang de Pico niet draait (R7, R8, R9)

In het schema (`hardware/dev`) zitten drie weerstanden van 100 kΩ, die op de huidige print niet geplaatst zijn:

| | Net | Naar | Bedoeling |
|---|---|---|---|
| R7 | /RESET_3V3 (U2 → GP17) | GND | /RESET gedefinieerd houden |
| R8 | /OE van U1, de databus (GP6) | GND | U1 aan tijdens het opstarten van de Pico |
| R9 | DIR van U1 (GP7) | +5V | richting Atari → Pico tijdens het opstarten |

Ze doen ertoe op het moment dat de Pico **niet** onze firmware draait: bij het aanzetten, bij een reset en in BOOTSEL (flashen). Dan zijn GP6/GP7 ingangen.

**Probleem zonder R8/R9:** de RP2040 en RP2350 zetten na een reset een **interne pull-down** (~50 kΩ) op hun GPIO's. Dan is /OE laag (U1 aan) en DIR laag (richting **Pico → Atari**). U1 drijft dan de ACSI-datalijnen van de Atari laag zolang de Pico in reset of BOOTSEL staat. Valt er op dat moment een ACSI-overdracht (de DMA-chip stuurt een commando, een andere schijf antwoordt), dan vechten twee uitgangen tegen elkaar. Dat het bij het vele flashen met de Atari aan goed ging, komt doordat de Atari toen zelden de bus gebruikte.

**R9 naar +5 V is geen goede oplossing:**

1. **Te zwak:** 100 kΩ tegen de interne pull-down van ~50 kΩ geeft ~1,7–2,2 V op DIR, net geen geldige "hoog" voor de LVC245.
2. **Verkeerde spanning:** DIR hangt ook aan GP7. 5 V via een weerstand op een GPIO is op de RP2040 buiten de specificatie (stroom met 100 kΩ heel klein, maar niet netjes), en staat er ook als de Atari aan is en de Pico niet.

**Advies voor de print:**

- **R8 als pull-up van 10 kΩ naar +3,3 V** (in plaats van een pull-down naar GND). Zolang de Pico niet draait staat U1 dan **uit**: de bus van de Atari is niet aangesloten, in welke stand dan ook. De firmware zet U1 bij het opstarten zelf aan (/OE laag, DIR hoog), daar verandert niets aan.
- **R9 vervalt** zodra /OE gegarandeerd uit staat: de richting doet er dan niet toe. Houd je hem toch, dan 10 kΩ naar **+3,3 V**.
- **R7 mag weg:** U2 staat altijd aan en drijft /RESET op GP17 zodra de 3,3 V er is.

**Op de huidige print:** een 10 kΩ van GP6 (/OE, U1 pin 19) naar 3,3 V is de belangrijkste. R7 en R9 leeg laten.

## ACSI_NET fase 1: testverslag (4 oktober 2026)

Ontwerp: `ACSI_NET-ontwerp.md`. Getest op de ST (TOS 1.04) met `NETTEST.TTP` van een TNFS-drive (log in `NETTEST.LOG`) en de Pico-log, adapter op ACSI-ID 6.

| Test | Resultaat |
|---|---|
| Oude firmware (zonder netwerkfunctie) | NET_INFO → status 2 → "Network function not supported" |
| NET_INFO | protocol 1, MAC, IP/masker/gateway van de Pico, Wi-Fi-status kloppen |
| **Korte DMA-read**: Atari programmeert 3 sectoren, Pico stuurt er 1, 2 of 3 | 2300 rondes per grootte (6900 commando's): 0 fouten, DMA-status steeds ok, niets geschreven voorbij de data |
| Sector 0 lezen tussen elke ronde (diskpad) | 2300 keer, 0 fouten |
| NET_CTRL aan (192.168.178.50/24) | opgeslagen, zichtbaar in NET_INFO en console `w` |
| NET_CTRL met het IP van de Pico | geweigerd (status 2, sense $24, resultaat 4); vorige instelling blijft |
| Onbekende sub $2E | geweigerd (status 2, sense $20) |
| NET_CTRL uit | opgeslagen |

Duur per commando (Atari, inclusief Supexec; Pico-kant tussen haakjes): 1 sector ~0,74 ms (0,50 ms), 2 sectoren ~1,13 ms (0,83 ms), 3 sectoren ~1,50 ms (1,18 ms). De Pico telde steeds 2 /ACK's meer dan bytes (n×512+2), hetzelfde als bij disk-reads (trage /DRQ, BC547).

**Conclusie:** de korte DMA-read is betrouwbaar. NET_RX kan dus zoals ontworpen: de Atari vraagt altijd 3 sectoren, en een lege poll kost 1 sector (~0,75 ms). Een aparte NET_POLL is niet nodig.

## ACSI_NET fase 2: testverslag (4 oktober 2026)

Frame-brug op de Pico (filter op `netif->input`, RX-ring 8, TX-ring 4, TX via een worker in de cyw43-context). Getest met `NETTEST A/L/R/D`, tussendoor steeds 16 KB-reads van `C:\ACSITNFS.PRG` en `F:\GAMES\BIG.BI4` (TNFS), vergeleken met een referentie.

| Test | Resultaat |
|---|---|
| `NETTEST A`: ARP-request vanaf de Atari naar de router | antwoord van 192.168.178.1 na 255 ms; 31 NET_RX-polls, 0 fouten |
| `NETTEST L`: ARP van de laptop voor 192.168.178.50 | 9 requests ontvangen en beantwoord; daarna 14 ICMP-echo's van de laptop ontvangen |
| NET_TX | 18 frames verzonden, 0 BUSY, 0 geweigerd, 0 zendfouten |
| NET_RX | 411 polls, 0 fouten; 0,47–1,1 ms per poll |
| Bestand-reads tijdens A en L | C: 97, F: 96, 0 fouten |
| Brug aan, niemand pollt (kopieertest) | ring vol → 147 frames gecontroleerd gedropt, geen invloed op disk/TNFS |
| `NETTEST D` | brug uit, filter weg, performance-mode uit |

**Gevonden en opgelost tijdens fase 2 (bestaand leespad, niet het netwerk):** de Pico zette na /ACK te snel de volgende byte op de bus; de DMA-chip las dan af en toe die volgende byte. NETTEST zelf laadde zo beschadigd (`LINK A6,#$ffec` → `#$ecec`, bommen). Hold na /ACK van ~50 ns naar ~130 ns (`acsi_bus.pio`). Daarna `NETTEST R`: 5984 KB, 0 fouten; in totaal 1538 reads zonder één /ACK-afwijking; snelheid gelijk (16 KB van C: in 11,4 ms).

**Gevonden, nog open (TNFS-schrijven, niet het netwerk):** bij het kopiëren van GAMES naar `F:\WRITE` duurde de sync van een bestand van 405 KB 9,65 s. Een read van de TNFS-drive die in die tijd binnenkwam, wachtte op core0 en gaf na 9 s een leesfout → het kopiëren brak af (`DATA_002.DEL` e.v. ontbreken). Gekopieerde bestanden zijn wel correct. Oplossing: de sync laat tussendoor wachtende reads voor gaan, of wordt in stukjes gedaan.

## ACSI_NET fase 3: testverslag (4 oktober 2026)

`ACSI_NET.STX` (skelet, 2,9 KB) onder STinG 1.26 met XControl 1.31; STinG-map op de TNFS-drive (`F:\STING`), `STING.PRG`/`STING.INF` in `C:\AUTO`.

| Test | Resultaat |
|---|---|
| Laden | adapter gevonden (sub 8), NET_INFO ok, poort ACSI2TNFS geïnstalleerd |
| Poort actief in STNGPORT.CPX (192.168.178.50/24) | NET_CTRL → Pico: bridge on, filter, performance-mode |
| Pollen vanuit de STinG-thread | duizenden NET_RX-polls, 0 fouten |
| Kopiëren met actieve poort: `C:\AUTO` en `F:\GAMES` naar `F:\WRITE` | alles identiek (16/16 + AUTO); 361 reads, 127 writes, 0 fouten; syncs van 42 s en 28 s ondertussen |
| Crystal Castles starten | drie bommen met STinG + XControl geladen, ook met de poort uit én zonder `ACSI_NET.STX`; zonder STinG/XControl start het. Oorzaak dus STinG/XControl (geheugen of vectoren), niet ACSI_NET |

Opgevallen: STinG roept de receive-routine vaker aan dan verwacht (~80–150×/s in plaats van 20×/s bij THREADING 10). Elke lege poll kost ~0,75 ms. Uitzoeken in fase 4.

## ACSI_NET fase 4: testverslag (4 oktober 2026) - eerste PING

`ACSI_NET.STX` 00.04 (5 KB): ARP-cache, ARP-antwoorden, ARP-vragen voor de next hop (hooguit 1×/s per adres, wachtrij tot het antwoord), IP-datagrammen ↔ Ethernet-frames, maximaal 4 frames in en 4 datagrammen uit per STinG-aanroep.

| Test | Resultaat |
|---|---|
| Laptop pingt de Atari (192.168.178.50) | 10/10 antwoorden, 66–102 ms (eerste 446 ms incl. ARP); ARP-tabel laptop: .50 en .103 beide `2c:cf:67:c8:e6:ba` |
| PING.PRG op de Atari naar de router (192.168.178.1) | 50 verzonden, 50 ontvangen, 0 verloren |
| Pico-teller | 63 frames naar de Atari, 63 verstuurd; 0 gedropt, 0 BUSY, 0 geweigerd, 0 zendfouten |
| Diskverkeer in de tussentijd | 119 reads, 0 fouten |

Let op: `THREADING` in STinGs `DEFAULT.CFG` is in **milliseconden** (STinG deelt door 5). 50 = elke 50 ms pollen (standaard); 10 gaf ~100 polls/s (de "te vaak pollen" uit fase 3). Een lege poll kost ~0,75 ms, bij 50 ms dus ~1,5 % CPU.

Bekende beperking: de Atari kan het eigen IP van de Pico (192.168.178.103) niet pingen; de brug laat zulke frames bewust vallen (zie ontwerp, risico 6). PING.PRG gebruikt standaard 127.0.0.1 (loopback in STinG zelf).

## ACSI_NET fase 5: testverslag (5 oktober 2026) - UDP, TCP, DNS

Client: gapFTP 0.88 (17 KB TTP, commandoregel, STinG). Server: Python `pyftpdlib` op de laptop (poort 2121).

| Test | Resultaat |
|---|---|
| FTP naar de laptop, download naar F: (TNFS) | HELLO.TXT, BIG.BIN (300 KB, 12 s), MEG.BIN (1 MB, 43 s): **alle drie identiek**; ~24 KB/s; ondertussen 954 reads en 2095 writes zonder fout, TNFS-syncs liepen ertussendoor |
| `open ftp.funet.fi` (DNS via de router, internet) | werkt |
| Download BIG.BIN naar C: | 27–30 s (~11 KB/s), identiek |
| Pico-tellers over alle tests | 0 frames gedropt, 0 BUSY, 0 zendfouten |

Wat er misging en waarom (geen netwerkfout):
- Eerste download naar C: liep vast: C: was vol. MEG.BIN (1 MB) paste niet; na de reset bleven ~700 KB aan **verloren clusters** in de FAT staan (bestand nooit gesloten), zodat ook na weggooien 0 bytes vrij bleef en gapFTP bij het eerste stuk data bleef hangen (TCP-venster dicht). Opgelost met console `F` (C: opnieuw) en de backup terugzetten.
- Naar C: is de helft zo snel als naar F:: gapFTP schrijft per 512 bytes, en elke schrijfopdracht naar C: kost een volledige flash-wis van 4 KB (~42 ms). Ook onnodige slijtage.

Verbeterpunten (los van ACSI_NET):
1. Console-commando "C: controleren": verloren clusters vinden en (na bevestiging) vrijgeven, zoals CHKDSK.
2. Schrijven naar C: bufferen: meerdere writes naar hetzelfde 4 KB-blok samen wegschrijven.

## Nog testen: ACSI2SD achter de adapter (doorlusconnector)

De dev-print heeft twee 20-polige connectoren die alle signalen doorverbinden, zoals bij Lotharek's ACSI2SD. Een tweede apparaat hangt dan gewoon parallel op de bus, met een eigen ACSI-ID. Nog niet getest: de ACSI2SD is hier niet aanwezig.

Wat er al voor gedaan is:

- **Bootdrive:** TOS voert de bootsector van elk ACSI-ID uit, van 0 naar 7 (`dmaboot` in `bios/startup.S` van TOS 1.x). Staat de ACSI2SD op een lager ID dan wij, dan heeft zijn driver C: al genomen. Onze driver zet de bootdrive (en de reset-hook) nu alleen nog als onze flash-disk C: krijgt; anders blijft de bootdrive van de andere driver staan. Zonder ACSI2SD getest: de ST start van onze C: zoals voorheen.

Testplan als de ACSI2SD er is:

1. **Pull-up op GP6 eerst** (zie hierboven): anders drijft U1 tijdens reset of BOOTSEL van de Pico de bus, ook voor de ACSI2SD.
2. ACSI2SD op ID 0, wij op 6. Koude start: de ACSI2SD krijgt C: en de ST start daarvan; onze drives krijgen de volgende vrije letters ("[OK] Drives installed: ...").
3. **Dubbel aankoppelen nagaan:**
   - HDDRIVER scant alleen de ID's die in HDDRUTIL aanstaan; ID 6 daar uitzetten.
   - AHDI scant alle ID's en koppelt onze partities dan ook aan, naast onze driver. Dezelfde schijf onder twee letters gaat bij schrijven mis. Oplossing (nog te bouwen): onze driver kijkt via `pun_ptr` of onze partities al aangekoppeld zijn en slaat ze dan over. Tot die tijd: verborgen modus (`H`) of HDDRIVER.
4. Lezen en schrijven op beide apparaten door elkaar, en een Pico-flash terwijl de Atari van de ACSI2SD leest (controle van de pull-up).
5. Kabellengte: de hele keten kort houden; bij haperingen eerst daar kijken.

## Eerst nagaan (voor optie 3: virtuele FAT)

Twee dingen moeten op de echte hardware bevestigd worden voordat hierop gebouwd wordt:

1. **Scant AHDI op de Mega STE ook andere ID's dan 0?** Verwacht: ja, maar niet getest. Test met de sniffer-modus: aanzetten en kijken naar welke ID's AHDI commando's stuurt.
2. **Scant HDDRIVER het ID van de Pico?** HDDRIVER scant alleen de ID's die in de HDDRUTIL-instellingen aanstaan. Het ID van de Pico moet daar dus bij staan.
