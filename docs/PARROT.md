# Modo Parrot — storia dello sviluppo

*Guida per l'uso: [PARROT_GUIDE_it.md](PARROT_GUIDE_it.md) · [English](PARROT_GUIDE.md). Questo documento raccoglie
le fasi di sviluppo (31/10–06/11/2026), incluso nel firmware dalla v1.2.0.*

Beacon DVB-S2 su una sola ESP32-S3 che **ascolta sulla sua stessa frequenza** quando non trasmette, legge il
nominativo di chi riceve (SDT del Transport Stream), misura il rapporto di ricezione (QRK: MER, margine D, scarto di
frequenza, MODCOD, SR) e **risponde** con un beacon che riporta QRZ e QRK, con la costellazione ricevuta (o l'immagine
H.264 Baseline) in un riquadro.

## Requisiti di progetto

- **Stand-alone**: installazione su traliccio, non presidiata, alimentazione da pannello fotovoltaico.
- **Il PC non fa parte del funzionamento**: serve solo per sviluppo e collaudo. Tutto gira sull'S3.
- **Nessun PA**: stessa antenna per TX e RX, solo filtro passa-banda.
- Radio del chip usata sia come modulatore (TX, come nel beacon) sia come ricevitore (RX: motore di cattura descritto
  da eSpDR / esp32-sdr-trx, licenza 0BSD, stessi registri e banchi SRAM del nostro modulatore).

## Ciclo previsto

1. **Ascolto** (es. 120 s): ogni secondo un'occhiata allo spettro.
2. Segnale presente → **attesa di stabilizzazione** regolabile (es. 5–10 s) → **più catture brevi** (es. 3 × 2 s,
   1 Msps int8 in PSRAM) → si tiene la migliore (MER più alto, nessun errore CRC).
3. **Decodifica sul chip**: DVB-S2 → TS → SDT (QRZ), MER (QRK); per il PiP solo un'immagine IDR integra (H.264),
   altrimenti la costellazione.
4. **Risposta** (es. 60 s): DVB-S2 con "QRZ + QRK" in sovrimpressione ed eventuale PiP.
5. Nessun segnale → ciclo normale del beacon.

## Fasi

| Fase | Contenuto | Stato |
|---|---|---|
| P0 | Ricevitore DVB-S2 di prova sul PC (Python) su catture sintetiche | **fatta**, 14/14 — [PARROT_P0.md](PARROT_P0.md) |
| P0b | Catture multiple, scelta della migliore, IDR integra, catture che partono a metà GOP | **fatta**, 7/7 — [PARROT_P0b.md](PARROT_P0b.md) |
| P1 | Firmware di cattura RX sull'S3, cattura inviata al PC e decodificata con `s2_rx.py` | **fatta**: QRZ IZ7PDX letto, MER 16,9 dB, PiP estratto — [PARROT_P1.md](PARROT_P1.md) |
| P2 | Ricevitore in C nel firmware (`firmware/main/s2rx.c`: rilevamento, FIR, sincronismo, LDPC, TS, SDT) | **fatta**: 2 s di cattura decodificati sulla scheda in ~21–27 s |
| P3 | Ciclo Parrot completo stand-alone (ascolto nelle pause, riavvio, risposta, ripresa del ciclo) | **fatta**, in aria 04/11 e 06/11 |
| P4 | Riquadro nella risposta: immagine H.264 Baseline o costellazione QPSK | **fatta** (06/11) |

## Struttura

```
firmware/main/   s2rx.c (ricevitore DVB-S2), rx_s3.c (cattura dalla radio), parrot_pip.c (riquadro), main.c (ciclo)
tools/parrot/    s2_synth.py (catture sintetiche), s2_rx.py (ricevitore Python), test_p0*.py, rx_*.py (cattura e
                 caricamento delle catture sulla scheda)
tools/dvbs2/     codificatore DVB-S2 (dalla norma ETSI), condiviso con il beacon
```

## Prove sul PC

Ambiente Python con numpy + Pillow, ffmpeg nel PATH:
```
cd tools/parrot
python test_p0.py
```

**Solo per radioamatori con licenza.** Un beacon automatico che risponde da solo va verificato con la propria
licenza / autorizzazione per stazioni automatiche.

## P3 in RF - 04/11/2026

Primo QSO Parrot completo con la TX di IZ7PDX (2375,000 MHz, SR 333, QPSK 3/4 senza pilot):

- `PARROT 120 60 3`: segnale alla 2ª occhiata, cattura di 2 s, decodifica sulla scheda: QRZ **IZ7PDX** dalla SDT, MER 11,2 dB, D 7,2, LDPC 8/19, 543 pacchetti TS senza errori;
- riavvio e risposta in TX per 60 s a SR 333 3/4: buffer mai vuoto, 1082 fotogrammi, 0 persi; dall'ascolto alla risposta 45 s;
- ricevuta sul Portsdown 4 con la fascia "QSO IZ7PDX / MER 11.2 dB D7.2 333kS 3/4 -3.7 kHz".

Correzioni emerse nelle prove: rilevatore (continua, cupola del decimatore, disturbi ai bordi), MODCOD letto con la rotazione di frequenza, frequenza fine sui 90 simboli noti dell'header, radio risintonizzata a ogni occhiata.

## Ciclo del beacon con il Parrot, in RF - 06/11/2026 (v1.2.0)

Corrispondente: Portsdown 4 su 2390,000 MHz, H.264 High 1024x576, QPSK 3/4.

- 333 kS: tre risposte ricevute; QSO con MER 18,6–20,1 dB, risposta circa 32 s dopo l'inizio della TX.
- 500 kS: QSO con MER 13,5 dB, D 9,5, 577/578 pacchetti; risposta 38 s dopo il segnale.

Correzioni emerse: riavvio con la radio pulita all'inizio di ogni pausa; 1,5 s di riscaldamento della RX e seconda
occhiata di conferma (falsi segnali nel primo secondo); soglia 10 dB; al massimo 700k simboli per decodifica (a 500 kS
la PSRAM non bastava all'LDPC); riquadro conservato nella RAM RTC (la PSRAM non mantiene i dati attraverso il riavvio
software); decoder H.264 di esp_h264 (tinyh264) solo per il profilo Baseline, quindi costellazione con H.264 High e
H.265.
