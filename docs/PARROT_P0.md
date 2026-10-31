# Modo PARROT DVB-S2 — fase P0: ricevitore di prova sul PC

*31/10/2026 — IZ7PDX. Solo PC: nessuna scheda, nessuna trasmissione.*

## Obiettivo del modo PARROT

Il beacon trasmette, poi **ascolta sulla stessa frequenza** (per esempio 120 s). Se riceve un segnale DVB-S2 ne legge
il **nominativo** (dalla SDT del Transport Stream) e misura il **rapporto di ricezione** (QRK: MER, margine D, scarto
di frequenza, MODCOD, SR). Al ciclo successivo risponde con un beacon di 60 s che riporta nominativo e QRK sul
monoscopio o sulla camera, eventualmente con un fotogramma del segnale ricevuto in PiP.

Scelta di fondo: **cattura e decodifica dopo**. La scheda registra pochi secondi di I/Q in PSRAM e li decodifica con
calma nel tempo che resta prima della risposta: il ricevitore non deve lavorare in tempo reale.

## Cosa fa la P0

`tools/parrot/s2_rx.py` è il prototipo in Python dell'algoritmo che girerà sull'ESP32-S3. Lavora su una cattura I/Q a
1 Msps, int8: il formato che l'S3 produce dopo la decimazione.

1. Spettro medio: presenza del segnale, centro e larghezza a −3 dB → scarto di frequenza e symbol rate (SR) iniziali.
2. Filtro adattato RRC 0,35 e ricampionamento a 4 campioni/simbolo.
3. Frequenza fine con la quarta potenza.
4. Sincronismo di simbolo Oerder-Meyr a blocchi; seconda passata se l'SR stimato è sbagliato (misura l'SR vero).
5. Aggancio dei PLHEADER per correlazione differenziale (SOF + coppie del PLS code) e decodifica del PLS code tra le
   128 parole: MODCOD, frame normale/corto, pilot.
6. Fase da PLHEADER e pilot (senza pilot: quarta potenza per slot, ancorata al PLHEADER), descrambling PL,
   MER con stimatore M2/M4, valori soft (LLR).
7. LDPC min-sum normalizzato. Il BCH non viene decodificato: i CRC-8 del BBHEADER e dei pacchetti dicono se il frame è buono.
8. BBHEADER, ricostruzione del TS, PAT/PMT/SDT → nominativo, codec video/audio, rapporto QRK.

`tools/parrot/s2_synth.py` genera le catture di prova: TS vero da ffmpeg (nominativo in sovrimpressione e nella SDT),
codifica DVB-S2 con il nostro codificatore già verificato sulla norma, modulazione RRC, scarto di frequenza, errore di
clock in ppm, ritardo frazionario, rumore a Es/N0 dato, rumore di fase, quantizzazione int8 come sull'S3.
`tools/parrot/test_p0.py` esegue tutti i casi e confronta il TS ricevuto con quello trasmesso, pacchetto per pacchetto.

## Risultati (14/14)

Catture di 2 s (4 s a 125 kS), fs 1 Msps, int8, 0,3 s di solo rumore prima del segnale.

| Caso | Es/N0 gen. | QRZ letto | MER mis. | D | scarto mis. (gen.) | SR mis. | LDPC ok | TS identici | tempo PC |
|---|---|---|---|---|---|---|---|---|---|
| 333k 3/4 Es/N0 10 | 10 dB | IZ7XYZ | 10.0 | 6.0 | 20000.0 Hz (20000) | 333004 | 20/20 | 642/642 | 2.3 s |
| 333k 3/4 Es/N0 6 | 6 dB | IZ7XYZ | 6.0 | 1.9 | -12000.1 Hz (-12000) | 332997 | 20/20 | 642/642 | 1.6 s |
| 333k 3/4 Es/N0 5 (D~1) | 5 dB | IZ7XYZ | 5.0 | 1.0 | 5000.1 Hz (5000) | 333006 | 20/20 | 642/642 | 2.1 s |
| 333k 3/4 Es/N0 4.3 (soglia) | 4.3 dB | IZ7XYZ | 4.3 | 0.3 | 5000.0 Hz (5000) | 333001 | 20/20 | 642/642 | 3.1 s |
| 333k 1/2 Es/N0 2 | 2 dB | IZ7XYZ | 1.9 | 0.9 | 30000.0 Hz (30000) | 333003 | 20/20 | 427/427 | 3.6 s |
| 250k 2/3 -35 kHz -15 ppm | 8 dB | IZ7XYZ | 8.0 | 4.9 | -35000.0 Hz (-35000) | 249998 | 15/15 | 428/428 | 1.8 s |
| 125k 3/4 +45 kHz | 9 dB | IZ7XYZ | 9.0 | 5.0 | 45000.0 Hz (45000) | 125001 | 15/15 | 481/481 | 1.4 s |
| 500k 3/4 | 9 dB | IZ7XYZ | 9.0 | 5.0 | 10000.0 Hz (10000) | 500004 | 30/30 | 963/963 | 3.7 s |
| 333k 3/4 frame corti | 8 dB | IZ7XYZ | 8.0 | 4.0 | 15000.0 Hz (15000) | 333003 | 79/79 | 610/610 | 1.5 s |
| 333k 3/4 senza pilot | 8 dB | IZ7XYZ | 8.0 | 4.0 | 15000.1 Hz (15000) | 333003 | 20/20 | 642/642 | 1.5 s |
| 333k 3/4 H.265 | 8 dB | IZ7XYZ | 8.0 | 4.0 | 15000.1 Hz (15000) | 333003 | 20/20 | 642/642 | 1.4 s |
| 333k 3/4 rumore di fase | 8 dB | IZ7XYZ | 8.0 | 4.0 | 14999.5 Hz (15000) | 333003 | 20/20 | 642/642 | 1.4 s |
| 333k 9/10 Es/N0 9 | 9 dB | IZ7XYZ | 9.0 | 2.6 | -19999.9 Hz (-20000) | 333004 | 20/20 | 772/772 | 1.9 s |
| solo rumore | -40 dB | — | — | — | — Hz (0) | — | 0/0 | 0/0 | 0.0 s |

- Decodifica completa fino alla **soglia teorica** della norma (D = 0,3 dB con FEC 3/4, D = 0,9 dB con FEC 1/2).
- MER misurato = Es/N0 generato entro 0,1 dB; scarto di frequenza entro 0,5 Hz; SR entro 6 S/s.
- SR coperti con fs = 1 Msps: da 125 a 500 kS; nessun falso allarme sul solo rumore.
- Dal TS ricevuto ffmpeg estrae il primo fotogramma (H.264 Constrained Baseline 640x360): è il materiale per il PiP.

## Cosa resta da verificare (P1, con la scheda)

- **Ricezione vera con l'S3**: il motore di cattura dell'ESP32-S3 (descritto da eSpDR e esp32-sdr-trx, licenza 0BSD)
  usa lo stesso blocco di registri e gli stessi banchi di SRAM del nostro modulatore. Da provare: cattura a 16 Msps,
  decimazione a 1 Msps sul chip, copia in PSRAM, invio della cattura al PC e decodifica con `s2_rx.py`.
- **Sensibilità** del ricevitore Wi-Fi a 2375 MHz (non misurata da nessuno): con il guadagno del ricevitore regolabile.
- **Passaggio TX ↔ RX** sulla stessa radio senza riavvio.
- Serve un trasmettitore DVB-S2 di prova (Portsdown 4 o Pluto a 2375 MHz, potenza minima, attenuatori), solo con
  l'autorizzazione dell'operatore.

## Dimensionamento per l'ESP32-S3 (stime, da confermare in P2)

| Voce | Stima |
|---|---|
| Cattura | 1 Msps × 2 byte = 2 MB/s: 2 s = 4 MB di PSRAM (la TX è ferma, i suoi buffer sono liberi). Con 666 kS/s (2 campioni/simbolo a 333 kS) 1,3 MB/s |
| Filtro adattato | sul PC si usa una FFT di tutta la cattura; sull'S3 FIR polifase nel dominio del tempo, a blocchi |
| LDPC | ~227 000 archi per frame normale 3/4; con buon segnale 2–5 iterazioni, vicino alla soglia ~20. Ordine di grandezza 0,1–1 s per frame in C: bastano i frame che contengono la SDT (e un'immagine IDR per il PiP) |
| Tempo disponibile | ~120 s di ascolto: la decodifica di 20–30 frame entra largamente |
| PiP del ricevuto | possibile con H.264 (decodificatore software esp_h264, da provare su una IDR 640x360 ridotta a 160x90); per H.265 non esiste un decodificatore per l'S3: al suo posto la **costellazione** e lo spettro ricevuti |

## File

- `tools/parrot/s2_synth.py` — generatore di catture DVB-S2 sintetiche (.cs8 + .json)
- `tools/parrot/s2_rx.py` — ricevitore (rapporto, TS, costellazione PNG)
- `tools/parrot/test_p0.py` — banco di prova (14 casi, risultati in `tools/parrot/out/`, fuori da git)

Esempio:
```
cd tools/parrot
python s2_synth.py --call IZ7XYZ --sr 333000 --fec 3/4 --esn0 8 --df 20000 --ppm 10 -o out/cap.cs8 --keep-ts out/src.ts
python s2_rx.py out/cap.cs8 --ts out/rx.ts --png out/const.png --json out/rx.json
```
