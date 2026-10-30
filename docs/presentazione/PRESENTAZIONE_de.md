# ESP32-S3-CAM_PDX DATV Beacon

**Ein vollständiger DVB-S2-Sender mit Kamera auf einer Platine für wenige zehn Euro**

*Luigi D'Arcangelo, IZ7PDX — Open-Source-Projekt, Lizenz GPL-3.0*

![Empfang der Bake mit Portsdown 4 und Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Eine einzige **ESP32-S3-Platine mit Kamera** wird zur Bake für digitales Amateurfunkfernsehen. Der WLAN-Sender des Chips
dient als **I/Q-Modulator**; alles andere — Video, MPEG-TS-Multiplexer, **DVB-S2**-Codierung nach ETSI EN 302 307-1 und
QPSK-Modulation — berechnet die Platine selbst. Kein SDR, kein PC, kein Raspberry Pi: Stromversorgung, Antenne und Filter.

## Was sie kann

- **DVB-S2 QPSK** mit 333 kS/s, FEC 1/2 bis 9/10: empfangen mit **MER 23–27 dB** an Portsdown 4 und Minitiouner.
- **2300–2450 MHz** direkt; mit einem **externen Mischer** jedes andere Band: die Endfrequenz erscheint im Bild und im
  Dienstnamen.
- **Live-Kamera** (H.264), Kamera als **Bild-im-Bild** im Testbild, Standard-Testbild in H.265 und **eigene Testbilder**:
  ein Foto vom Telefon oder ein am PC vorbereitetes Video (auch eines zu Halloween!).
- **Autonome Bake**: Zyklus Testbild / Kamera / Pause, automatischer Start beim Einschalten, eine **Status-LED**, die mit
  der Leistung „wärmer“ wird.
- **Einstellung per Telefon** in 11 Sprachen, während nicht gesendet wird.
- Firmware, Werkzeuge, Dokumentation und Messberichte sind **frei** (GPL-3.0).

## Was man braucht

- Eine ESP32-S3-N16R8-Platine mit Kamera (getestet: diymore ESP32-S3-CAM, Freenove-Belegung).
- Eine 13-cm-Antenne und ein **Bandpassfilter**; für größere Entfernungen einen linearen Verstärker.
- Einen DVB-S2-Empfänger (Minitiouner, Portsdown, kompatible Satellitenempfänger).

## Machen Sie mit!

Das Projekt ist gerade entstanden und wächst mit der Amateurfunk-Gemeinschaft. Jeder Beitrag ist willkommen:

- **Tests auf Sendung**: Entfernung, MER, verwendeter Empfänger und Antennen, Fotos vom Empfang;
- **Fehlermeldungen** und **Verbesserungswünsche**;
- Ideen und Aufbauten: Verstärker und Filter, Transverter für andere Bänder, andere ESP32-S3-Platinen, höhere Symbolraten;
- **Durchsicht der Übersetzungen** der Webseite und der Dokumente.

Repository, Meldungen und Diskussionen: **https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon** — Kontakt: **iz7pdx@gmail.com**

*Hinweis: nur für lizenzierte Funkamateure, auf den Frequenzen und mit den Leistungen, die Ihre Lizenz erlaubt, immer mit
einem geeigneten Filter am Ausgang.*
