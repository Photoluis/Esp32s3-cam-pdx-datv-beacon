"""Beacon DATV IZ7PDX - analisi dei tempi di un TS: per ogni PES calcola l'istante di arrivo (dal PCR, interpolato sulla
posizione nel flusso a bitrate costante) e lo confronta con il PTS. Margine = PTS - arrivo: se diventa piccolo o negativo
il decoder riceve i dati in ritardo e li scarta.

Uso: python ts_timing.py file.ts
"""
import sys


def main():
    d = open(sys.argv[1], "rb").read()
    n = len(d) // 188
    pcr = []                                   # (indice pacchetto, PCR in secondi)
    pes = {}                                   # pid -> [(indice, pts)]
    for i in range(n):
        p = d[i * 188:(i + 1) * 188]
        pid = ((p[1] & 0x1F) << 8) | p[2]
        afc = (p[3] >> 4) & 3
        off = 4
        if afc & 2:
            alen = p[4]
            if alen and p[5] & 0x10:
                b = p[6:12]
                base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
                ext = ((b[4] & 1) << 8) | b[5]
                pcr.append((i, (base * 300 + ext) / 27e6))
            off = 5 + alen
        if (p[1] & 0x40) and afc & 1 and off + 14 <= 188 and p[off:off + 3] == b"\x00\x00\x01":
            if p[off + 7] & 0x80:
                q = p[off + 9:off + 14]
                pts = ((q[0] >> 1) & 7) << 30 | q[1] << 22 | (q[2] >> 1) << 15 | q[3] << 7 | q[4] >> 1
                pes.setdefault(pid, []).append((i, pts / 90000))
    (i0, t0), (i1, t1) = pcr[0], pcr[-1]
    rate = (i1 - i0) / (t1 - t0)               # pacchetti al secondo
    print(f"PCR: {len(pcr)} valori, {rate * 1504:.0f} bit/s")
    for pid, lst in pes.items():
        m = [(i, pts - (t0 + (i - i0) / rate)) for i, pts in lst]
        print(f"PID 0x{pid:x}: {len(lst)} PES, margine PTS-arrivo min {min(x for _, x in m):.3f} s, "
              f"max {max(x for _, x in m):.3f} s")
        step = max(1, len(m) // 12)
        print("   " + "  ".join(f"t={t0 + (i - i0) / rate - t0:5.1f}s:{x:+.2f}" for i, x in m[::step]))


if __name__ == "__main__":
    main()
