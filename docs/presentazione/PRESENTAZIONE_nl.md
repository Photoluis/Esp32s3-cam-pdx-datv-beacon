# ESP32-S3-CAM_PDX DATV Beacon

**Een complete DVB-S2-zender met camera op een print van enkele tientallen euro's**

*Luigi D'Arcangelo, IZ7PDX — open-sourceproject, licentie GPL-3.0*

![Ontvangst van het baken met Portsdown 4 en Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Eén enkele **ESP32-S3-print met camera** wordt een baken voor digitale amateurtelevisie. De wifi-zender van de chip wordt
gebruikt als **I/Q-modulator**; al het andere — video, MPEG-TS-multiplexer, **DVB-S2**-codering volgens ETSI EN 302 307-1
en QPSK-modulatie — berekent de print zelf. Geen SDR, pc of Raspberry Pi: voeding, antenne en filter.

## Wat het doet

- **DVB-S2 QPSK** op 333 kS/s, FEC 1/2 tot 9/10: ontvangen met **MER 23–27 dB** op Portsdown 4 en Minitiouner.
- **2300–2450 MHz** rechtstreeks; elke andere band met een **externe mixer**: de eindfrequentie verschijnt in beeld en in
  de servicenaam.
- **Live camera** (H.264), camera als **beeld-in-beeld** in het testbeeld, standaard testbeeld in H.265 en **eigen
  testbeelden**: een foto van de telefoon of een op de pc voorbereide video (er is er ook een voor Halloween!).
- **Zelfstandig baken**: cyclus testbeeld / camera / pauze, automatisch starten bij inschakelen, een **status-led** die
  met het vermogen "warmer" wordt.
- **Instellen via de telefoon** in 11 talen, wanneer er niet wordt uitgezonden.
- Firmware, tools, documentatie en meetrapporten zijn **vrij** (GPL-3.0).

## Wat je nodig hebt

- Een ESP32-S3 N16R8-print met camera (getest: diymore ESP32-S3-CAM, Freenove-indeling).
- Een 13 cm-antenne en een **banddoorlaatfilter**; een lineaire versterker voor grotere afstanden.
- Een DVB-S2-ontvanger (Minitiouner, Portsdown, compatibele satellietontvangers).

## Doe mee!

Het project is net geboren en groeit met de radioamateurgemeenschap. Elke bijdrage is welkom:

- **tests in de lucht**: afstand, MER, gebruikte ontvanger en antennes, foto's van de ontvangst;
- **foutmeldingen** en **verzoeken om verbeteringen**;
- ideeën en bouwprojecten: versterkers en filters, transverters voor andere banden, andere ESP32-S3-printen, hogere
  symboolsnelheden;
- **nalezen van de vertalingen** van de webpagina en de documenten.

Repository, meldingen en discussies: **https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon** — contact: **iz7pdx@gmail.com**

*Let op: alleen voor gelicentieerde radioamateurs, op de frequenties en met de vermogens die uw machtiging toestaat,
altijd met een geschikt filter op de uitgang.*
