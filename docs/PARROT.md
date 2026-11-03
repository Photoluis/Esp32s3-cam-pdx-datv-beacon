# ESP32-S3_PDX Parrot DVB-S2

*IZ7PDX — progetto locale, non pubblicato. Derivato da [ESP32-S3-CAM_PDX DATV Beacon](https://github.com/Photoluis/Esp32s3-cam-pdx-datv-beacon) (GPL-3.0).*

Beacon DVB-S2 su una sola ESP32-S3 che **ascolta sulla sua stessa frequenza** quando non trasmette, legge il
nominativo di chi riceve (SDT del Transport Stream), misura il rapporto di ricezione (QRK: MER, margine D, scarto di
frequenza, MODCOD, SR) e **risponde al ciclo successivo** con un beacon che riporta QRZ e QRK sul monoscopio o sulla
camera, eventualmente con un fotogramma del segnale ricevuto in PiP.

## Requisiti (decisi dall'utente, 31/10/2026)

- Seconda scheda ESP32-S3-CAM N16R8 (in arrivo), dedicata al Parrot; il beacon resta sulla prima scheda (COM3).
- **Stand-alone**: installazione su traliccio, non presidiata, alimentazione da pannello fotovoltaico.
- **Il PC non fa parte del funzionamento**: serve solo per sviluppo e collaudo. Tutto gira sull'S3.
- **Nessun PA** sul traliccio: stessa antenna per TX e RX, solo filtro passa-banda.
- Radio del chip usata sia come modulatore (TX, come nel beacon) sia come ricevitore (RX: motore di cattura descritto
  da eSpDR / esp32-sdr-trx, licenza 0BSD, stessi registri e banchi SRAM del nostro modulatore).
- Nessuna fretta: si riprende all'arrivo della nuova scheda.

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
| P2 | Ricevitore in C nel firmware (FIR polifase, LDPC, TS, SDT), consumi per il solare | da fare |
| P3 | Ciclo Parrot completo stand-alone | da fare |
| P4 | PiP del fotogramma ricevuto (H.264) o costellazione | da fare |

## Struttura

```
firmware/        firmware del beacon (base del Parrot; la parte RX verrà aggiunta qui)
tools/parrot/    s2_synth.py (catture sintetiche), s2_rx.py (ricevitore), test_p0.py (banco di prova)
tools/dvbs2/     codificatore DVB-S2 (dalla norma ETSI), condiviso con il beacon
docs/            PARROT_P0.md + documenti ereditati dal beacon
```

## Rapporti con il beacon

- Questo repository git è una copia del beacon con tutta la storia. Il beacon è collegato come remote **`beacon`**
  (`C:\PDX_AGENT\esp32s3_datv_beacon`), solo in lettura (push disattivato).
- Portare qui una correzione del beacon: `git fetch beacon` poi `git cherry-pick <commit>` (o `git merge beacon/main`).
- Nessun remote GitHub: si pubblica solo su decisione dell'utente.

## Prove sul PC

Ambiente Python: per ora quello del beacon (`..\esp32s3_datv_beacon\tools\.venv`, numpy + Pillow; ffmpeg nel PATH).
```
cd tools\parrot
..\..\..\esp32s3_datv_beacon\tools\.venv\Scripts\python.exe test_p0.py
```

**Solo per radioamatori con licenza.** Un beacon automatico che risponde da solo va verificato con la propria
licenza / autorizzazione per stazioni automatiche.

## P3 in RF - 04/11/2026

Primo QSO Parrot completo con la TX dell'utente (2375,000 MHz, SR 333, QPSK 3/4 senza pilot):

- `PARROT 120 60 3`: segnale alla 2ª occhiata, cattura di 2 s, decodifica sulla scheda: QRZ **IZ7PDX** dalla SDT, MER 11,2 dB, D 7,2, LDPC 8/19, 543 pacchetti TS senza errori;
- riavvio e risposta in TX per 60 s a SR 333 3/4: buffer mai vuoto, 1082 fotogrammi, 0 persi; dall'ascolto alla risposta 45 s;
- ricevuta dall'utente sul Portsdown 4 con la fascia "QSO IZ7PDX / MER 11.2 dB D7.2 333kS 3/4 -3.7 kHz".

Correzioni emerse nelle prove: rilevatore (continua, cupola del decimatore, disturbi ai bordi), MODCOD letto con la rotazione di frequenza, frequenza fine sui 90 simboli noti dell'header, radio risintonizzata a ogni occhiata.
