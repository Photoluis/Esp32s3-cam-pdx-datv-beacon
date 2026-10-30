# Beacon DATV 13 cm su ESP32-S3 — IZ7PDX

Beacon DVB-S2 autonomo per la banda radioamatoriale dei 13 cm (2300–2450 MHz) realizzato con una sola scheda
ESP32-S3: il trasmettitore Wi-Fi del chip viene usato come modulatore I/Q, mentre il TS MPEG, la codifica DVB-S2
(BCH, LDPC, framing) e la modulazione QPSK con filtro RRC sono calcolati sull'ESP stesso.

> **English summary.** Stand-alone DVB-S2 amateur TV beacon for the 13 cm band on a single ESP32-S3 board. The chip's
> Wi-Fi transmitter is driven as an I/Q modulator; MPEG-TS multiplexing, DVB-S2 coding (BCH, LDPC, PL framing) and
> RRC-shaped QPSK are all computed on the ESP32-S3. Received with MER 25–27 dB on a Minitiouner (Portsdown 4) at
> 333 kS/s, QPSK 3/4. Work in progress; documentation in Italian for now.

## Stato (30/10/2026, firmware RF0008)

| Funzione | Stato |
|---|---|
| Portante, tono, QPSK RRC 0,35 dal DAC del Wi-Fi | collaudato (spettro, quarzo ~1 ppm) |
| Codificatore DVB-S2 (QPSK, frame normali e corti, tutti i rate, pilot) sull'ESP | collaudato: identico bit per bit al codificatore Python in 42/42 casi |
| Multiplexer TS sull'ESP (PAT/PMT/SDT, PCR, PTS sempre crescenti, CBR) | collaudato: Minitiouner, MER 25–26 dB, nessuna interruzione |
| Monoscopio H.265 640x360 25 fps + tono AAC, loop di 19,2 s senza interruzioni | collaudato |
| Beacon autonomo (configurazione in NVS, partenza automatica, ciclo) | collaudato |
| Configurazione da web (access point a TX ferma) e da seriale | collaudato |
| Camera dal vivo (H.264 320x240), PiP sul monoscopio, monoscopio personalizzato (immagine o video) | collaudato: MER 24–25 dB, 0 fotogrammi persi |
| Pagina web in 11 lingue (IT, EN, DE, ES, FR, PT, NL, PL, EL, JA, AR), livello in dB, FEC dal menu | collaudato (11 lingue: verificate sul PC) |

## Documentazione

- [Il progetto](docs/PROGETTO.md): perché l'ESP32-S3, come funziona, tappe dello sviluppo, risultati
  ([PDF completo](docs/Beacon_DATV_13cm_ESP32-S3_IZ7PDX.pdf), anche offline)
- [Monoscopio personalizzato](docs/MONOSCOPIO.md): immagini dal telefono, video dal PC
- [Prova MER / linearità](docs/TEST_MER_20261030.md) del 30/10/2026
- [Timeline della pubblicazione](docs/PUBBLICAZIONE.md)

## Hardware

- Scheda ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) con camera OV3660, microSD, porte USB-C OTG e TTL
  (provata: diymore ESP32-S3-CAM, disposizione Freenove).
- Antenna per i 13 cm. **Per l'uso in aria è necessario un filtro passa-banda** sull'uscita: il DAC genera immagini.
- Ricevitore DVB-S2 per la prova (es. Minitiouner / Portsdown 4).

## Struttura

```
firmware/        firmware ESP-IDF 6.1 (main/: modulatore, DVB-S2, multiplexer, beacon, web)
tools/           strumenti PC (Python): codificatore DVB-S2 di riferimento, monoscopio, controllo seriale
tools/dvbs2/     codificatore DVB-S2 in Python, estrazione e verifica delle tabelle LDPC dalla norma ETSI
hwtest/          prova iniziale della scheda (PlatformIO / Arduino)
```

## Compilazione e caricamento

Firmware (ESP-IDF 6.1):
```
cd firmware
idf.py set-target esp32s3
idf.py -p COM3 flash
```
Monoscopio (PC, Python 3 con numpy, ffmpeg con libx265):
```
cd tools
python make_es.py --callsign IZ7PDX/B -o out/mono_es.bin
python -m esptool --port COM3 write-flash 0x810000 out/mono_es.bin
```
Comandi principali sulla seriale (115200): `INFO`, `CONFIG`, `SET <chiave> <valore>`, `SAVE`, `BEACON`, `ESPLAY`,
`S2TEST`; elenco completo all'inizio di `firmware/main/main.c`. Pagina web: rete `<QRZ>-BEACON`, http://192.168.4.1.

## Note legali

Il firmware trasmette solo tra 2300 e 2450 MHz. **Usarlo solo con una licenza di radioamatore valida, sulle frequenze
e con le potenze consentite dalla propria licenza e dal band plan**, con un filtro adeguato sull'uscita.

## Crediti

- L'idea di usare il trasmettitore Wi-Fi come modulatore I/Q viene da
  [ESP32-DATV-SA](https://github.com/barisdinc/ESP32-DATV-SA) (SP8ESA, per ESP32-C3). Questo progetto è scritto da zero
  per l'ESP32-S3 e non contiene codice di ESP32-DATV-SA (usato solo come confronto durante lo sviluppo).
- DVB-S2: ETSI EN 302 307-1 V1.4.1. Le tabelle LDPC sono estratte dalla norma e verificate (`tools/dvbs2/etsi_tables.py`).
- Componenti Espressif: ESP-IDF, esp32-camera, esp_h264.

## Licenza

GPL-3.0 — vedi [LICENSE](LICENSE).
