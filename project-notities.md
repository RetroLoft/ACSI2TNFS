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
| R8 | /OE van U1, de databus (GP15) | GND | U1 aan tijdens het opstarten van de Pico |
| R9 | DIR van U1 (GP14) | +5V | richting Atari → Pico tijdens het opstarten |

Ze doen ertoe op het moment dat de Pico **niet** onze firmware draait: bij het aanzetten, bij een reset en in BOOTSEL (flashen). Dan zijn GP14/GP15 ingangen.

**Probleem zonder R8/R9:** de RP2040 en RP2350 zetten na een reset een **interne pull-down** (~50 kΩ) op hun GPIO's. Dan is /OE laag (U1 aan) en DIR laag (richting **Pico → Atari**). U1 drijft dan de ACSI-datalijnen van de Atari laag zolang de Pico in reset of BOOTSEL staat. Valt er op dat moment een ACSI-overdracht (de DMA-chip stuurt een commando, een andere schijf antwoordt), dan vechten twee uitgangen tegen elkaar. Dat het bij het vele flashen met de Atari aan goed ging, komt doordat de Atari toen zelden de bus gebruikte.

**R9 naar +5 V is geen goede oplossing:**

1. **Te zwak:** 100 kΩ tegen de interne pull-down van ~50 kΩ geeft ~1,7–2,2 V op DIR, net geen geldige "hoog" voor de LVC245.
2. **Verkeerde spanning:** DIR hangt ook aan GP14. 5 V via een weerstand op een GPIO is op de RP2040 buiten de specificatie (stroom met 100 kΩ heel klein, maar niet netjes), en staat er ook als de Atari aan is en de Pico niet.

**Advies voor de print:**

- **R8 als pull-up van 10 kΩ naar +3,3 V** (in plaats van een pull-down naar GND). Zolang de Pico niet draait staat U1 dan **uit**: de bus van de Atari is niet aangesloten, in welke stand dan ook. De firmware zet U1 bij het opstarten zelf aan (/OE laag, DIR hoog), daar verandert niets aan.
- **R9 vervalt** zodra /OE gegarandeerd uit staat: de richting doet er dan niet toe. Houd je hem toch, dan 10 kΩ naar **+3,3 V**.
- **R7 mag weg:** U2 staat altijd aan en drijft /RESET op GP17 zodra de 3,3 V er is.

**Op de huidige print:** een 10 kΩ van GP15 (/OE) naar 3,3 V is de belangrijkste. R7 en R9 leeg laten.

## Eerst nagaan (voor optie 3: virtuele FAT)

Twee dingen moeten op de echte hardware bevestigd worden voordat hierop gebouwd wordt:

1. **Scant AHDI op de Mega STE ook andere ID's dan 0?** Verwacht: ja, maar niet getest. Test met de sniffer-modus: aanzetten en kijken naar welke ID's AHDI commando's stuurt.
2. **Scant HDDRIVER het ID van de Pico?** HDDRIVER scant alleen de ID's die in de HDDRUTIL-instellingen aanstaan. Het ID van de Pico moet daar dus bij staan.
