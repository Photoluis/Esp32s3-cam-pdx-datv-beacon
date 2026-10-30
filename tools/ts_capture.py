"""Beacon DATV IZ7PDX - cattura il TS del multiplexer del firmware (comando TSDUMP) in un file .ts.

Uso: python ts_capture.py COM3 <pacchetti> out/capture.ts
"""
import sys
import time

import serial


def main():
    port, n, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.5
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    p.write(f"TSDUMP {n}\n".encode())
    out, buf, t_end = bytearray(), b"", time.time() + 30 + n * 0.04
    while time.time() < t_end:
        buf += p.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("ascii", "replace").strip()
            if s.startswith("T "):
                out += bytes.fromhex(s[2:])
            elif s.startswith(("TSDUMP", "ERR")):
                print(s)
                if s.startswith(("TSDUMP fine", "ERR")):
                    t_end = 0
    p.close()
    open(path, "wb").write(out)
    print(f"{path}: {len(out) // 188} pacchetti")


if __name__ == "__main__":
    main()
