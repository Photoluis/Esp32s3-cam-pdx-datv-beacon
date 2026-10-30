"""Beacon DATV IZ7PDX - invio di un comando TX al firmware RF000x sulla porta TTL.

Uso: python tx_ctl.py COM3 "CW 2400.000 200 300" [file_stop]
Apre la porta senza resettare la scheda, invia il comando e stampa le risposte fino a "TX OFF".
Se file_stop esiste durante la TX invia un carattere, che ferma la trasmissione.
"""
import os
import sys
import time

import serial


def main():
    sys.stdout.reconfigure(errors="replace")   # il riavvio della scheda manda caratteri non stampabili
    port, cmd = sys.argv[1], sys.argv[2]
    stop_file = sys.argv[3] if len(sys.argv) > 3 else None
    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False  # DTR e RTS a riposo: nessun reset né modo download
    p.rts = False
    p.open()
    time.sleep(0.2)
    p.reset_input_buffer()
    p.write((cmd + "\n").encode())
    is_tx = cmd.split()[0].upper() in ("CW", "TONE", "QPSK", "PLAY", "ESPLAY", "BEACON")
    buf, t_end, stopped = b"", time.time() + (330 if is_tx else 2), False
    while time.time() < t_end:
        buf += p.read(256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").strip()
            if text:
                print(time.strftime("%H:%M:%S"), text, flush=True)
            if text.startswith(("TX OFF", "ERR")):
                p.close()
                return
        if stop_file and not stopped and os.path.exists(stop_file):
            p.write(b"x")
            stopped = True
            print(time.strftime("%H:%M:%S"), "stop richiesto", flush=True)
    p.close()
    if is_tx:
        print("timeout senza TX OFF", flush=True)


if __name__ == "__main__":
    main()
