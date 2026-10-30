# ESP32-S3-CAM_PDX DATV Beacon

**Kompletny nadajnik DVB-S2 z kamerą na płytce za kilkadziesiąt euro**

*Luigi D'Arcangelo, IZ7PDX — projekt open source, licencja GPL-3.0*

![Odbiór radiolatarni na Portsdown 4 i Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Jedna **płytka ESP32-S3 z kamerą** staje się radiolatarnią krótkofalarskiej telewizji cyfrowej. Nadajnik Wi-Fi układu
pracuje jako **modulator I/Q**; całą resztę — obraz, multiplekser MPEG-TS, kodowanie **DVB-S2** według ETSI EN 302 307-1
i modulację QPSK — oblicza sama płytka. Bez SDR, komputera i Raspberry Pi: zasilanie, antena i filtr.

## Co potrafi

- **DVB-S2 QPSK** z szybkością 333 kS/s, FEC od 1/2 do 9/10: odbierany z **MER 23–27 dB** na Portsdown 4 i Minitiouner.
- **2300–2450 MHz** bezpośrednio; z **zewnętrznym mieszaczem** dowolne inne pasmo: częstotliwość końcowa jest widoczna
  na obrazie i w nazwie usługi.
- **Kamera na żywo** (H.264), kamera jako **obraz w obrazie** na planszy testowej, standardowa plansza H.265 i **własne
  plansze**: zdjęcie z telefonu albo film przygotowany na komputerze (jest też plansza na Halloween!).
- **Samodzielna radiolatarnia**: cykl plansza / kamera / przerwa, automatyczny start po włączeniu, **dioda stanu**, która
  „rozgrzewa się” wraz z mocą.
- **Konfiguracja z telefonu** w 11 językach, gdy nadajnik nie nadaje.
- Firmware, narzędzia, dokumentacja i raporty z testów są **wolne** (GPL-3.0).

## Czego potrzeba

- Płytki ESP32-S3 N16R8 z kamerą (testowana: diymore ESP32-S3-CAM, układ Freenove).
- Anteny na 13 cm i **filtra pasmowego**; na większe odległości wzmacniacza liniowego.
- Odbiornika DVB-S2 (Minitiouner, Portsdown, zgodne odbiorniki satelitarne).

## Współpracujmy!

Projekt dopiero się narodził i będzie rósł razem ze społecznością krótkofalowców. Każdy wkład jest mile widziany:

- **testy na antenie**: odległość, MER, użyty odbiornik i anteny, zdjęcia odbioru;
- **zgłoszenia problemów** i **propozycje ulepszeń**;
- pomysły i konstrukcje: wzmacniacze i filtry, transwertery na inne pasma, inne płytki ESP32-S3, wyższe szybkości
  symbolowe;
- **sprawdzenie tłumaczeń** strony internetowej i dokumentów.

Repozytorium, zgłoszenia i dyskusje: **https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon** — kontakt: **iz7pdx@gmail.com**

*Uwaga: wyłącznie dla krótkofalowców z ważnym pozwoleniem, na częstotliwościach i z mocami dozwolonymi przez pozwolenie,
zawsze z odpowiednim filtrem na wyjściu.*
