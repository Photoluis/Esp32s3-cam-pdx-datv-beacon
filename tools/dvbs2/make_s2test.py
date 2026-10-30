"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - casi di prova per il codificatore DVB-S2 del firmware (comando S2TEST).

Per ogni codice (frame normali e corti) con e senza pilot: un TS di prova e i simboli attesi calcolati da encoder.py
(CRC-8 precedente iniziale = 0, come il firmware). Il file va scritto a 0xC00000 nella partizione "symbols":
  python make_s2test.py ../out/s2test.bin
  python -m esptool --port COM3 write-flash 0xE10000 ../out/s2test.bin      (0x210000 + 0xC00000)
Formato: "PDXS2T01", uint32 casi; per caso: uint8 rate (0..10), corto, pilot, frame; uint32 byte TS, byte simboli;
poi TS e simboli impacchettati (4 per byte).
"""
import math
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import encoder as enc  # noqa: E402


def golden_ts(npk: int, seed: int) -> bytes:
    rng = np.random.default_rng(seed)
    p = rng.integers(0, 256, size=(npk, 188), dtype=np.uint8)
    p[:, 0] = 0x47
    return p.tobytes()


def main():
    out = [b""]
    n = 0
    for frame in ("normal", "short"):
        for ri, rate in enumerate(enc.RATES):
            if rate not in enc._TABLES[frame]:
                continue
            for pilots in (False, True):
                e = enc.Encoder(rate, frame, pilots)
                nf = 1
                while (nf * e.frame_symbols) % 4:
                    nf *= 2
                nb = e.dfl // 8
                ts = golden_ts(math.ceil(nf * nb / 187) + 2, 100 + n)
                stream = bytearray()
                crc = 0
                for row in np.frombuffer(ts, np.uint8).reshape(-1, 188):
                    stream.append(crc)
                    stream += row[1:].tobytes()
                    crc = enc.crc8(row[1:].tobytes())
                si, sq = [], []
                for f in range(nf):
                    pos = f * nb
                    first = -(-pos // 188) * 188
                    syncd = (first - pos) * 8 if first < pos + nb else 65535
                    i, q = e.plframe(e.bbframe(bytes(stream[pos:pos + nb]), syncd))
                    si.append(i)
                    sq.append(q)
                sym = enc.pack(np.concatenate(si), np.concatenate(sq))
                out.append(struct.pack("<BBBBII", ri, frame == "short", pilots, nf, len(ts), len(sym)) + ts + sym)
                n += 1
                print(f"{frame:6s} {rate:5s} pilot {int(pilots)}: {nf} frame, TS {len(ts)} B, simboli {len(sym)} B")
    out[0] = b"PDXS2T01" + struct.pack("<I", n)
    data = b"".join(out)
    Path(sys.argv[1]).write_bytes(data)
    print(f"scritto {sys.argv[1]}: {n} casi, {len(data)} byte")


if __name__ == "__main__":
    main()
