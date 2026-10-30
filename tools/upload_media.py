"""Beacon DATV IZ7PDX - carica un monoscopio personalizzato sulla scheda via porta seriale (COM).

  python upload_media.py COM3 VIDEO out/mio_es.bin      video preparato con make_es.py --video / --image
  python upload_media.py COM3 IMAGE foto.jpg            immagine qualsiasi (convertita qui a 320x240) o .bin di make_bg.py
  python upload_media.py COM3 ERASE VIDEO|IMAGE         cancella il file personalizzato

Il firmware accetta scritture solo nelle aree personalizzate: il monoscopio standard non si può sovrascrivere né
cancellare. Protocollo: "MEDIAW <tipo> <byte> <crc32>", la scheda risponde READY, poi blocchi da 2048 byte ciascuno
confermato con "ACK <byte ricevuti>", infine "OK caricato: ..." oppure "ERR ...". La TX dev'essere ferma.
"""
import sys
import time
import zlib
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).parent))
import make_bg  # noqa: E402

BLOCK = 2048


_buf = b""


def wait_line(p: serial.Serial, prefixes, timeout: float) -> str:
    global _buf
    end = time.time() + timeout
    while True:
        while b"\n" in _buf:                  # più righe possono arrivare in una sola lettura
            line, _buf = _buf.split(b"\n", 1)
            s = line.decode("utf-8", "replace").strip()
            if s.startswith(prefixes):
                return s
        if time.time() > end:
            raise SystemExit("nessuna risposta dalla scheda")
        _buf += p.read(256)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    port, kind, arg = sys.argv[1], sys.argv[2].upper(), sys.argv[3]
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = p.rts = False                     # niente reset della scheda all'apertura
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    if kind == "ERASE":
        p.write(f"MEDIAERASE {arg.upper()}\n".encode())
        print(wait_line(p, ("OK", "ERR"), 10))
        return
    if kind not in ("VIDEO", "IMAGE"):
        raise SystemExit("tipo VIDEO, IMAGE o ERASE")
    path = Path(arg)
    data = path.read_bytes()
    if kind == "IMAGE" and data[:8] != b"PDXIMG01":
        data = make_bg.build(str(path), path.stem)
    if kind == "VIDEO" and data[:8] != b"PDXES001":
        raise SystemExit("il video va prima preparato con make_es.py (file PDXES001)")
    p.write(f"MEDIAW {kind} {len(data)} {zlib.crc32(data):08x}\n".encode())
    r = wait_line(p, ("READY", "ERR"), 10)
    if r.startswith("ERR"):
        raise SystemExit(r)
    t0 = time.time()
    for off in range(0, len(data), BLOCK):
        p.write(data[off:off + BLOCK])
        r = wait_line(p, ("ACK", "ERR"), 10)
        if r.startswith("ERR"):
            raise SystemExit(r)
        done = min(off + BLOCK, len(data))
        if done % (64 * 1024) < BLOCK or done == len(data):
            print(f"\r{done}/{len(data)} byte", end="", flush=True)
    print()
    print(wait_line(p, ("OK", "ERR"), 20), f"({time.time() - t0:.0f} s)")
    p.close()


if __name__ == "__main__":
    main()
