# Modo PARROT — fase P0b: materiale per il PiP e scelta della cattura

*02/11/2026 — IZ7PDX. Solo PC: nessuna scheda, nessuna trasmissione.*

## Cosa aggiunge alla P0

- **Stato CRC di ogni pacchetto TS** ricevuto (il CRC-8 di ogni pacchetto protegge il precedente; il primo dopo un
  buco non è verificabile).
- **Immagine chiave integra** (`video_keyframe`): la prima immagine chiave (H.264 IDR, H.265 IRAP) il cui PES ha
  **tutti** i pacchetti con CRC buono e contatori di continuità in ordine. Se nella cattura non ce n'è una, niente
  immagine: il PiP usa la costellazione.
- **PiP del segnale ricevuto** (`pip_png`, 160x90): fotogramma H.264 se integro (sull'S3 lo decodificherà esp_h264);
  per l'H.265 l'S3 non ha un decodificatore, quindi costellazione.
- **Scelta della cattura migliore** (`best_capture`): prima quelle con QRZ letto, poi senza errori CRC, poi con
  immagine chiave, poi MER più alto.
- Generatore: `--skip` (cattura che parte a metà frame e a metà GOP), `--keyint` (distanza tra immagini chiave),
  `--burst t:durata:dB` (raffiche di disturbo che distruggono alcuni frame).

## Risultati (7/7)

| Caso | QRZ | Immagine chiave | PiP | Esito |
|---|---|---|---|---|
| H.264, cattura a metà frame e a metà GOP | IZ7XYZ | H.264, identica all'originale | fotogramma | OK |
| H.264, GOP di 3 s, cattura di 1,2 s | IZ7XYZ | nessuna (giusto) | costellazione | OK |
| Caso peggiore: raffiche +6 dB ogni 0,25 s (40% dei frame persi; tutte le SDT colpite) | nessuno (nessun dato falso) | nessuna | costellazione | OK |
| H.264, 5 raffiche casuali +6 dB (32 pacchetti con CRC errato) | IZ7XYZ | H.264, identica all'originale (scartate quelle rovinate) | fotogramma | OK |
| H.265, cattura a metà GOP | IZ7XYZ | H.265, identica | costellazione (niente decodificatore sull'S3) | OK |
| Migliore tra 3 catture (Es/N0 5, 10, 7 dB) | — | — | — | scelta la seconda (10 dB): OK |
| Due catture: la prima senza QRZ (SDT perse), la seconda pulita | —, IZ7XYZ | — | — | scelta la seconda: OK |

"Identica" = il flusso elementare dell'immagine estratta coincide byte per byte con un'immagine chiave del TS
trasmesso. Regressione P0: 14/14, TS identico.

## Conseguenze per il firmware (P2/P3)

- Ogni cattura è da 2–2,5 s; ne servono **più di una** per ogni ascolto: le SDT arrivano ogni 0,5–2 s a seconda del
  trasmettitore e un disturbo può colpirle tutte in una sola cattura.
- L'immagine chiave va cercata con la regola "PES tutto integro"; i CRC dei pacchetti bastano, il BCH non serve.
- Se il trasmettitore ha un GOP più lungo della cattura, il PiP sarà la costellazione: è il comportamento giusto.

File: `tools/parrot/test_p0b.py` (banco di prova), `s2_rx.py` (`video_keyframe`, `best_capture`, `pip_png`),
`s2_synth.py` (`--skip`, `--keyint`, `--burst`).
