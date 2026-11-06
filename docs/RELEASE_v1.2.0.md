# ESP32-S3-CAM_PDX DATV Beacon v1.2.0

*Release notes — English first, italiano più sotto.*

## English

**What's new**
- **Parrot mode:** during the cycle pauses the beacon listens on its own frequency (±200 kHz), receives a DVB-S2 QPSK
  station up to 500 kS, decodes it on the ESP32-S3 itself (our own DVB-S2 receiver: detection, timing, PL header, LDPC,
  TS, SDT), reads its callsign and **answers on the air** with QSO, MER, margin D, RF level, SR, FEC and offset, plus a
  corner box with the received constellation (or the received picture for H.264 Baseline stations). Tested on the air
  with a Portsdown 4: 333 kS MER 18.6–20.1 dB, 500 kS MER 13.5 dB. Guide: `docs/PARROT_GUIDE.md`.
- **Cycle diagram** on the web page (11 languages): TX / listening / pause to scale, the steps of a QSO, time shares and
  instructions for the other station, updated while you type.
- **Traffic-light status LED:** green = receiving, yellow = standby, red blinking = TX (faster at higher level), fast
  green = callsign received, slow yellow = settings saved, short green = command received.
- **16:9 live modes** (camera, PiP, still image, Parrot reply: H.264 320x180); overlay backgrounds 50% transparent.
- **Camera flip** horizontal / vertical.
- **STOP button on GPIO0** (BOOT, or an external push button to GND); after every TX or stop the board restarts in
  configuration mode, so the access point always accepts the password.
- Settings saved by v1.1.0 are converted automatically (Parrot off).

**Assets**

| File | What it is | Flash at |
|---|---|---|
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_full.bin` | complete image: bootloader, partitions, firmware, standard test card and PiP background (erases settings and custom files) | `0x0` |
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_app_0x10000.bin` | firmware update only (keeps settings and custom test cards) | **`0x10000`** |
| `ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_Windows.zip` | Windows package: `esptool.exe`, both images, double-click scripts | — |

**Update keeping your settings:** browser installer without *Erase device*, or `2_UPDATE_firmware_only.bat`, or
`python -m esptool --chip esp32s3 --port COM3 -b 460800 write-flash 0x10000 ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_app_0x10000.bin`.

**Correction for v1.0.0 / v1.1.0:** their `…_app_only.bin`, written at `0x0`, also covers the settings area and
**erases the settings** (custom test cards are kept), contrary to what their notes said; the v1.1.0 browser installer
did the same. From v1.2.0 the update writes only the firmware at `0x10000` and the browser installer writes the parts
separately. Verified on a board: update, back to v1.1.0 and again v1.2.0, settings kept.

**Going back to v1.1.0:** write the v1.1.0 firmware; v1.1.0 does not read the v1.2.0 settings (it starts with the
defaults) but does not delete them: back on v1.2.0 they are there again.

**Known limitations:** Parrot detection threshold about 10 dB above noise; the corner picture only for H.264 Baseline
stations (constellation for H.264 Main/High and H.265); MER read at 500 kS a few dB lower than at 333 kS; camera / PiP /
still image only up to 500 kS; output power in dBm not yet calibrated.

**Licensed radio amateurs only**, on the frequencies and at the power allowed by your licence, always with a suitable
output filter. A station that answers by itself is an automatic station: check your licence. 73 de IZ7PDX

## Italiano

**Novità**
- **Modo Parrot:** nelle pause del ciclo il beacon ascolta sulla propria frequenza (±200 kHz), riceve una stazione
  DVB-S2 QPSK fino a 500 kS, la decodifica sull'ESP32-S3 (ricevitore DVB-S2 nostro: rilevamento, sincronismo, PL
  header, LDPC, TS, SDT), legge il nominativo e **risponde in aria** con QSO, MER, margine D, livello RF, SR, FEC e
  scarto, più un riquadro con la costellazione ricevuta (o l'immagine ricevuta per le stazioni H.264 Baseline). Provato
  in aria con un Portsdown 4: 333 kS MER 18,6–20,1 dB, 500 kS MER 13,5 dB. Guida: `docs/PARROT_GUIDE_it.md`.
- **Schema del ciclo** nella pagina web (11 lingue): TX / ascolto / pausa in scala, passi di un QSO, percentuali e
  istruzioni per il corrispondente, aggiornato mentre si scrive.
- **LED a semaforo:** verde = ricezione, giallo = standby, rosso lampeggiante = TX (più veloce a livello più alto),
  verde veloce = nominativo ricevuto, giallo lento = impostazioni salvate, breve verde = comando ricevuto.
- **Modi dal vivo in 16:9** (camera, PiP, immagine fissa, risposta del Parrot: H.264 320x180); fondi della
  sovrimpressione trasparenti al 50%.
- **Camera ribaltabile** in orizzontale e in verticale.
- **Pulsante di STOP su GPIO0** (BOOT, o un pulsante esterno verso GND); dopo ogni TX o stop la scheda riparte in
  configurazione, così l'access point accetta sempre la password.
- Le impostazioni salvate dalla v1.1.0 vengono convertite da sole (Parrot spento).

**Aggiornare conservando le impostazioni:** installazione dal browser senza *Erase device*, oppure
`2_UPDATE_firmware_only.bat`, oppure il comando esptool con `..._v1.2.0_app_0x10000.bin` a **`0x10000`**.

**Correzione per le v1.0.0 / v1.1.0:** il loro `…_app_only.bin`, scritto a `0x0`, copre anche l'area delle impostazioni
e **cancella le impostazioni** (i monoscopi personalizzati restano), al contrario di quanto dicevano le loro note;
l'installatore dal browser della v1.1.0 faceva lo stesso. Dalla v1.2.0 l'aggiornamento scrive solo il firmware a
`0x10000` e l'installatore dal browser scrive le parti separate. Verificato su una scheda: aggiornamento, ritorno alla
v1.1.0 e di nuovo v1.2.0, impostazioni conservate.

**Tornare alla v1.1.0:** scrivere il firmware v1.1.0; la v1.1.0 non legge le impostazioni della v1.2.0 (parte con i
valori predefiniti) ma non le cancella: tornando alla v1.2.0 ci sono ancora.

**Limiti noti:** soglia di rilevamento del Parrot circa 10 dB sopra il rumore; immagine nel riquadro solo per le stazioni
H.264 Baseline (costellazione per H.264 Main/High e H.265); MER letto a 500 kS qualche dB più basso che a 333 kS;
camera / PiP / immagine solo fino a 500 kS; potenza in dBm non ancora tarata.

**Solo per radioamatori con licenza**, sulle frequenze e con le potenze consentite, sempre con un filtro sull'uscita.
Una stazione che risponde da sola è una stazione automatica: verificate la vostra licenza. 73 de IZ7PDX

## SHA-256

```
6b8b61ae93799e41a7a26247c6809d5a16505c9c385198653220a00dc7831215  ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_full.bin
7a32120f335b62155fcf5b34aabe9a317082ccf89e38aed9b406ef220842510c  ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_app_0x10000.bin
231c7bddba346d6db181f577bc623efc835ea173e6671276ce0daf56f5c6629a  ESP32-S3-CAM_PDX_DATV_Beacon_v1.2.0_Windows.zip
```
