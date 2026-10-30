# Pubblicazione su GitHub — timeline

Progetto: **ESP32-S3-CAM_PDX DATV Beacon** — repository proposto `esp32s3-cam-pdx-datv-beacon` — contatto iz7pdx@gmail.com.

Proposta del 30/10/2026. Le date sono indicative: ogni fase si chiude solo con l'ok di IZ7PDX.

| Fase | Quando | Cosa | Esito |
|---|---|---|---|
| 1. Congelamento | 31/10 – 2/11 | prova finale in aria, pulizia del repository, numero di versione | versione candidata v1.0.0-rc1 |
| 2. Documentazione | 3/11 – 5/11 | README in inglese e italiano, guide, PDF | documenti pronti |
| 3. Licenze | 6/11 | intestazioni GPL-3.0, avvisi di terze parti, avvertenze legali | repository pubblicabile |
| 4. Pacchetti | 6/11 – 7/11 | firmware precompilato e immagini standard, istruzioni di programmazione | file della release |
| 5. GitHub | 8/11 | creazione del repository, primo push, release v1.0.0 | **progetto pubblico** |
| 6. Annuncio | 9/11 – 15/11 | BATC, CQ-DATV, ARI, gruppi DATV, video dimostrativo | comunità informata |
| 7. Dopo la v1.0 | da metà novembre | misura in dBm, versione per il traliccio (filtri, PA, case), segnalazioni | v1.1, v1.2 |

## 1. Congelamento (31/10 – 2/11)

- **Prova finale in aria** con la configurazione di serie: ciclo monoscopio / PiP / pausa per almeno un'ora, partenza
  automatica dopo una mancanza di alimentazione, cambio di modo dalla pagina web in due lingue.
- **Pulizia:** togliere dal repository le varianti di prova (`sdkconfig.v4cfg`, `bis*`, cartelle `build_*` già escluse),
  la prova `hwtest/` se non serve più, i commenti con date di lavoro dove non aiutano chi legge.
- **Versione:** il firmware passa da RF0008 a **v1.0.0** (stringa visibile sulla seriale e nella pagina), con un file
  `CHANGELOG.md`.

## 2. Documentazione (3/11 – 5/11)

- **README in inglese** come pagina principale (GitHub è letto in tutto il mondo) con un collegamento al README italiano;
  in alto: foto della ricezione, cosa fa, cosa serve, come provarlo in 10 minuti.
- Guide: compilazione con ESP-IDF, programmazione senza compilare, primo avvio, pagina web, monoscopi personalizzati
  (`docs/MONOSCOPIO.md`), comandi della seriale, prove (`docs/TEST_MER_20261030.md`).
- **PDF del progetto** (`docs/ESP32-S3-CAM_PDX_DATV_Beacon.pdf`) generato da `tools/md2pdf.py`: lo stesso testo del
  repository, consultabile anche offline. Versione inglese del PDF dopo la traduzione.

## 3. Licenze e avvertenze (6/11)

- Intestazione GPL-3.0 con il nome dell'autore in ogni sorgente; file `LICENSE` già presente.
- File `THIRD_PARTY.md`: ESP-IDF, esp32-camera ed esp_h264 (Apache 2.0, con OpenH264 BSD), FFmpeg/x265 usati solo sul PC.
- La norma ETSI resta solo citata con il collegamento (il PDF non va ridistribuito).
- Avvertenze ben visibili: solo radioamatori con licenza, banda e potenza consentite, filtro obbligatorio.

## 4. Pacchetti per chi non compila (6/11 – 7/11)

- **Firmware unico** (`esptool merge-bin`): bootloader, tabella delle partizioni, applicazione, monoscopio e sfondo
  standard in un solo file da scrivere a 0x0 con `esptool` o con un programmatore dal browser.
- Istruzioni passo passo per Windows, Linux e macOS; indicazione della scheda provata e di quelle compatibili.

## 5. GitHub (8/11)

- Da decidere con IZ7PDX: **nome dell'account**; repository `esp32s3-cam-pdx-datv-beacon`; descrizione,
  argomenti (`datv`, `dvb-s2`, `ham-radio`, `esp32-s3`, `amateur-television`, `13cm`).
- Primo push della storia git locale (autore Luigi D'Arcangelo IZ7PDX), etichetta **v1.0.0**, release con firmware
  unico, PDF e note di rilascio.
- Facoltativo: GitHub Pages con la documentazione, sezione Discussions per le domande.

## 6. Annuncio (9/11 – 15/11)

- Piano completo in `docs/DIFFUSIONE.md`: BATC e CQ-DATV, ARI e RadioRivista, ATV Italia, ARI Fidenza, gruppi Facebook,
  AMSAT-DL, associazioni estere, Hackaday, Reddit, QRZ.com.
- Presentazioni pronte in 11 lingue con l'invito a collaborare: `docs/presentazione/` (Markdown + PDF).
- Breve video: accensione, pagina dal telefono, ricezione su Portsdown con MER, cambio monoscopio ↔ PiP, monoscopio di
  Halloween.

## 7. Dopo la v1.0

- **v1.1:** misura della potenza nel canale sul connettore IPEX con lo strumento, taratura in dBm nel firmware e nella
  pagina.
- **v1.2:** versione per il traliccio: filtro passa-banda, amplificatore, contenitore stagno, antenna definitiva;
  stessa prova di linearità con l'amplificatore.
- Raccolta delle segnalazioni, altre lingue nella pagina su richiesta della comunità (oggi 11), symbol rate più alti.
