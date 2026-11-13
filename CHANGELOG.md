# Changelog

## In sviluppo

- **Seeed XIAO ESP32S3 / XIAO ESP32S3 Plus senza camera:** variante di compilazione `sdkconfig.defaults.xiao`
  (`CONFIG_PDX_BOARD_XIAO_S3`): console sulla USB nativa (USB-Serial-JTAG), LED giallo su GPIO21 con ritmi al posto
  dei colori, nessuna camera (voci nascoste nella pagina web, `SET cam` solo 0). Guida: `docs/INSTALL.md`.
- **Parrot da 40 kS:** riceve anche 40, 50, 66 e 100 kS (velocità di ESP32-ATV). Occhiate alterne con il LO 80 kHz
  sopra il segnale e la gobba della radio attorno al LO ignorata; cattura di 4 s a 500 ksps (nuova decimazione 32:
  CIC3 /16 e mezza banda /2 sui due core); portanti dei trasmettitori scartate. Ricevitore: frequenza residua stimata
  in ogni frame (a 40 kS i pilot distano 37 ms: con la deriva del trasmettitore l'LDPC passava in 2 frame su 18).
  Provato in aria con ESP32-ATV: 40 kS 19/19 frame MER 19,9 dB, 66 kS 31/31 frame MER 20,9 dB.
- **Parrot, segnali forti:** livello digitale automatico (shift da 8 a 14) quando un'occhiata satura; prima una
  stazione a pochi metri saturava l'uscita int8 e il rilevatore la scartava (9 dB invece di 21).
- **Risposta e beacon da 40 kS:** `SET sr 40|50|66|100`; risposta con l'SR e la lunghezza dei frame ricevuti. Sotto
  100 kS frame corti; immagine fissa (barre dal vivo / risposta del Parrot) a 2 fps senza audio; sotto 150 kbit/s di
  TS PAT/PMT ogni 0,5 s e PCR ogni 100 ms (prima avrebbero occupato quasi tutto il TS). Il monoscopio precodificato non
  sta in un TS così lento: usare `monosrc LIVE` o `IMAGE`. Provato in aria: 40 kS 3/4, 66 kS 3/4 e 1/2 ricevuti
  sull'app ESP32-ATV.

## v1.2.0 — 06/11/2026

- **Modo Parrot:** nelle pause del ciclo il beacon ascolta sulla propria frequenza (±200 kHz), riceve un DVB-S2 QPSK
  fino a 500 kS, lo decodifica sul chip (ricevitore DVB-S2 in C: rilevamento, sincronismo, PL header, LDPC, TS, SDT),
  legge il nominativo e risponde in aria con il rapporto (QSO, MER, margine D, RF in dBFS, SR, FEC, scarto) e un
  riquadro con la costellazione ricevuta (o l'immagine, se H.264 Baseline). La risposta usa l'SR della stazione
  ricevuta. Comandato dalla pagina web (sezione Parrot) e da seriale (`SET parrot/preply/pstab`, `PARROT...`). Provato
  in aria con un Portsdown 4: 333 kS MER 18,6–20,1 dB, 500 kS MER 13,5 dB. Guida: `docs/PARROT_GUIDE.md` / `_it.md`.
- **Schema del ciclo** nella pagina web (11 lingue): TX / ascolto / pausa in scala, passi di un QSO, percentuali e
  istruzioni per il corrispondente; si aggiorna mentre si modificano i campi.
- **LED a semaforo:** verde = ricezione, giallo = standby, rosso lampeggiante = TX (più veloce a livello più alto);
  verde veloce = nominativo acquisito dal Parrot; giallo lento = impostazioni applicate/salvate; breve verde veloce =
  comando dalla seriale o dal web.
- **Modi dal vivo in 16:9** (camera, PiP, immagine, risposta del Parrot: H.264 320x180, fascia centrale delle sorgenti
  320x240); l'immagine caricata dal web viene adattata a 16:9. Fondi dei testi in sovrimpressione trasparenti al 50%.
- **Camera ribaltabile** in orizzontale e in verticale (pagina web, `SET camh/camv`).
- **Dopo ogni TX o stop la scheda si riavvia in configurazione** (radio pulita): prima l'access point dopo uno stop
  poteva non accettare la password. Pulsante di STOP: BOOT = GPIO0 (anche un pulsante esterno verso GND).
- Titolo della pagina "ESP32-S3_CAM_PDX_DATV_BEACON" con il nominativo del beacon sotto; il telefono collegato
  all'access point annulla la partenza automatica.
- Configurazione v6: le impostazioni della v1.1.0 vengono convertite da sole (Parrot spento).
- **Aggiornamento corretto:** il file di aggiornamento ora contiene solo il firmware e si scrive a `0x10000`
  (`..._app_0x10000.bin`); l'installatore dal browser scrive le parti separate. Correzione: nelle v1.0.0 / v1.1.0
  `..._app_only.bin` scritto a `0x0` (e l'installatore dal browser) cancellava le impostazioni, al contrario di quanto
  scritto nelle note. Note di rilascio: `docs/RELEASE_v1.2.0.md`.

## v1.1.0 — 01/11/2026

- **Symbol rate selezionabile:** 125, 250, 333, 500, 1000 kS (`SET sr`, menu nella pagina web, `ESPLAY ... 500k`).
  500 kS con tutti i modi; 1000 kS solo con il monoscopio standard: camera, PiP e immagine passano da soli al
  monoscopio (il buffer dei simboli è più corto della codifica di un fotogramma H.264). SR in sovrimpressione.
  Prove in aria: `docs/TEST_SR_20261101.md` (MER 25 a 500 e 1000 kS, PiP fluido a 500 kS).
- **QTH locator** (Maidenhead, 4 o 6 caratteri): campo nella pagina web e `SET loc`; in sovrimpressione accanto alla
  frequenza e nel fornitore della SDT ("BEACON 2375.000 MHz JN80PS"), quindi visibile anche con il monoscopio standard.
- **Livello con cursore** da −40 a −5 dB (anche il firmware rifiuta valori sopra −5 dB, dove inizia la compressione);
  campi della correzione DC con la tastiera completa (il segno meno ora si digita anche dall'iPhone).
- **Temperatura del chip nel registro** alla fine di ogni segmento del beacon ("BEACON TX OFF ... TEMP 55.3 C").
- **Monoscopi senza SR nell'immagine** (validi a ogni SR): standard e Halloween rigenerati.
- **Installazione:** pagina dal browser (GitHub Pages, ESP Web Tools), pacchetto Windows con `esptool.exe` e script a
  doppio clic, guida `docs/INSTALL.md` / `INSTALL_it.md`; foto della scheda e confronto ESP32-C3 / S3.
- Configurazione salvata compatibile: le impostazioni della v1.0.0 vengono convertite da sole (SR 333 kS, nessun
  locatore).

Prova termica: circa 20 minuti di ciclo continuo a 1000 kS, −7 dB, senza dissipatore: chip a 55 °C (42 °C a riposo).

## v1.0.0 — 31/10/2026 (prima pubblicazione)

Prima versione pubblica di **ESP32-S3-CAM_PDX DATV Beacon**.

- **Modulatore:** trasmettitore Wi-Fi dell'ESP32-S3 usato come modulatore I/Q, QPSK con filtro RRC 0,35 a 333 kS/s,
  2300–2450 MHz, livello di uscita in dB (0 dB = massimo, lineare fino a circa −6 dB).
- **DVB-S2** sull'ESP (ETSI EN 302 307-1): BBFRAME, BCH, LDPC, framing PL, pilot, scrambling; tutti i FEC QPSK da 1/2
  a 9/10, frame normali e corti; identico bit per bit al codificatore di riferimento in Python (42/42 casi).
- **Multiplexer MPEG-TS** sull'ESP: PAT/PMT/SDT, PCR dedicato, PTS sempre crescenti, bitrate costante; flusso continuo tra
  i segmenti del beacon (il ricevitore cambia programma da solo).
- **Video:** monoscopio standard H.265 640x360 con tono AAC; camera dal vivo H.264 320x240; camera in PiP sul monoscopio;
  monoscopio con dati in tempo reale; monoscopi personalizzati (immagine dal telefono o dal PC, video preparato con
  `tools/make_es.py`, anche con musica propria); monoscopio di Halloween con musica originale.
- **Transverter:** uscita come IF verso un mixer esterno; frequenza finale in sovrimpressione, nella SDT e nella pagina
  web; spettro invertito compensato.
- **Beacon autonomo:** ciclo monoscopio / camera / pausa, partenza automatica all'accensione, configurazione in NVS.
- **Configurazione:** pagina web dal telefono (access point a TX ferma) in 11 lingue; comandi sulla seriale.
- **LED RGB di stato:** blu in configurazione, colore di "calore" proporzionale al livello in trasmissione; normale,
  al minimo o spento (alimentazione a batteria / pannello).
- **Telemetria:** temperatura del chip, tempo di accensione e ciclo in sovrimpressione (modi dal vivo, da menu);
  temperatura anche nella pagina web e sulla seriale.
- **Strumenti PC:** monoscopi, sfondi, Halloween, caricamento via COM, cattura e analisi del TS, PDF della documentazione.

Prove in aria con Portsdown 4 e Minitiouner: MER 23–27 dB; prova di linearità su 10 livelli
(`docs/TEST_MER_20261030.md`); prova continua di 20 minuti (9 cicli Halloween / PiP / pausa): buffer dei simboli mai
vuoto, 5513 fotogrammi della camera, nessuno perso, nessun PES in ritardo.

Limiti noti: circa 1 fotogramma H.264 su 100 non decodificabile nei modi PiP e immagine fissa (encoder software
esp_h264; il flusso TS è integro); symbol rate collaudato solo 333 kS; potenza in dBm non ancora tarata.

## Sviluppo (29–30/10/2026)

Versioni interne RF0001…RF0008: dalla prima portante sul DAC del Wi-Fi alla catena DVB-S2 completa sull'ESP, camera,
PiP, monoscopi personalizzati, pagina web multilingue e transverter. Dettagli nella storia git.
