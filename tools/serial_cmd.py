"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - invia comandi senza TX al firmware e stampa le risposte (attesa massima per comando in secondi).

Uso: python serial_cmd.py COM3 60 "CAMBENCH 320 240 100 300 12" ["altro comando" ...]
Per i comandi che trasmettono usare tx_ctl.py.
"""
import sys
import time

import serial


def main():
    port, wait = sys.argv[1], float(sys.argv[2])
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    for cmd in sys.argv[3:]:
        if cmd.split()[0].upper() in ("CW", "TONE", "QPSK", "PLAY", "ESPLAY", "BEACON"):
            raise SystemExit("comando che trasmette: usare tx_ctl.py")
        print(">", cmd, flush=True)
        p.write((cmd + "\n").encode())
        buf, t_end = b"", time.time() + wait
        while time.time() < t_end:
            buf += p.read(512)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text:
                    print(time.strftime("%H:%M:%S"), text, flush=True)
                if text.startswith(("OK ", "ERR")) and not text.startswith("OK simboli"):
                    t_end = min(t_end, time.time() + 0.5)        # ancora mezzo secondo per le righe di dettaglio
    p.close()


if __name__ == "__main__":
    main()
