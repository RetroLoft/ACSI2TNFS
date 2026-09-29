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

## Eerst nagaan (voor optie 3: virtuele FAT)

Twee dingen moeten op de echte hardware bevestigd worden voordat hierop gebouwd wordt:

1. **Scant AHDI op de Mega STE ook andere ID's dan 0?** Verwacht: ja, maar niet getest. Test met de sniffer-modus: aanzetten en kijken naar welke ID's AHDI commando's stuurt.
2. **Scant HDDRIVER het ID van de Pico?** HDDRIVER scant alleen de ID's die in de HDDRUTIL-instellingen aanstaan. Het ID van de Pico moet daar dus bij staan.
