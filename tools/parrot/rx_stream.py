"""ESP32-S3_PDX Parrot (IZ7PDX, GPL-3.0) - P1b: cattura decimata dalla scheda (RXSTREAM + RXDUMP) e decodifica.

Uso: python rx_stream.py COM5 2375 --gain 60 --ms 2000 [--decim 16] [--order 3] [--shift N] [--name prova] [--decode]
Salva out/<name>.cs8 (int8 I/Q, spettro già dritto) e .json; con --decode passa la cattura a s2_rx.receive().
"""
import argparse
import json
import sys
import time
import zlib
from pathlib import Path

import numpy as np
import serial

HERE = Path(__file__).resolve().parent
OUT = HERE / "out"


def lines_until(p, stop, timeout):
    buf, t_end, out = b"", time.time() + timeout, []
    while time.time() < t_end:
        buf += p.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            if s:
                out.append(s)
                print(s, flush=True)
            if stop(s):
                return out, buf
    raise SystemExit("tempo scaduto")


META = {}


def fetch(p, b0, cnt):
    """RXDUMP [b0 cnt]: blocchi A5 5A | indice u32 | lunghezza u16 | dati | CRC32; restituisce i blocchi buoni."""
    p.baudrate = 115200
    p.timeout = 0.02
    p.reset_input_buffer()
    p.write((f"RXDUMP {b0} {cnt}\n" if b0 is not None else "RXDUMP\n").encode())
    hdr, rest = lines_until(p, lambda s: "RXDUMP " in s or "ERR" in s, 10)
    h = [s for s in hdr if "RXDUMP " in s][-1]
    parts = h[h.find("RXDUMP "):].split()
    n, crc = int(parts[1]), int(parts[2], 16)
    META.update(fs=int(parts[3]), lo=int(parts[4]))
    nblk = (n + 4095) // 4096
    want = cnt if cnt is not None else nblk
    p.baudrate = 230400
    p.timeout = 0.2
    buf, good = bytearray(rest), {}
    t_end = time.time() + want * 4114 * 10 / 230400 * 1.5 + 5
    last = time.time()
    while time.time() < t_end:
        chunk = p.read(65536)
        if chunk:
            buf += chunk
            last = time.time()
        elif time.time() - last > 2.0:
            break
        while True:
            i = buf.find(b"\xA5\x5A")
            if i < 0 or len(buf) < i + 8:
                break
            idx = int.from_bytes(buf[i + 2:i + 6], "little")
            ln = int.from_bytes(buf[i + 6:i + 8], "little")
            if ln > 4096 or idx >= nblk:
                del buf[:i + 2]
                continue
            if len(buf) < i + 8 + ln + 4:
                break
            d = bytes(buf[i + 8:i + 8 + ln])
            c = int.from_bytes(buf[i + 8 + ln:i + 12 + ln], "little")
            if zlib.crc32(d) & 0xFFFFFFFF == c:
                good[idx] = d
                del buf[:i + 12 + ln]
            else:
                del buf[:i + 2]
        if b"RXDUMP fine" in buf:
            break
    time.sleep(0.5)
    p.baudrate = 115200
    return good, n, crc

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("mhz", type=float)
    ap.add_argument("--gain", type=int, default=60)
    ap.add_argument("--ms", type=int, default=2000)
    ap.add_argument("--decim", type=int, default=16)
    ap.add_argument("--order", type=int, default=3)
    ap.add_argument("--shift", type=int)
    ap.add_argument("--name", default="")
    ap.add_argument("--decode", action="store_true")
    a = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    name = a.name or f"st_{a.mhz:.3f}_g{a.gain}_d{a.decim}_o{a.order}"
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = a.port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    cmd = f"RXSTREAM {a.mhz} {a.gain} {a.ms} {a.decim} {a.order}" + (f" {a.shift}" if a.shift is not None else "")
    p.write((cmd + "\n").encode())
    stat, _ = lines_until(p, lambda s: "RXSTREAM fine" in s or "ERR" in s, 30 + a.ms / 1000)
    if any("ERR" in s for s in stat):
        raise SystemExit("errore della scheda")
    blocks, n, crc = fetch(p, None, None)
    nblk = (n + 4095) // 4096
    for attempt in range(6):
        missing = [b for b in range(nblk) if b not in blocks]
        if not missing:
            break
        print(f"blocchi da ripetere: {len(missing)}", flush=True)
        # richieste per intervalli contigui
        runs, s0 = [], missing[0]
        for x, y in zip(missing, missing[1:] + [None]):
            if y != x + 1:
                runs.append((s0, x - s0 + 1))
                s0 = y
        for b0, cnt in runs:
            more, _, _ = fetch(p, b0, cnt)
            blocks.update(more)
    p.close()
    data = b"".join(blocks.get(b, b"") for b in range(nblk))
    ok = len(data) == n and (zlib.crc32(data) & 0xFFFFFFFF) == crc
    print(f"ricevuti {len(data)}/{n} byte in {nblk} blocchi, CRC totale {'OK' if ok else 'ERRATO'}")
    if not ok:
        raise SystemExit(1)
    fs, lo = META["fs"], META["lo"]
    (OUT / f"{name}.cs8").write_bytes(data)
    x = np.frombuffer(data, np.int8).astype(np.float32)
    i, q = x[0::2], x[1::2]
    info = {"cmd": cmd, "board": stat, "fs": fs, "lo_hz": lo, "samples": len(i),
            "rms_i": float(i.std()), "rms_q": float(q.std()), "dc_i": float(i.mean()), "dc_q": float(q.mean()),
            "sat_i": int(np.sum(np.abs(i) >= 127)), "sat_q": int(np.sum(np.abs(q) >= 127))}
    (OUT / f"{name}.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    print(f"I rms {info['rms_i']:.1f} DC {info['dc_i']:+.1f} sat {info['sat_i']} | Q rms {info['rms_q']:.1f} "
          f"DC {info['dc_q']:+.1f} sat {info['sat_q']}  ({len(i)} campioni a {fs} S/s)")
    if a.decode:
        sys.path.insert(0, str(HERE))
        import s2_rx
        z = (i - i.mean()) + 1j * (q - q.mean())
        rep, ts, const = s2_rx.receive(z.astype(np.complex128), fs)
        rep.pop("_kf", None)
        if const is not None:
            s2_rx.const_png(const, OUT / f"{name}_const.png")
        if ts:
            (OUT / f"{name}.ts").write_bytes(b"".join(ts))
        (OUT / f"{name}_rx.json").write_text(json.dumps(rep, indent=1, default=str), encoding="utf-8")


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    main()
