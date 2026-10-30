"""Beacon DATV IZ7PDX - prove del codificatore DVB-S2.

1. vettore di prova del BB scrambler (figura 5 della norma: uscita 0000 0011 ...)
2. BCH: ogni parola di codice è divisibile per il polinomio generatore
3. LDPC: tutte le equazioni di parità sono soddisfatte
4. confronto con un codificatore indipendente (ESP32-DATV-SA di SP8ESA, solo in locale, licenza PolyForm Noncommercial):
   tabelle LDPC e simboli impacchettati di più PLFRAME, per tutti i rate, frame normali e corti, con e senza pilot.

Uso: python test_encoder.py <cartella host di ESP32-DATV-SA>
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import encoder as enc  # noqa: E402


def golden_ts(npk: int, seed: int = 7) -> bytes:
    rng = np.random.default_rng(seed)
    p = rng.integers(0, 256, size=(npk, 188), dtype=np.uint8)
    p[:, 0] = 0x47
    return p.tobytes()


def main():
    ok = True
    prbs = enc.bb_prbs(16)
    print("BB PRBS primi 16 bit:", "".join(map(str, prbs)), "(atteso 0000001100...)")
    ok &= "".join(map(str, prbs[:8])) == "00000011"

    ref_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    ref = None
    if ref_dir:
        sys.path.insert(0, str(ref_dir))
        import dvbs2 as ref  # noqa: E402
        ref_tables = json.loads((ref_dir / "dvbs2_ldpc.json").read_text())
        mine = json.loads((Path(__file__).parent / "ldpc_tables.json").read_text())
        print("chiavi tabelle del riferimento:", list(ref_tables.keys())[:4], "...")

    for frame in ("normal", "short"):
        for rate in enc._TABLES[frame]:
            for pilots in (False, True):
                e = enc.Encoder(rate, frame, pilots)
                # 4 PLFRAME: multiplo di 4 simboli; TS di lunghezza qualsiasi tagliato a 4 DATA FIELD esatti
                nb = e.dfl // 8
                ts = golden_ts(-(-4 * nb // 187) + 2)
                stream_bytes = 4 * nb
                bb = e.bbframe(bytes(nb), 0)
                cw = e.ldpc.encode(np.concatenate((bb, e.bch.parity(bb))))
                ok_ldpc = e.ldpc.check(cw)
                m = int("".join(map(str, cw[:e.bch.n])), 2)            # BCH: c(x) mod g = 0
                ok_bch = e.bch._mod(m) == 0
                line = f"{frame:6s} {rate:5s} pilot {int(pilots)}  BCH {'OK' if ok_bch else 'KO'}  LDPC {'OK' if ok_ldpc else 'KO'}"
                ok &= ok_bch and ok_ldpc
                if ref:
                    r = ref.Encoder(rate, frame, pilots)
                    out_ref = r.frames(ts)[: 4 * e.frame_symbols // 4]
                    # stesso ingresso: il riferimento parte con CRC precedente 0 e prende i pacchetti in ordine
                    pk = np.frombuffer(ts, np.uint8).reshape(-1, 188)
                    stream = bytearray()
                    crc = 0
                    for row in pk:
                        stream.append(crc)
                        stream += row[1:].tobytes()
                        crc = enc.crc8(row[1:].tobytes())
                    si, sq = [], []
                    for f in range(4):
                        pos = f * nb
                        first = -(-pos // 188) * 188
                        syncd = (first - pos) * 8 if first < pos + nb else 65535
                        i, q = e.plframe(e.bbframe(bytes(stream[pos:pos + nb]), syncd))
                        si.append(i)
                        sq.append(q)
                    out = enc.pack(np.concatenate(si), np.concatenate(sq))
                    same = out == out_ref[:len(out)] and len(out_ref) >= len(out)
                    line += f"  vs riferimento {'UGUALE' if same else 'DIVERSO'} ({len(out)} byte)"
                    ok &= same
                print(line, flush=True)
    print("RISULTATO:", "TUTTO OK" if ok else "ERRORI")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
