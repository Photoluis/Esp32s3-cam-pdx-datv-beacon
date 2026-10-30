# ESP32-S3-CAM_PDX DATV Beacon

**Un trasmettitore DVB-S2 completo, con camera, su una scheda da poche decine di euro**

*Luigi D'Arcangelo, IZ7PDX — progetto open source, licenza GPL-3.0*

![Ricezione del beacon con Portsdown 4 e Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

Una sola scheda **ESP32-S3 con camera** diventa un beacon di televisione digitale amatoriale. Il trasmettitore Wi-Fi del
chip viene usato come **modulatore I/Q**; tutto il resto — video, multiplexer MPEG-TS, codifica **DVB-S2** secondo la norma
ETSI EN 302 307-1 e modulazione QPSK — è calcolato dalla scheda stessa. Niente SDR, PC o Raspberry: alimentazione,
antenna e filtro.

## Cosa fa

- **DVB-S2 QPSK** a 333 kS/s, FEC da 1/2 a 9/10: ricevuto con **MER 23–27 dB** su Portsdown 4 e Minitiouner.
- **2300–2450 MHz** direttamente; con un **mixer esterno** qualsiasi altra banda: la frequenza finale compare in video e
  nel nome del servizio.
- **Camera dal vivo** (H.264), camera in **Picture-in-Picture** sul monoscopio, monoscopio standard H.265, **monoscopi
  personalizzati**: una foto dal telefono o un video preparato sul PC (c'è anche quello di Halloween!).
- **Beacon autonomo**: ciclo monoscopio / camera / pausa, partenza automatica all'accensione, **LED di stato** che si
  "scalda" con la potenza.
- **Configurazione dal telefono** in 11 lingue, a trasmissione ferma.
- Firmware, strumenti, documentazione e prove **liberi** (GPL-3.0).

## Cosa serve

- Una scheda ESP32-S3 N16R8 con camera (provata: diymore ESP32-S3-CAM, disposizione Freenove).
- Un'antenna per i 13 cm e un **filtro passa-banda**; per distanze maggiori un amplificatore lineare.
- Un ricevitore DVB-S2 (Minitiouner, Portsdown, ricevitori satellitari compatibili).

## Collaboriamo!

Il progetto è appena nato e cresce con la comunità radioamatoriale. Ogni contributo è benvenuto:

- **prove in aria**: distanza, MER, ricevitore e antenne usati, foto della ricezione;
- **segnalazioni** di problemi e **richieste di miglioramento**;
- idee e realizzazioni: amplificatori e filtri, transverter per altre bande, altre schede ESP32-S3, symbol rate più alti;
- **revisione delle traduzioni** della pagina web e dei documenti.

Repository, segnalazioni e discussioni: **{{REPO}}** — contatto: **iz7pdx@gmail.com**

*Avvertenza: da usare solo da radioamatori con licenza valida, sulle frequenze e con le potenze consentite, sempre con
un filtro adeguato sull'uscita.*
