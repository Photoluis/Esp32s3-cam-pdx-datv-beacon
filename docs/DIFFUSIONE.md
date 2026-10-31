# Diffusione del progetto

Dove presentare **ESP32-S3-CAM_PDX DATV Beacon** dopo la pubblicazione su GitHub, con che cosa e in che ordine.
Materiale pronto in `docs/presentazione/`: una presentazione per lingua (Markdown + PDF) con l'invito a collaborare.

## Materiale da allegare ai messaggi

| Materiale | File |
|---|---|
| Presentazione nella lingua del gruppo | `docs/presentazione/ESP32-S3-CAM_PDX_DATV_Beacon_<lingua>.pdf` (it, en, de, es, fr, pt, nl, pl, el, ja, ar) |
| Documento completo (italiano) | `docs/ESP32-S3-CAM_PDX_DATV_Beacon.pdf` |
| Foto della ricezione (MER 26 dB) | `docs/img/test_mer_20261030/05_m9.6dB_MER26.0.jpg` |
| Spettro a 333 kS | `docs/img/spettro_2375_333k.jpg` |
| Monoscopio di Halloween (video) | `docs/media/halloween.mp4` |
| Pagina web dal telefono | `docs/img/web_it.png`, `docs/img/web_en.png` |

## Canali

### Italia

| Canale | Come | Note |
|---|---|---|
| **ARI** — Associazione Radioamatori Italiani ([ari.it](https://www.ari.it)) | notizia per il sito e proposta di articolo per **RadioRivista** | contatto della redazione da cercare sul sito ARI (non trovato con la ricerca) |
| **ATV Italia** | post nel gruppo / pagina | indirizzo da confermare da parte di IZ7PDX |
| **ARI Fidenza** ([arifidenza.it](https://www.arifidenza.it)) | messaggio alla sezione, attiva da tempo nella sperimentazione ATV | verificare il canale attuale (sito / social) |
| Gruppi Facebook italiani di DATV, QO-100 e radioamatori | post con foto, video di Halloween e PDF italiano | scegliere i gruppi in cui IZ7PDX è già iscritto |
| Gruppi ATV regionali (piemontese-ligure, lombardo, triveneto, emiliano, toscano-laziale, pugliese) | contatto diretto | elenco dei gruppi da [qsl.net/i/ik1hgi](https://qsl.net/i/ik1hgi/atv/xxtxhgi.htm) |

### Internazionale

| Canale | Come | Note |
|---|---|---|
| **BATC** — British Amateur Television Club, forum ([forum.batc.org.uk](https://forum.batc.org.uk)) | nuovo argomento nella sezione DATV / Portsdown, PDF inglese | il punto di riferimento mondiale della DATV; wiki con Portsdown e Minitiouner |
| **CQ-DATV** (rivista elettronica gratuita della comunità BATC) | proposta di articolo | indirizzo per gli articoli da chiedere sul forum BATC |
| **AMSAT-DL** forum ([forum.amsat-dl.org](https://forum.amsat-dl.org)) | post nella sezione QO-100 | interessante per l'uso con transverter verso 2,4 GHz |
| **DARC — referato AGAF** (Germania, amateur TV) | messaggio con il PDF tedesco | AGAF è oggi un referato del DARC |
| **REF** (Francia) | messaggio / bollettino con il PDF francese | |
| Associazioni nazionali per le altre lingue | PDF nella lingua: URE (Spagna), REP (Portogallo), VERON (Paesi Bassi), PZK (Polonia), RAAG (Grecia), JARL (Giappone), associazioni dei paesi arabi | canali DATV specifici da individuare |
| **Hackaday** (segnalazione di un progetto) | "tip" con link al repository | pubblica spesso progetti ESP32 per radioamatori |
| Reddit r/amateurradio, r/esp32 | post con foto e link | |
| **QRZ.com** | pagina di IZ7PDX con link al progetto | |

## Ordine consigliato

1. **Giorno 0** — release v1.0.0 su GitHub (con PDF e video di Halloween).
2. **Giorno 0–1** — forum BATC (inglese) e gruppi italiani (ATV Italia, ARI Fidenza, Facebook).
3. **Giorno 2–3** — ARI (notizia + proposta per RadioRivista), AMSAT-DL, CQ-DATV.
4. **Giorno 3–7** — associazioni estere con il PDF nella loro lingua, Hackaday, Reddit.
5. **Sempre** — rispondere alle domande, raccogliere segnalazioni e prove in aria nelle Issues / Discussions.

## Testo breve per i social (italiano)

Happy Halloween a tutti !!! Questo è il mio ultimo progetto: una scheda con telecamera basata sulla nuova  ESP32-S3 (no C3 !) , economica, che si trasforma in un trasmettitore DVB-S2 completo (telecamera inclusa nel kit)!
Il trasmettitore Wi-Fi del chip funge da modulatore I/Q, mentre la codifica video H.264/H.265, il multiplexing TS e la codifica DVB-S2 vengono gestiti interamente dalla scheda.
Copre la banda 2300–2450 MHz (altre bande sono accessibili tramite un mixer), supporta riprese in diretta e funzione PiP (Picture-in-Picture), consente di creare monoscopi personalizzati dallo smartphone ed è configurabile in 11 lingue.
Segnale ricevuto con un MER di 23–27 dB utilizzando Portsdown 4 e Minitiouner. Progetto open source (licenza GPL-3.0): provatelo e inviatemi i vostri report di trasmissione e le vostre idee!

Potete trovare il progetto completo e tutte le istruzioni qui:
https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon

73 de IZ7PDX

## Short text for social media (English)

Happy Halloween, everyone!!! This is my latest project: an affordable camera board based on the new ESP32-S3 (not the C3!) that functions as a complete DVB-S2 transmitter (camera included in the kit)!
The chip’s Wi-Fi transmitter acts as an I/Q modulator, while H.264/H.265 video encoding, TS multiplexing, and DVB-S2 encoding are handled entirely by the board itself.
It covers the 2300–2450 MHz band (other bands can be accessed via a mixer), supports live streaming and a Picture-in-Picture (PiP) function, allows for the creation of custom test patterns from a smartphone, and is configurable in 11 languages.
The signal is received with an MER of 23–27 dB using Portsdown 4 and Minitiouner. It is an open-source project (GPL-3.0 license)—give it a try and send me your transmission reports and ideas!

You can find the complete project and all instructions here:
https://github.com/Photoluis/esp32s3-cam-pdx-datv-beacon

73 de IZ7PDX


## Fonti della ricerca (30/10/2026)

- [BATC](https://batc.org.uk/), [forum BATC](https://forum.batc.org.uk/viewtopic.php?p=27715), [wiki BATC — Portsdown](https://wiki.batc.org.uk/The_Portsdown_Transmitter)
- [CQ-DATV sul forum BATC](https://forum.batc.org.uk/viewtopic.php?p=23321)
- [AMSAT-DL — bandplan QO-100 WB](https://amsat-dl.org/en/p4-a-wb-transponder-bandplan-and-operating-guidelines/)
- [DARC — AGAF ATV news](https://www.darc.de/en/der-club/referate/agaf/aktuelles/)
- [REF](https://en.wikipedia.org/wiki/R%C3%A9seau_des_%C3%89metteurs_Fran%C3%A7ais)
- [ARI — RadioRivista](https://www.ari.it/component/content/article/57-inbacheca/8713-leditoriale-di-maggio.html)
- [Gruppi ATV italiani (IK1HGI)](https://qsl.net/i/ik1hgi/atv/xxtxhgi.htm)
- [Hackaday — progetti ESP32](https://hackaday.com/tag/ESP32/page/37/)
