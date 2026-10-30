# Monoscopio personalizzato

Il beacon ha un **monoscopio standard** (barre colorate, QRZ, frequenza, H.265 640x360) e uno **sfondo standard del PiP**
(barre 320x240). Il firmware non permette di sovrascriverli né di cancellarli: si possono solo **affiancare** con un
monoscopio personalizzato, che si sceglie dal menu.

| Tipo | Come si prepara | Come si carica | Come va in aria |
|---|---|---|---|
| **Immagine fissa** | nessuna preparazione: qualsiasi foto o disegno | pagina web (telefono o PC) oppure seriale | H.264 320x240 generato dall'ESP, con la sovrimpressione |
| **Video** (o immagine in H.265) | `tools/make_es.py` sul PC | pagina web dal PC oppure seriale | H.265 640x360 come preparato, con il tono |

L'immagine personalizzata serve anche come **sfondo del PiP** al posto delle barre (menu: *Sfondo del PiP*).

## Immagine fissa dal telefono

1. Collegarsi alla rete Wi-Fi **`<QRZ>-BEACON`** (password predefinita `beacon13cm`, si cambia dal menu) e aprire
   **http://192.168.4.1**. La rete è attiva solo a TX ferma.
2. Sezione *Monoscopi* → *Immagine personalizzata* → scegliere la foto. La pagina la ritaglia al centro in 4:3, la riduce a
   320x240 e mostra l'anteprima.
3. **Carica immagine**. In *Ciclo del beacon* scegliere *Monoscopio da trasmettere = immagine personalizzata* e/o
   *Sfondo del PiP = immagine personalizzata*, poi **Applica e salva**.

Consigli: soggetto al centro, testo grande (320x240 è una risoluzione bassa), pochi dettagli fini. Con la
sovrimpressione attiva QRZ e frequenza occupano l'angolo in alto a sinistra e il contatore la fascia in basso.

## Video dal PC

Serve Python 3 con `numpy` e `pyserial` e **ffmpeg** (con libx265).

```
cd tools
python make_es.py --video mio_video.mp4 --loops 2 --no-band -o out/mio_es.bin
```

- `--loops N`: durata del loop = N x 19,2 s (da 1 a 5). Il video viene ripetuto se è più corto e tagliato se è più lungo.
- `--image foto.jpg` al posto di `--video`: immagine fissa in H.265 640x360.
- `--no-text`: senza le scritte QRZ/frequenza; `--no-band`: senza la fascia di controllo dei fotogrammi in basso.
- `--callsign` e `--label-freq`: testo delle scritte.
- Il file deve stare in **3 MB**. Il bitrate è calcolato per FEC 1/2, quindi il file va bene con tutti i FEC.
- L'audio è sempre il tono da 1 kHz del beacon.

Caricamento:
- **pagina web dal PC** (collegato a `<QRZ>-BEACON`): *Video personalizzato* → scegliere `out/mio_es.bin` → *Carica video*;
- **seriale (COM)**, a TX ferma: `python upload_media.py COM3 VIDEO out/mio_es.bin` (circa 2 minuti per 1 MB).

Poi nel menu *Monoscopio da trasmettere = video personalizzato*.

## Dalla seriale

```
python upload_media.py COM3 IMAGE foto.jpg          immagine qualsiasi (convertita sul PC a 320x240)
python upload_media.py COM3 VIDEO out/mio_es.bin    video preparato con make_es.py
python upload_media.py COM3 ERASE IMAGE             cancella l'immagine personalizzata (ERASE VIDEO per il video)
```

Comandi del firmware (115200 baud): `MEDIA` mostra i quattro file, `MEDIAERASE VIDEO|IMAGE` cancella un file
personalizzato, `SET monosrc STD|VIDEO|IMAGE`, `SET cammode FULL|PIP`, `SET pipbg STD|IMAGE`, `SET ovl ON|OFF`, poi `SAVE`.

Se il file personalizzato scelto manca, il beacon trasmette il monoscopio standard e lo segnala sulla seriale.

## Per chi prepara la scheda: file standard

```
python make_es.py -o out/mono_es.bin            monoscopio standard
python make_bg.py -o out/bg_std.bin             sfondo standard del PiP
python -m esptool --port COM3 write-flash 0x810000 out/mono_es.bin 0xC10000 out/bg_std.bin
```

Mappa della partizione `symbols` (0x210000): 0x600000 monoscopio standard, 0x700000 video personalizzato (3 MB),
0xA00000 sfondo standard, 0xA40000 immagine personalizzata, 0xC00000 vettori di prova DVB-S2.
