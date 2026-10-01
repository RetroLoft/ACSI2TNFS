ACSI2TNFS - FLASH HARDDISK
==========================

Gefeliciteerd: je leest dit bestand vanaf een Raspberry Pi Pico
die zich via de ACSI-poort gedraagt als harddisk.

Wat staat er op deze schijf?
  README.TXT   dit bestand
  ACSITNFS.PRG configuratieprogramma (GEM): Wi-Fi, TNFS-drives, klok
  CONFIG.TOS   eenvoudig configuratieprogramma (tekstscherm)

Hoe werkt het?
  - De Pico beantwoordt ACSI-commando's op zijn eigen device id
    (een nieuwe adapter: id 6, zie "ACSI id wijzigen").
  - De schijf (ca. 1 MB) staat in het flashgeheugen van de Pico.
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

ACSI id wijzigen
  Geef elk apparaat op de ACSI-bus een eigen id. Hangen twee
  apparaten op hetzelfde id, dan antwoorden ze tegelijk en werkt
  geen van beide; ook CONFIG.TOS bereikt de adapter dan niet.
  - CONFIG.TOS: menu-optie 2 (actief na een reset van de Atari).
  - USB-console van de Pico: toets 0 t/m 7. Werkt altijd, ook
    bij een conflict: sluit de Pico met USB aan op een pc en
    open de seriele poort met een terminalprogramma.

USB-console van de Pico (115200, maakt niet uit bij USB):
  s = sniffer (passief)   t = target (harddisk)   0-7 = ACSI id
  v = verbose log         i = info                B = BOOTSEL

Met een Pico W / Pico 2 W: Wi-Fi, TNFS-drives en de klok stel je
in met ACSITNFS.PRG.
