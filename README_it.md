# ESP32-S3-CAM_PDX DATV Beacon

*[English](README.md) · Italiano*

Beacon DVB-S2 autonomo per la banda radioamatoriale dei 13 cm (2300–2450 MHz), e tramite un mixer esterno per qualsiasi
altra banda, realizzato con una sola scheda **ESP32-S3 con camera**: il trasmettitore Wi-Fi del chip viene usato come
modulatore I/Q, mentre video, multiplexer MPEG-TS, codifica DVB-S2 (BCH, LDPC, framing) e modulazione QPSK con filtro RRC
sono calcolati sull'ESP stesso.

![Ricezione con Portsdown 4 e Minitiouner, MER 26 dB](docs/img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

## Stato (31/10/2026)

| Funzione | Stato |
|---|---|
| Portante, tono, QPSK RRC 0,35 dal DAC del Wi-Fi | collaudato (spettro, quarzo ~1 ppm, banda 465 kHz a 333 kS) |
| Codificatore DVB-S2 (QPSK, frame normali e corti, tutti i rate, pilot) sull'ESP | collaudato: identico bit per bit al codificatore Python in 42/42 casi |
| Multiplexer TS sull'ESP (PAT/PMT/SDT, PCR, PTS sempre crescenti, CBR) | collaudato: MER 25–26 dB, nessuna interruzione |
| Monoscopio H.265 640x360 + tono AAC, loop di 19,2 s senza stacchi | collaudato |
| Camera dal vivo (H.264 320x240), PiP, monoscopi personalizzati, monoscopio con dati in tempo reale | collaudato: MER 24–25 dB |
| Beacon autonomo, configurazione da web (11 lingue) e da seriale, livello in dB, FEC dal menu | collaudato |
| Uscita come IF verso un mixer esterno, frequenza finale in video e nella SDT | collaudato con OL fittizio; compensazione dello spettro invertito da provare con un mixer vero |
| LED RGB di stato (colore proporzionale alla potenza) | collaudato |

## Documentazione

- [Il progetto](docs/PROGETTO.md): perché l'ESP32-S3, come funziona, tappe dello sviluppo, risultati
  ([PDF completo](docs/ESP32-S3-CAM_PDX_DATV_Beacon.pdf), anche offline)
- [Presentazione in 11 lingue](docs/presentazione/) con l'invito a collaborare (Markdown + PDF)
- [Monoscopio personalizzato](docs/MONOSCOPIO.md): immagini dal telefono, video dal PC, monoscopio di Halloween
- [Prova MER / linearità](docs/TEST_MER_20261030.md) del 30/10/2026
- [Timeline della pubblicazione](docs/PUBBLICAZIONE.md) e [piano di diffusione](docs/DIFFUSIONE.md)

## Hardware

- Scheda ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) con camera OV3660, microSD, porte USB-C OTG e TTL, LED RGB su GPIO48
  (provata: diymore ESP32-S3-CAM, disposizione Freenove).
- Antenna per i 13 cm. **Per l'uso in aria è necessario un filtro passa-banda** sull'uscita: il DAC genera immagini.
- Ricevitore DVB-S2 per la prova (es. Minitiouner / Portsdown 4).

## Struttura

```
firmware/        firmware ESP-IDF 6.1 (main/: modulatore, DVB-S2, multiplexer, camera, beacon, web)
tools/           strumenti PC (Python + ffmpeg): monoscopi, caricamento, controllo seriale, PDF
tools/dvbs2/     codificatore DVB-S2 in Python, estrazione e verifica delle tabelle LDPC dalla norma ETSI
docs/            documentazione, prove, immagini, presentazioni, monoscopio di Halloween
```

## Compilazione e caricamento

Firmware (ESP-IDF 6.1):
```
cd firmware
idf.py set-target esp32s3
idf.py -p COM3 flash
```
Monoscopio standard e sfondo del PiP (PC, Python 3 con numpy e Pillow, ffmpeg con libx265):
```
cd tools
python make_es.py -o out/mono_es.bin
python make_bg.py -o out/bg_std.bin
python -m esptool --port COM3 write-flash 0x810000 out/mono_es.bin 0xC10000 out/bg_std.bin
```
Comandi principali sulla seriale (115200): `INFO`, `CONFIG`, `SET <chiave> <valore>`, `SAVE`, `BEACON`, `ESPLAY`,
`MEDIA`, `LEDTEST`; elenco completo all'inizio di `firmware/main/main.c`. Pagina web: rete `<QRZ>-BEACON`,
http://192.168.4.1.

## Collaborare

Prove in aria, segnalazioni, richieste di miglioramento e traduzioni sono benvenute: aprite una *Issue* o una
*Discussion* nel repository, oppure scrivete a **iz7pdx@gmail.com**.

## Note legali

La scheda trasmette solo tra 2300 e 2450 MHz. **Usarla solo con una licenza di radioamatore valida, sulle frequenze e
con le potenze consentite dalla propria licenza e dal band plan**, con un filtro adeguato sull'uscita (e dopo il mixer,
se si usa un transverter).

## Crediti

- DVB-S2: ETSI EN 302 307-1 V1.4.1. Le tabelle LDPC sono estratte dalla norma e verificate (`tools/dvbs2/etsi_tables.py`).
- Componenti Espressif: ESP-IDF, esp32-camera, esp_h264 (Apache 2.0; esp_h264 contiene OpenH264, BSD).

## Licenza

GPL-3.0 — vedi [LICENSE](LICENSE). © 2026 Luigi D'Arcangelo IZ7PDX — iz7pdx@gmail.com
