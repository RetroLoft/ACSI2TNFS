ACSI2TNFS - FLASH HARDDISK
==========================

Gefeliciteerd: je leest dit bestand vanaf een Raspberry Pi Pico
die zich via de ACSI-poort gedraagt als harddisk.

Wat staat er op deze schijf?
  README.TXT   dit bestand
  CONFIG.TOS   configuratieprogramma voor de adapter

Hoe werkt het?
  - De Pico beantwoordt ACSI-commando's op device id 0.
  - De schijf (ca. 1,5 MB) staat in het flashgeheugen van de Pico.
  - Sector 0 bevat een bootloader. TOS voert die uit tijdens het
    opstarten; die laadt een kleine driver die de GEM-partitie
    als station C: aanmeldt.
  - Schrijven kan ook: de Pico wist en programmeert telkens een
    blok van 4 KB flash.

Station C: zichtbaar maken op de desktop
  Selecteer het icoon van floppy A:, kies Options / Install Disk
  Drive, verander de letter in C en klik op Install.
  Daarna eventueel Options / Save Desktop.

CONFIG.TOS
  Toont firmwareversie, uptime en statistieken van de adapter,
  laat de led op de Pico knipperen en kan het ACSI id wijzigen
  (actief na een reset van de Atari).

USB-console van de Pico (115200, maakt niet uit bij USB):
  s = sniffer (passief)   t = target (harddisk)   0-7 = ACSI id
  v = verbose log         i = info                B = BOOTSEL

Volgende stap: TNFS over het netwerk (met een Pico W).
