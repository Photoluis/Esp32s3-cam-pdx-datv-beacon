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
Then connect to the Wi-Fi network `IZ7PDX-BEACON` (password `beacon13cm`), open http://192.168.4.1 and set **your own
callsign**, frequency and cycle. Change the Wi-Fi password from the menu.

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
Poi collegarsi alla rete `IZ7PDX-BEACON` (password `beacon13cm`), aprire http://192.168.4.1 e impostare **il proprio
nominativo**, la frequenza e il ciclo; cambiare la password del Wi-Fi dal menu.

**Limiti noti:** circa 1 fotogramma H.264 su 100 non decodificabile in PiP e immagine fissa (encoder software; il TS è
integro); collaudato solo a 333 kS; potenza in dBm non ancora tarata.

**Solo per radioamatori con licenza**, sulle frequenze e con le potenze consentite, sempre con un filtro sull'uscita.
Prove in aria, segnalazioni e richieste di miglioramento sono benvenute nelle Issues / Discussions o a
iz7pdx@gmail.com. 73 de IZ7PDX

## SHA-256

```
ba0ebd2ade276ecae00a8e170d3500ffdc349af971f4bd68b57b23e39dc8142f  ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_full.bin
f7981a5529b3161a558b93284baf4c83af3286ab89673e8bd92ab459145b2216  ESP32-S3-CAM_PDX_DATV_Beacon_v1.0.0_app_only.bin
429580ad8af1134e0beb808aaa7489019c55d1c3d66cb3a0b54270056be51039  halloween_es.bin
2b72bcc8b7a89b2c381c9633ab070bfa46c9d988eb88ef82910dfe275b0c31f5  ESP32-S3-CAM_PDX_DATV_Beacon.pdf
```
