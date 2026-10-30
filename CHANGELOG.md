# Changelog

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
- **LED RGB di stato:** blu in configurazione, colore di "calore" proporzionale al livello in trasmissione.
- **Strumenti PC:** monoscopi, sfondi, Halloween, caricamento via COM, cattura e analisi del TS, PDF della documentazione.

Prove in aria con Portsdown 4 e Minitiouner: MER 23–27 dB; prova di linearità su 10 livelli
(`docs/TEST_MER_20261030.md`).

Limiti noti: circa 1 fotogramma H.264 su 100 non decodificabile nei modi PiP e immagine fissa (encoder software
esp_h264; il flusso TS è integro); symbol rate collaudato solo 333 kS; potenza in dBm non ancora tarata.

## Sviluppo (29–30/10/2026)

Versioni interne RF0001…RF0008: dalla prima portante sul DAC del Wi-Fi alla catena DVB-S2 completa sull'ESP, camera,
PiP, monoscopi personalizzati, pagina web multilingue e transverter. Dettagli nella storia git.
