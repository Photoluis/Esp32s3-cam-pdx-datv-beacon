# ESP32-S3-CAM_PDX DATV Beacon v1.0.0

*Release notes — English first, italiano più sotto.*

## English

First public release of **ESP32-S3-CAM_PDX DATV Beacon**: a complete **DVB-S2** amateur television transmitter with
camera on a single low-cost ESP32-S3 board. The chip's Wi-Fi transmitter is the I/Q modulator; video, MPEG-TS
multiplexing and DVB-S2 coding all run on the board.

**Highlights**
- DVB-S2 QPSK, 333 kS/s, FEC 1/2–9/10, 2300–2450 MHz; received with **MER 23–27 dB** on Portsdown 4 and Minitiouner.
- Live camera (H.264), picture-in-picture on the test card, standard H.265 test card, live-data test card, custom test
  cards from your phone or PC — and a **Halloween test card with original spooky music**.
- IF output for an external mixer (transverter): the final frequency appears in the picture and in the service name.
- Stand-alone beacon cycle, web set-up from the phone in **11 languages**, RGB status LED (normal / minimum / off),
  optional telemetry overlay (chip temperature, uptime, cycle).
- 20-minute continuous on-air test: 0 symbol-buffer underruns, 5513 camera frames, none lost.

**Assets**

| File | What it is | Flash at |
|---|---|---|
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_full.bin` | complete image: bootloader, partitions, firmware, standard test card and PiP background (erases the board) | `0x0` |
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_app_only.bin` | firmware update only (keeps test cards and custom files) | `0x0` |
| `halloween_es.bin` | Halloween test card with music, to upload as custom video from the web page | — |
| `ESP32-S3-CAM_PDX_DATV_Beacon.pdf` | complete project document (Italian) | — |
| presentations | `docs/presentazione/` in 11 languages | — |

**Flashing without building** (Python + esptool, board on the TTL serial port):
```
python -m pip install esptool
python -m esptool --chip esp32s3 --port COM3 -b 921600 write-flash 0x0 ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_full.bin
```
Then connect to the Wi-Fi network `NOCALL-BEACON` (password `beacon13cm`), open http://192.168.4.1 and set **your own
callsign** (the page warns until you do), frequency and cycle, then *Apply and save*. Change the Wi-Fi password from the
menu. The standard test card shows the project name; your callsign is sent in the DVB service name and in the live
modes overlay.

**Known limitations:** about 1 in 100 H.264 frames cannot be decoded in PiP / still-image modes (software encoder; the TS
is intact); only 333 kS tested; output power in dBm not yet calibrated.

**Licensed radio amateurs only**, on the frequencies and at the power levels allowed by your licence, always with a
suitable output filter. Feedback, on-air reports and improvement requests are welcome in Issues / Discussions or at
iz7pdx@gmail.com. 73 de IZ7PDX

## Italiano

Prima versione pubblica di **ESP32-S3-CAM_PDX DATV Beacon**: un trasmettitore di televisione amatoriale **DVB-S2**
completo, con camera, su una sola scheda ESP32-S3 economica. Il trasmettitore Wi-Fi del chip fa da modulatore I/Q;
video, multiplexer MPEG-TS e codifica DVB-S2 sono calcolati sulla scheda.

**In evidenza**
- DVB-S2 QPSK, 333 kS/s, FEC 1/2–9/10, 2300–2450 MHz; ricevuto con **MER 23–27 dB** su Portsdown 4 e Minitiouner.
- Camera dal vivo (H.264), PiP sul monoscopio, monoscopio standard H.265, monoscopio con dati in tempo reale, monoscopi
  personalizzati dal telefono o dal PC — e un **monoscopio di Halloween con musica spettrale originale**.
- Uscita come IF verso un mixer esterno (transverter): la frequenza finale compare in video e nel nome del servizio.
- Ciclo autonomo, configurazione dal telefono in **11 lingue**, LED RGB di stato (normale / minimo / spento),
  telemetria facoltativa in sovrimpressione (temperatura del chip, accensione, ciclo).
- Prova continua di 20 minuti in aria: buffer dei simboli mai vuoto, 5513 fotogrammi della camera, nessuno perso.

**File allegati:** immagine completa da scrivere a `0x0` (cancella la scheda: firmware, monoscopio standard e sfondo del
PiP), solo firmware per gli aggiornamenti (conserva monoscopi e file personalizzati), monoscopio di Halloween da caricare
dalla pagina web, PDF completo del progetto; presentazioni in 11 lingue in `docs/presentazione/`.

**Programmazione senza compilare:**
```
python -m pip install esptool
python -m esptool --chip esp32s3 --port COM3 -b 921600 write-flash 0x0 ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_full.bin
```
Poi collegarsi alla rete `NOCALL-BEACON` (password `beacon13cm`), aprire http://192.168.4.1 e impostare **il proprio
nominativo** (la pagina lo ricorda finché non è impostato), la frequenza e il ciclo, poi *Applica e salva*; cambiare la
password del Wi-Fi dal menu. Il monoscopio standard mostra il nome del progetto; il nominativo va in onda nel nome del
servizio DVB e nella sovrimpressione dei modi dal vivo.

**Limiti noti:** circa 1 fotogramma H.264 su 100 non decodificabile in PiP e immagine fissa (encoder software; il TS è
integro); collaudato solo a 333 kS; potenza in dBm non ancora tarata.

**Solo per radioamatori con licenza**, sulle frequenze e con le potenze consentite, sempre con un filtro sull'uscita.
Prove in aria, segnalazioni e richieste di miglioramento sono benvenute nelle Issues / Discussions o a
iz7pdx@gmail.com. 73 de IZ7PDX

## SHA-256

```
7ba197d1583a6f815483252606f8e5828b803e332e203e3bc4e78b231d9235a0  ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_full.bin
ce0dd445381a1c5ebbf3486bab6d34358d273b3152fce72d44d1278b7fb5233d  ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_app_only.bin
b5010765cd502150a2bc11f02ce03acf512ce7226b0c4e99256b747a9f5264e2  halloween_es.bin
781f4fa4ee186844b190fde2d44ed0764851b688346df2fa363af170e6403ab7  ESP32-S3-CAM_PDX_DATV_Beacon.pdf
```
