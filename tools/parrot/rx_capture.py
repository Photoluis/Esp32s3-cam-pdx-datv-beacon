"""ESP32-S3_PDX Parrot (IZ7PDX, GPL-3.0) - P1a: cattura grezza dal ricevitore dell'ESP32-S3 e analisi sul PC.

Invia "RXCAP <MHz> [guadagno]" sulla seriale, riceve 16384 coppie I/Q a 16 Msps (10 bit con segno), salva la cattura
(.cs16 + .json) e lo spettro (PNG), e stampa livello del rumore, componente continua e righe più forti.

Uso: python rx_capture.py COM5 2399.0 [--gain 60] [--name prova] [--mark 2400.0]
"""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import serial

OUT = Path(__file__).resolve().parent / "out"


def capture(port, mhz, gain, timeout=60):
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    p.write(f"RXCAP {mhz} {gain}\n".encode())
    words, head, buf, t_end = [], None, b"", time.time() + timeout
    while time.time() < t_end:
        buf += p.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            k = s.find("RXCAP ") if "RXCAP " in s else -1
            if s.startswith("R ") or " R " in s[:12]:
                hx = s.split("R ", 1)[1].strip()
                words += [int(hx[i:i + 8], 16) for i in range(0, len(hx) - 7, 8)]
            elif k >= 0 and "fine" in s:
                p.close()
                return head, np.array(words, np.uint32)
            elif k >= 0:
                head = s[k:]
                print(head, flush=True)
            elif "ERR" in s:
                p.close()
                raise SystemExit(s)
    p.close()
    raise SystemExit(f"tempo scaduto: {len(words)} parole ricevute")


def to_iq(w):
    i = (w & 0x3FF).astype(np.int32)
    q = ((w >> 10) & 0x3FF).astype(np.int32)
    i[i >= 512] -= 1024
    q[q >= 512] -= 1024
    return i, q


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("mhz", type=float, help="frequenza del LO")
    ap.add_argument("--gain", type=int, default=60)
    ap.add_argument("--name", default="")
    ap.add_argument("--mark", type=float, action="append", default=[], help="frequenza da cercare (MHz), ripetibile")
    a = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    head, w = capture(a.port, a.mhz, a.gain)
    i, q = to_iq(w)
    fs = 16e6
    name = a.name or f"rx_{a.mhz:.3f}_g{a.gain}"
    np.stack([i, -q], 1).astype(np.int16).tofile(OUT / f"{name}.cs16")   # già coniugato
    # lo spettro del motore di cattura è invertito (prova del 02/11 con l'armonica del quarzo a 2400 MHz): si usa il
    # coniugato, I - jQ, per avere le frequenze al loro posto
    x = (i - i.mean()) - 1j * (q - q.mean())
    n = len(x)
    win = np.hanning(n)
    X = np.fft.fftshift(np.fft.fft(x * win)) / win.sum()
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))
    p = 20 * np.log10(np.abs(X) + 1e-9)
    ps = np.convolve(p, np.ones(5) / 5, "same")
    floor = float(np.median(ps))
    rep = {"head": head, "pairs": int(n), "rms_i": float(i.std()), "rms_q": float(q.std()),
           "dc_i": float(i.mean()), "dc_q": float(q.mean()), "peak_abs": int(max(np.abs(i).max(), np.abs(q).max())),
           "floor_db": round(floor, 1)}
    # righe più forti (lontano dalla continua), a distanza di almeno 20 kHz l'una dall'altra
    order = np.argsort(p)[::-1]
    lines = []
    for k in order:
        if abs(f[k]) < 30e3 or any(abs(f[k] - l[0]) < 20e3 for l in lines):
            continue
        lines.append((float(f[k]), float(p[k])))
        if len(lines) == 6:
            break
    rep["lines"] = [{"off_khz": round(l[0] / 1e3, 1), "rf_mhz": round(a.mhz + l[0] / 1e6, 4),
                     "above_floor_db": round(l[1] - floor, 1)} for l in lines]
    for m in a.mark:
        k = int(np.argmin(np.abs(f - (m - a.mhz) * 1e6)))
        lo, hi = max(0, k - 30), min(n, k + 31)
        kk = lo + int(np.argmax(p[lo:hi]))
        rep.setdefault("marks", []).append({"mhz": m, "found_mhz": round(a.mhz + f[kk] / 1e6, 5),
                                            "above_floor_db": round(float(p[kk]) - floor, 1)})
    (OUT / f"{name}.json").write_text(json.dumps(rep, indent=1), encoding="utf-8")
    print(f"I rms {rep['rms_i']:.1f}  Q rms {rep['rms_q']:.1f}  DC {rep['dc_i']:+.1f}/{rep['dc_q']:+.1f}  "
          f"picco {rep['peak_abs']}/511  rumore {floor:.1f} dB")
    for l in rep["lines"]:
        print(f"  riga {l['rf_mhz']:.4f} MHz ({l['off_khz']:+.1f} kHz)  {l['above_floor_db']:+.1f} dB sul rumore")
    for m in rep.get("marks", []):
        print(f"  cercata {m['mhz']} MHz: trovata {m['found_mhz']} MHz, {m['above_floor_db']:+.1f} dB sul rumore")
    try:
        from PIL import Image, ImageDraw
        W, H = 800, 300
        img = Image.new("RGB", (W, H), (8, 12, 20))
        d = ImageDraw.Draw(img)
        ds = ps[:: max(1, n // W)][:W]
        lo, hi = floor - 10, max(ds.max(), floor + 30)
        pts = [(x_, H - 1 - int((v - lo) / (hi - lo) * (H - 20))) for x_, v in enumerate(ds)]
        d.line(pts, fill=(255, 200, 40))
        d.text((6, 4), f"LO {a.mhz} MHz  fs 16 Msps  gain {a.gain}  span +-8 MHz", fill=(200, 200, 200))
        img.save(OUT / f"{name}.png")
    except ImportError:
        pass


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    main()
