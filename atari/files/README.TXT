ACSI2TNFS - FLASH HARDDISK
==========================

Gefeliciteerd: je leest dit bestand vanaf een Raspberry Pi Pico
die zich via de ACSI-poort gedraagt als harddisk.

Wat staat er op deze schijf?
  README.TXT   dit bestand
  ACSITNFS.PRG configuratieprogramma (GEM): Wi-Fi, TNFS-drives, klok

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

ACSI id wijzigen
  Geef elk apparaat op de ACSI-bus een eigen id. Hangen twee
  apparaten op hetzelfde id, dan antwoorden ze tegelijk en werkt
  geen van beide; ook ACSITNFS.PRG bereikt de adapter dan niet.
  - ACSITNFS.PRG: Config > More > ACSI device ID (na Save en
    een herstart).
  - USB-console van de Pico: toets 0 t/m 7. Werkt altijd, ook
    bij een conflict: sluit de Pico met USB aan op een pc en
    open de seriele poort met een terminalprogramma.

README.TXT en ACSITNFS.PRG zijn alleen-lezen. Per ongeluk weg?
  Toets R in de USB-console zet ze terug; je eigen bestanden op C:
  blijven staan. Reset daarna de Atari.

USB-console van de Pico (115200, maakt niet uit bij USB):
  0-7 = ACSI id           H = verbergen/tonen     K = klok aan/uit
  R = systeembestanden op C: herstellen           F = C: helemaal opnieuw
  i = info                n = netwerkstatus       B = BOOTSEL

Met een Pico W / Pico 2 W: Wi-Fi, TNFS-drives en de klok stel je
in met ACSITNFS.PRG.
