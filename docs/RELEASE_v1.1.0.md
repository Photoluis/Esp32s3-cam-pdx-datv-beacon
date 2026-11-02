# ESP32-S3-CAM_PDX DATV Beacon v1.1.0

*Release notes — English first, italiano più sotto.*

## English

**What's new**
- **Selectable symbol rate: 125, 250, 333, 500 and 1000 kS/s** (web page menu, `SET sr`). 500 kS works with every mode
  (test card, camera, PiP: MER 25, smooth 9 fps camera); 1000 kS works with the standard test card (MER 25) — camera,
  PiP and still image automatically fall back to the test card at 1000 kS. A band-pass filter is even more important
  at 1000 kS (1.35 MHz wide signal).
- **QTH locator** (Maidenhead): web field and `SET loc`; shown in the live overlay and in the DVB service provider
  name, so receivers display it with the standard test card too.
- **Output level slider** from −40 to −5 dB (no more typing a minus sign on the iPhone); full keyboard for the DC
  correction fields.
- **Chip temperature in the serial log** at the end of every beacon segment.
- **Test cards without the symbol rate written in the picture** (they are valid at any SR).
- **Easy installation:** browser installer (Chrome / Edge) at https://photoluis.github.io/Esp32s3-cam-pdx-datv-beacon/,
  Windows package with double-click scripts (no Python, no Arduino), step-by-step guide `docs/INSTALL.md`.
- Settings saved by v1.0.0 are converted automatically (SR 333 kS, no locator).

**Assets**

| File | What it is | Flash at |
|---|---|---|
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_full.bin` | complete image: bootloader, partitions, firmware, standard test card and PiP background (erases settings and custom files) | `0x0` |
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_app_only.bin` | firmware update only (keeps settings and custom test cards) | `0x0` |
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_Windows.zip` | Windows package: `esptool.exe`, both images, double-click scripts | — |

**Update from v1.0.0 keeping your settings:** browser installer without *Erase device*, or
`2_UPDATE_firmware_only.bat`, or
`python -m esptool --chip esp32s3 --port COM3 -b 460800 write-flash 0x0 ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_app_only.bin`.

**Known limitations:** camera / PiP / still image only up to 500 kS; about 1 in 100 H.264 frames cannot be decoded in
PiP / still-image modes (software encoder; the TS is intact); output power in dBm not yet calibrated.

**Licensed radio amateurs only**, on the frequencies and at the power allowed by your licence, always with a suitable
output filter. 73 de IZ7PDX

## Italiano

**Novità**
- **Symbol rate selezionabile: 125, 250, 333, 500 e 1000 kS/s** (menu nella pagina web, `SET sr`). 500 kS funziona
  con tutti i modi (monoscopio, camera, PiP: MER 25, camera fluida a 9 fps); 1000 kS con il monoscopio standard
  (MER 25) — a 1000 kS camera, PiP e immagine passano da soli al monoscopio. A 1000 kS il filtro passa-banda è ancora
  più importante (segnale largo 1,35 MHz).
- **QTH locator** (Maidenhead): campo nella pagina web e `SET loc`; in sovrimpressione e nel fornitore del servizio
  DVB, quindi il ricevitore lo mostra anche con il monoscopio standard.
- **Livello con cursore** da −40 a −5 dB (niente più segno meno da digitare sull'iPhone); tastiera completa per la
  correzione DC.
- **Temperatura del chip nel registro seriale** alla fine di ogni segmento del beacon.
- **Monoscopi senza symbol rate scritto nell'immagine** (validi a ogni SR).
- **Installazione facile:** dal browser (Chrome / Edge) su https://photoluis.github.io/Esp32s3-cam-pdx-datv-beacon/,
  pacchetto Windows con script a doppio clic (senza Python né Arduino), guida passo passo `docs/INSTALL_it.md`.
- Le impostazioni salvate dalla v1.0.0 vengono convertite da sole (SR 333 kS, nessun locatore).

**Aggiornare dalla v1.0.0 conservando le impostazioni:** installazione dal browser senza *Erase device*, oppure
`2_UPDATE_firmware_only.bat`, oppure il comando esptool con `..._v1.1.0_app_only.bin` a `0x0`.

**Limiti noti:** camera / PiP / immagine solo fino a 500 kS; circa 1 fotogramma H.264 su 100 non decodificabile in PiP
e immagine fissa; potenza in dBm non ancora tarata.

**Solo per radioamatori con licenza**, sulle frequenze e con le potenze consentite, sempre con un filtro sull'uscita.
73 de IZ7PDX

## SHA-256

```
2815f66664e74f5304158cf44018f16e38a569cbaee0732ed68f9f95b2d4ca98  ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_full.bin
6e8dc8e0e205ea370f0949665427c4283f416264459e93156859082050cbcbed  ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_app_only.bin
3fc1f73fb3e5d70cc11509fef17d0487a98e2779c84f40db2288b599288de841  ESP32-S3-CAM_PDX_DATV_Beacon_v1.1.0_Windows.zip
c4b7d8f73aa65ca02fb74bbc6d137ecc9cfb1399fb2c305c8dc3bcd5556676e5  docs/media/halloween_es.bin
```
