"""ESP32-S3_PDX Parrot (IZ7PDX, GPL-3.0) - carica una cattura .cs8 nella PSRAM della scheda (RXLOAD) e la decodifica
con il ricevitore C del firmware (RXDEC), per il confronto con s2_rx.py.

Uso: python rx_load.py COM5 out/tx1_2s.cs8 [--fs 1000000] [--sr 0] [--nodec]
"""
import argparse
import sys
import time
import zlib
from pathlib import Path

import serial


def wait_line(p, key, timeout):
    buf, t_end = b"", time.time() + timeout
    while time.time() < t_end:
        buf += p.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            if s:
                print(s, flush=True)
            if key in s:
                return s
    raise SystemExit(f"tempo scaduto aspettando {key}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("capture")
    ap.add_argument("--fs", type=int, default=1000000)
    ap.add_argument("--sr", type=int, default=0)
    ap.add_argument("--nodec", action="store_true")
    a = ap.parse_args()
    data = Path(a.capture).read_bytes()
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = a.port, 115200, 0.05
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    p.write(f"RXLOAD {len(data)} {a.fs}\n".encode())
    wait_line(p, "RXLOAD pronto", 5)
    time.sleep(0.3)
    p.baudrate = 230400
    time.sleep(0.2)
    p.reset_input_buffer()
    nblk = (len(data) + 4095) // 4096
    t0, retries = time.time(), 0
    for b in range(nblk):
        d = data[4096 * b:4096 * (b + 1)]
        frame = b"\xA5\x5A" + b.to_bytes(4, "little") + len(d).to_bytes(2, "little") + d + \
            (zlib.crc32(d) & 0xFFFFFFFF).to_bytes(4, "little")
        for attempt in range(8):
            p.write(frame)
            t_end, ack = time.time() + 2.0, b""
            while time.time() < t_end and not ack:
                ack = p.read(1)
            if ack == b"K":
                break
            retries += 1
        else:
            raise SystemExit(f"blocco {b} non accettato")
        if b % 100 == 0:
            print(f"  blocco {b}/{nblk} ({time.time() - t0:.0f} s)", flush=True)
    time.sleep(0.05)
    p.baudrate = 115200
    wait_line(p, "RXLOAD fine", 30)
    print(f"caricati {len(data)} byte in {time.time() - t0:.0f} s, ripetizioni {retries}")
    if not a.nodec:
        p.write(f"RXDEC {a.sr}\n".encode())
        wait_line(p, "RXDEC risultato", 180)
    p.close()


if __name__ == "__main__":
    sys.stdout.reconfigure(errors="replace")
    main()
