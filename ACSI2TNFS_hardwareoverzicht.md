# ACSI2TNFS – hardwarebrief voor Claude

**Bron en reikwijdte:** dit overzicht is uit de meegeleverde KiCad 9-bestanden `acsi2tnfs(1).kicad_sch` en `acsi2tnfs.kicad_pcb` afgeleid. Pin-/nettoewijzingen hieronder zijn gecontroleerd op de **PCB-padnetten**. Dit beschrijft de **oorspronkelijke Raspberry Pi Pico W-variant**; niet de afzonderlijk besproken ESP32-S3/FPGA-universele adapter of de floppy-emulator. Er is geen firmware meegeleverd, dus softwaregedrag en werkelijke ACSI-timing mogen niet uit de hardware worden afgeleid. PCB-nettoewijzingen alleen bewijzen evenmin dat elke verbinding fysiek doorgemeten is.

## 1. Wat de hardware moet doen

Een externe Atari ACSI/DMA-peripheral met een Raspberry Pi Pico W als controller, 8-bit bidirectionele ACSI-databus via een SN74LVC245A, vijf host-naar-Pico-controlsignalen via een tweede SN74LVC245A, twee actief-laag interrupt-/DMA-handshake-uitgangen via NPN open-collectorachtige schakelingen, en een SPI-header voor externe 3,3V-SD-kaartmodule. Pico W heeft eigen Wi-Fi en USB. J2 brengt de ACSI-signalen naar een 20-polige header voor een kabel/adapter naar de 19-polige Atari ACSI D-sub. J4 is een tweede parallel aangesloten 20-polige debugheader.

### Spanningsdomeinen
- ACSI-zijde: 5V TTL-compatibel; Pico W GPIO: 3,3V (niet rechtstreeks 5V-tolerant).
- U1 en U2: SN74LVC245A gevoed uit Pico's 3,3V-uitgang. Hun inputs accepteren volgens TI tot 5,5V; 3,3V-uitgangen sturen TTL-compatibele ACSI-ingangen.
- `+5V` is op dit ontwerp gekoppeld aan Pico W pin 40 (`VBUS`) en voedt de lokale pull-ups/C5. **De ACSI-poort levert geen voedingsspanning.** De 5V komen dus niet van J2.
- Pico W pin 36 (`3V3_OUT`) voedt `+3.3V`, de beide transceivers, SD-header en leds.

## 2. Exacte Pico W GPIO-indeling (PCB A1)

| Pico GPIO | Pico fysieke pin | PCB-net | Functie / signaalpad |
|---|---:|---|---|
| GP0 | 1 | `UART0_TX` | Debug-J1 pin 1, UART0 TX |
| GP1 | 2 | `UART0_RX` | Debug-J1 pin 2, UART0 RX |
| GP2 | 4 | `CLK` | SD-J3 pin 4, SPI0 SCK |
| GP3 | 5 | `MOSI` | SD-J3 pin 3, SPI0 TX/MOSI |
| GP4 | 6 | `MISO` | SD-J3 pin 5, SPI0 RX/MISO |
| GP5 | 7 | `CS` | SD-J3 pin 2, SPI0 chip-select |
| GP6 | 9 | `/OE` | U1 pin 19, active-low enable van bidirectionele databuffer |
| GP7 | 10 | `DIR` | U1 pin 1, richting databuffer |
| GP8 | 11 | `DATA0_3V3` | U1 pin 18, ACSI D0 |
| GP9 | 12 | `DATA1_3V3` | U1 pin 17, ACSI D1 |
| GP10 | 14 | `DATA2_3V3` | U1 pin 16, ACSI D2 |
| GP11 | 15 | `DATA3_3V3` | U1 pin 15, ACSI D3 |
| GP12 | 16 | `DATA4_3V3` | U1 pin 14, ACSI D4 |
| GP13 | 17 | `DATA5_3V3` | U1 pin 13, ACSI D5 |
| GP14 | 19 | `DATA6_3V3` | U1 pin 12, ACSI D6 |
| GP15 | 20 | `DATA7_3V3` | U1 pin 11, ACSI D7 |
| GP16 | 21 | `/CS_3V3` | Host `/CS` via U2 |
| GP17 | 22 | `/RESET_3V3` | Host `/RESET` via U2; R7 100k naar GND |
| GP18 | 24 | `/ACK_3V3` | Host ACK via U2 (netnaam op PCB `/ACK`) |
| GP19 | 25 | `A1_3V3` | Host A1 via U2 |
| GP20 | 26 | `R/W_3V3` | Host R/W via U2 |
| GP21 | 27 | `Net-(A1-GPIO21)` | Via R5 4k7 naar basis Q1 (BC547); hoog = `/IRQ` laag |
| GP22 | 29 | `Net-(A1-GPIO22)` | Via R2 4k7 naar basis Q2 (BC547); hoog = `/DRQ` laag |
| GP28 | 34 | `STATUS_LED` | Via R4 330Ω naar D2 led, vervolgens GND; hoog = aan |

**Overige aansluitingen A1:** fysieke pinnen 3/8/13/18/23/28/33/38 = GND; 36 = `+3.3V` (`3V3_OUT`); 40 = `+5V` (`VBUS`). Fysieke pinnen 30 (RUN), 31 (GP26), 32 (GP27), 35 (ADC_VREF), 37 (3V3_EN), 39 (VSYS) zijn in de PCB als niet aangesloten aangegeven. GP23–GP25 van de Pico W zijn geen vrije gewone header-I/O voor dit ontwerp; bouw de firmware op bovenstaande fysieke aansluitingen.

## 3. ACSI D-sub 19 pinout en J2/J4-koppeling

De PCB gebruikt **twee 2x10 headers J2 en J4 met parallelle netten**. Beiden hebben pinnen 1–19 met onderstaande ACSI-pinnummers, en pin 20 = niet aangesloten. J2 heet `To ACSI`, J4 `Debug Header`. De PCB zelf bevat **geen DB-19 D-sub-footprint**. Controleer de fysieke oriëntatie van de custom 2x10-footprint en de kabel naar een Atari-connector: een 1-op-1 genummerde netlijst is NIET hetzelfde als gegarandeerd correcte flatcable-/IDC-oriëntatie.

| ACSI pin = J2/J4 pin | Signaal | Richting t.o.v. Atari host | Betekenis |
|---:|---|---|---|
| 1 | D0 | bidirectioneel | Data bit 0 |
| 2 | D1 | bidirectioneel | Data bit 1 |
| 3 | D2 | bidirectioneel | Data bit 2 |
| 4 | D3 | bidirectioneel | Data bit 3 |
| 5 | D4 | bidirectioneel | Data bit 4 |
| 6 | D5 | bidirectioneel | Data bit 5 |
| 7 | D6 | bidirectioneel | Data bit 6 |
| 8 | D7 | bidirectioneel | Data bit 7 |
| 9 | `/CS` | Atari → device | Active-low chip select / software handshake |
| 10 | `/IRQ` | device → Atari | Active-low interrupt/status request; uitgang Q1 |
| 11 | GND | — | Massa |
| 12 | `/RESET` | Atari → device | Active-low hardware-reset |
| 13 | GND | — | Massa |
| 14 | `ACK` (PCB-net `/ACK`) | Atari → device | DMA acknowledge, handshakesignaal |
| 15 | GND | — | Massa |
| 16 | A1 | Atari → device | Adres-/selectiesignaal |
| 17 | GND | — | Massa |
| 18 | R/W | Atari → device | Transferrichting |
| 19 | `/DRQ` | device → Atari | Active-low DMA data request; uitgang Q2 |
| 20 | NC (alleen header) | — | Niet aanwezig op de 19-polige ACSI-poort |

**Signaalnaam-opmerking:** De PCB heeft `/ACK` als netnaam waar externe documentatie vaak `ACK` schrijft. Gebruik de daadwerkelijke hardware- en timingdefinitie, niet alleen een slash in een KiCad-label, om logische polariteit te bepalen. De pinbenaming `R{slash}W` in KiCad wordt hier als R/W weergegeven.

## 4. Componenten (waardes en footprint uit de meegeleverde KiCad-bestanden)

| Ref. | Component / waarde | Footprint | Aansluiting en bedoeling |
|---|---|---|---|
| A1 | Raspberry Pi Pico W | `Module:RaspberryPi_Pico_Common_THT` | Hoofdcontroller + Wi-Fi + USB |
| U1 | `SN74LVC245APW` *schemawaarde* | `Package_SO:SOIC-20W_7.5x15.4mm_P1.27mm` | Bidirectionele ACSI D0–D7 ↔ GP8–15; VCC 3,3V, DIR=GP7, /OE=GP6 |
| U2 | `SN74LVC245APW` *schemawaarde* | dezelfde SOIC-20W | Vaste A→B-richting: /CS, /RESET, /ACK, A1, R/W → GP16–20. DIR (pin 1) aan 3,3V; /OE (pin 19) aan GND. De drie ongebruikte kanaalparen zijn aan GND geknoopt. |
| Q1 | BC547 NPN | `Package_TO_SOT_THT:TO-92_Inline` | Collector `/IRQ`, basis via R5 vanaf GP21, emitter GND |
| Q2 | BC547 NPN | zelfde TO-92 | Collector `/DRQ`, basis via R2 vanaf GP22, emitter GND |
| R1 | 3k3 | 0805 | `/IRQ` pull-up naar +5V |
| R2 | 4k7 | 0805 | GP22 → Q2-basis |
| R3 | 330Ω | 0805 | +3,3V → D1 (power-led) |
| R4 | 330Ω | 0805 | GP28 → D2 (status-led) |
| R5 | 4k7 | 0805 | GP21 → Q1-basis |
| R6 | 3k3 | 0805 | `/DRQ` pull-up naar +5V |
| R7 | 100k | 0805 | `/RESET_3V3` pulldown naar GND |
| R8 | 100k | 0805 | U1 `/OE` pulldown naar GND |
| R9 | 100k | 0805 | U1 `DIR` pull-up naar +5V (TI bevestigt 5,5V inputtolerantie; blijft een controlepunt) |
| C1 | 10µF | 0805 | +3,3V naar GND |
| C2, C3, C4 | 100nF elk | 0805 | +3,3V naar GND, ontkoppeling |
| C5 | 10µF | 0805 | +5V naar GND |
| D1 | LED | `LED_0805_2012Metric` | 3,3V power-led, via R3 aan +3,3V, kathode GND |
| D2 | LED | `LED_0805_2012Metric` | Status-led via R4/GP28, kathode GND |
| J1 | Debug 1x3 | 2,54mm THT pinheader | Pin 1 TX, pin 2 RX, pin 3 GND |
| J2 | To ACSI 2x10 | custom `Library:PinHeader_2x10_P2.54mm_Vertical_NR_PER_ROW` | ACSI pin 1–19 + NC20 |
| J3 | To SD card 1x6 | 2,54mm THT pinheader | 1=3,3V; 2=CS; 3=MOSI; 4=CLK; 5=MISO; 6=GND |
| J4 | Debug Header 2x10 | zelfde custom footprint als J2 | 100% dezelfde ACSI-signalen als J2 |

**LET OP: U1/U2 partnummer/footprint-inconsistentie:** de schemawaarde eindigt op `PW` (TSSOP-20-variant), maar de werkelijk getekende PCB-footprint is een grote SOIC-20W met 1,27mm pitch. Kies voor assemblage een elektrisch equivalente, bij die SOIC-footprint passende exacte bestelvariant en wijzig de BOM/schemawaarde. Plaats geen PW/TSSOP op deze SOIC-pads. Het is kennelijk een latere aanpassing voor gemakkelijker handsolderen.

### Gedrag van de twee transceivers
- U1 A-kant (pinnen 2–9) = ACSI `DATA0..7` en B-kant (pinnen 18–11) = Pico `DATA0..7_3V3`.
- TI-functietabel: `OE=1` = beide zijden high-Z; bij `OE=0, DIR=1` gaat A→B (Atari→Pico); bij `OE=0, DIR=0` gaat B→A (Pico→Atari).
- U1 `/OE` heeft **R8 pulldown**: standaard enabled, niet automatisch high-Z tijdens de Pico-boot. U1 `DIR` heeft R9 pull-up: standaard A→B. Voor veilig loskoppelen tijdens reset/boot of multi-devicebus, dit expliciet meenemen in de firmware en eventueel hardware aanpassen. TI adviseert /OE normaal pull-up voor high-Z bij power-up/down.
- U2 is permanent enabled A→B: /CS, /RESET, /ACK, A1 en R/W worden als 3,3V-inputsignalen aangeboden aan de Pico. U2 wordt nooit naar B→A geschakeld.
- Q1/Q2 zijn low-side-schakelaars: GP21/22 hoog zet de betreffende `/IRQ`/`/DRQ`-lijn laag, GPIO laag laat de externe pull-up hem hoog maken. Houd deze GPIO's tijdens boot standaard laag/input totdat firmware de bus werkelijk mag bedienen. De pinnen `/IRQ` en `/DRQ` zijn geen 3,3V push-pull-Pico-uitgangen.

## 5. Wat Claude over Atari ACSI moet weten

- **ACSI = Atari Computer System Interface**, de externe 19-polige D-sub DMA-randapparatuurpoort van de Atari ST-familie (ST/STE/Mega/Mega STE/TT met ACSI). De Falcon heeft géén gelijkwaardige externe ACSI-poort; verwar die niet met Falcon SCSI/IDE.
- ACSI is een **8-bit, TTL-signaalbus**, SCSI-/SASI-achtig op commandoniveau, maar geen gewone 50-pins SCSI-bus. De **Atari is de initiator/host** die de transactie bestuurt; een ACSI-doelapparaat reageert. Geen SCSI Message Phase of normale SCSI bus arbitration.
- ACSI heeft twee handshakevarianten: CPU-gestuurde command-/statusbytecommunicatie via host `/CS` en device `/IRQ`, en DMA-dataoverdracht met device `/DRQ` en host `ACK`. Richting en adresselectie via `R/W` en `A1`. Gebruik de officiële timingdiagrammen voor precieze flanken; niet alleen aannames op basis van signaalnamen.
- Een standaard ACSI-commandoblok is historisch **6 bytes**; de hoogste 3 bits van byte 0 bevatten de ACSI controller-ID (0–7). Device-/logical-unit-adressering zit in de overige commandovelden; precieze opcodes/uitbreidingen zijn driverafhankelijk. Dataoverdracht is vaak in blokken van 512 bytes.
- Er zijn maximaal 8 logische controller-ID's; volgens Atari's Integration Guide is maximaal 4 fysieke controllers daisy-chainen aan te houden voor betrouwbaarheid. De ACSI-poort heeft **geen +5V-voedingspin**.
- **ACSI elektrisch niet als SCSI termineren** enkel omdat de namen op elkaar lijken: original ACSI gebruikt TTL-/CMOS-businterfacing, geen standaard passieve SCSI 220/330Ω terminatienetwerken. Houd de kabel kort en controleer grounding.
- Timing is krap. Wi-Fi/TNFS/SD mag real-time DRQ↔ACK-handshake niet blokkeren. Denk aan PIO/DMA/interrupt-architectuur, expliciete direction/OE en een buffer tussen trage backend en timingkritische Atari-interface. Houd multi-target / gedeelde bus in gedachten.
- P. Putnik publiceerde praktijkmetingen: ongeveer 2MB/s piek tijdens sector-DMA mogelijk op geteste Atari's, tegenover de vaak geciteerde 1–1,25MB/s in documentatie; dit is een **ervarings-/meetresultaat**, geen universele garantie. Hij waarschuwt ook voor timingproblemen bij de laatste bytes van een sector bij zijn CF-adapter. Baseer firmware op de officiële timing én metingen aan het eigen board.

## 6. Uitdrukkelijke reviewpunten voor Claude

1. Gebruik de meegeleverde KiCad-schema- en PCB-netten als single source of truth voor dit concrete board; wijzig ze niet stilzwijgend naar een andere GPIO-map uit een ouder Pico-project of naar de ESP32/FPGA-boardarchitectuur.
2. Controleer de echte ACSI D-sub-connectororiëntatie/kabel naar J2, en de custom 'NR_PER_ROW' nummering; pin20 is NC.
3. Verifieer U1/U2 **SOIC-footprint versus `...PW` TSSOP-componentnaam** en selecteer passend bestelnummer.
4. Beoordeel power-up failsafe: U1 `/OE` heeft default LOW, dus geen automatische high-Z; DIR default HIGH. U2 is altijd enabled. Denk na over gelijktijdig actieve ACSI-devices en ongewenst bus-driving.
5. Verifieer polariteit/actieve flank van ACK: KiCad gebruikt `/ACK`, referenties soms `ACK`; volg Atari timinggids en meet.
6. Check BC547 C/B/E in de specifieke gekozen fabrikant/TO-92-montage; gebruik geen ongecontroleerd 'standaard SOT-23'-pinout.
7. ACSI biedt géén voeding: USB/Pico VBUS moet +5V voor pull-ups leveren. Valideer elektrisch gedrag bij Atari aan, Pico uit en omgekeerd.
8. Firmware mag `/IRQ` en `/DRQ` slechts via GP21/GP22 en Q1/Q2 aansturen; maak ze bij init inactief.
9. Bevestig dat SD-module **3,3V** werkt en niet een module met vereist 5V-ingang gebruikt. J3 bevat geen extra SD-levelshifter/regulator.
10. Firmware, timing en ondersteunde commandoset zijn **niet** onderdeel van deze meegeleverde bestanden; speculeer er niet over alsof ze al geïmplementeerd zijn.

## 7. Mee te geven documentatie (directe URLs)

Verplichte gebruikerslinks:

1. Praktijkervaring en correcties, P. Putnik: https://atari.8bitchip.info/AcsiDmaExD.html
2. Atari ACSI/DMA pinout: https://allpinouts.org/pinouts/connectors/parallel/atari-acsi-dma/
3. AtariForumWiki, ACSI en Atari hardware specification: https://temlib.org/AtariForumWiki/index.php/ACSI

Aanvullend teruggevonden originele documenten:

4. **ATARI ACSI/DMA Integration Guide, 28 juni 1991 – originele handleiding, 48 pagina's, timingdiagrammen, software/DMA-handshake en connectoren:** https://archive.org/download/ACSI_DMA_Guide_6-28-1991/ACSI_DMA_Guide_6-28-1991.pdf
5. Internet Archive-item: https://archive.org/details/ACSI_DMA_Guide_6-28-1991
6. **Application Notes on the Atari Computer System Interface (ACSI), 27 september 1985:** https://www.bitsavers.org/pdf/atari/ST/Atari_ST_GEM_Programming_1986/GEM_0942.pdf
7. **Atari ST ProfiBuch** (de uitgave uit 1987 wordt specifiek aangehaald op Putniks pagina; een latere herziene ST/STE/TT-PDF is online): https://julian-reschke.de/ATARI%20Profibuch%20ST-STE-TT.pdf
8. TI SN74LVC245A-datasheet (function table, 5,5V input, power-up high-Z-advies): https://www.ti.com/lit/ds/symlink/sn74lvc245a.pdf

**Verzoek aan Claude:** lees eerst de KiCad-bestanden en de officiële 1991 ACSI/DMA Integration Guide. Geef een ontwerp-/firmwarereview die elektrische veiligheid, hardwarehandshake, direction/high-Z, multi-devicebus en real-time verwerking expliciet afdekt. Scheid geverifieerde boardfeiten van aannames of verbetervoorstellen.
