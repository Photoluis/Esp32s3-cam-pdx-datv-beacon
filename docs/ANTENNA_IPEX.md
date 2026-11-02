# RF output: PCB antenna or IPEX connector

*[Italiano più sotto](#italiano)*

## English

Many ESP32-S3 modules used on the ESP32-S3-CAM boards have **both** a printed (PCB) antenna and an **IPEX (U.FL)
connector**. Only one of them is connected to the chip, chosen by a tiny 0402 component next to the IPEX connector.
**The firmware cannot change this choice**: the board has no RF switch, so there is no "antenna" option in the web page.

On the tested board (GOOUUU ESP32-S3-CAM V1.5) the module left the factory with the RF output on the **PCB antenna**:
a cable plugged into the IPEX connector carries nothing.

### How to recognise the setting

Next to the IPEX connector, close to the R6 / R7 markings, there is a "V"-shaped selection pad: one common pad and two
alternative positions.
- The position towards the track that goes to the printed antenna → **PCB antenna**.
- The position towards the centre pin of the IPEX connector → **IPEX output**.

Only one position carries a component; the other shows two bare tinned pads.

**Check without transmitting (recommended):** with the board unpowered, measure continuity between the IPEX centre pin
and the two pads of the fitted component. No continuity with either pad = the output goes to the PCB antenna.
**Check on air:** transmit at low level and cover the printed antenna with a finger: if the received signal drops a
lot, the signal is coming from there.

### Moving the output to the IPEX connector

1. Unsolder the component from the PCB-antenna position and solder it, rotated, on the IPEX position (between the
   common pad and the IPEX connector). Alternatively fit a new 0 Ω 0402 resistor on the IPEX position and leave the
   other position empty.
2. Use a fine-tip iron or hot air and a magnifier: it is an 0402 part.
3. **Never leave both positions fitted**: the printed antenna would stay in parallel and mismatch the output.
4. The fitted part may have a light-coloured body: it could be a DC-blocking capacitor rather than a 0 Ω resistor.
   It makes no difference for the move, but **reuse the same part** and do not replace it with a different value.
5. After the change, **always connect the pigtail with an antenna, a filter or a 50 Ω load** before transmitting:
   without it the chip transmits into an open connector.

The IPEX output is needed for any **power measurement** (spectrum analyser, power meter) and for an external
**band-pass filter**, which is required for on-air use. The modification is at your own risk and may void the
warranty of the board.

## Italiano

Molti moduli ESP32-S3 montati sulle schede ESP32-S3-CAM hanno **sia** l'antenna stampata sul circuito **sia** il
**connettore IPEX (U.FL)**. Solo uno dei due è collegato al chip, scelto da un minuscolo componente 0402 accanto
all'IPEX. **Il firmware non può cambiare questa scelta**: sulla scheda non c'è un commutatore RF, quindi nella pagina
web non esiste un'opzione "antenna".

Sulla scheda provata (GOOUUU ESP32-S3-CAM V1.5) il modulo è uscito di fabbrica con l'uscita RF sull'**antenna
stampata**: un cavo collegato all'IPEX non porta nulla.

### Come riconoscere l'impostazione

Accanto all'IPEX, vicino alle sigle R6 / R7, c'è una piazzola di selezione a "V": un punto comune e due posizioni
alternative.
- La posizione verso la pista che va all'antenna stampata → **antenna stampata**.
- La posizione verso il centrale dell'IPEX → **uscita IPEX**.

Solo una posizione ha il componente montato; l'altra mostra due piazzole stagnate e nude.

**Verifica senza trasmettere (consigliata):** a scheda spenta, misurare con il tester la continuità tra il centrale
dell'IPEX e le due piazzole del componente montato. Nessuna continuità = l'uscita va all'antenna stampata.
**Verifica in aria:** trasmettere a basso livello e coprire con un dito l'antenna stampata: se il segnale ricevuto cala
molto, l'emissione viene da lì.

### Spostare l'uscita sul connettore IPEX

1. Dissaldare il componente dalla posizione dell'antenna stampata e rimontarlo, ruotato, sulla posizione dell'IPEX
   (tra il punto comune e l'IPEX). In alternativa montare una resistenza nuova da 0 Ω (0402) sulla posizione dell'IPEX
   e lasciare vuota l'altra.
2. Serve un saldatore a punta fine o l'aria calda, con la lente: è un componente 0402.
3. **Non lasciare montate tutte e due le posizioni**: l'antenna stampata resterebbe in parallelo e disadatterebbe
   l'uscita.
4. Il componente montato può avere il corpo chiaro: potrebbe essere un condensatore di blocco invece di una resistenza
   da 0 Ω. Per lo spostamento non cambia nulla, ma **riusarlo così com'è** e non sostituirlo con un altro valore.
5. Dopo la modifica, prima di trasmettere, **collegare sempre il pigtail con un'antenna, un filtro o un carico da
   50 Ω**: senza, il chip trasmette su un connettore aperto.

L'uscita IPEX serve per ogni **misura di potenza** (analizzatore di spettro, wattmetro) e per il **filtro
passa-banda** esterno, obbligatorio per l'uso in aria. La modifica è a proprio rischio e può far decadere la garanzia
della scheda.
