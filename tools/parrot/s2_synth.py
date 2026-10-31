"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - Parrot P0: generatore di catture I/Q DVB-S2 sintetiche.

Crea un TS (ffmpeg: testsrc2 + nominativo, H.264 o H.265, tono AAC, SDT con service_name / service_provider),
lo codifica in DVB-S2 con tools/dvbs2/encoder.py, lo modula QPSK RRC 0,35 e lo campiona come farebbe il ricevitore
dell'ESP32-S3 dopo la decimazione: fs = 1 Msps, int8 I/Q interlacciati (.cs8), con scarto di frequenza, errore del
clock (ppm), ritardo frazionario, rumore bianco a Es/N0 dato e una parte iniziale/finale di solo rumore.

Esempio:
  python s2_synth.py --call IZ7XYZ --sr 333000 --fec 3/4 --esn0 8 --df 25000 --ppm 12 --seconds 3 -o out/cap.cs8
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dvbs2"))
import encoder as s2  # noqa: E402

SQ2 = np.sqrt(0.5)
NULL_PACKET = bytes([0x47, 0x1F, 0xFF, 0x10]) + bytes(184)


def rrc_H(f: np.ndarray, sr: float, beta: float = 0.35) -> np.ndarray:
    """risposta in frequenza (ampiezza) del filtro a radice di coseno rialzato, 1 in banda."""
    a = np.abs(f)
    f1, f2 = (1 - beta) * sr / 2, (1 + beta) * sr / 2
    h = np.zeros_like(a)
    h[a <= f1] = 1.0
    m = (a > f1) & (a < f2)
    h[m] = np.sqrt(0.5 * (1 + np.cos(np.pi / (beta * sr) * (a[m] - f1))))
    return h


def make_ts(call: str, provider: str, bitrate: float, seconds: float, codec: str, path: Path) -> None:
    ff = shutil.which("ffmpeg")
    if not ff:
        raise SystemExit("ffmpeg non trovato")
    video_k = max(60, int(bitrate * 0.72 / 1000) - 32)
    font = "C\\:/Windows/Fonts/arialbd.ttf"
    vf = (f"drawtext=fontfile='{font}':text='{call}':fontcolor=white:fontsize=64:box=1:boxcolor=black@0.6:"
          f"boxborderw=10:x=(w-text_w)/2:y=40,"
          f"drawtext=fontfile='{font}':text='PARROT P0 TEST':fontcolor=yellow:fontsize=28:x=(w-text_w)/2:y=290")
    venc = (["-c:v", "libx264", "-profile:v", "baseline", "-preset", "veryfast", "-g", "25", "-bf", "0",
             "-b:v", f"{video_k}k", "-maxrate", f"{video_k}k", "-bufsize", f"{video_k}k"] if codec == "h264" else
            ["-c:v", "libx265", "-preset", "fast", "-b:v", f"{video_k}k",
             "-x265-params", f"keyint=25:min-keyint=25:bframes=0:vbv-maxrate={video_k}:vbv-bufsize={video_k}:log-level=error"])
    cmd = [ff, "-hide_banner", "-loglevel", "error", "-y",
           "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25", "-f", "lavfi", "-i", "sine=frequency=800:sample_rate=48000",
           "-t", f"{seconds:.3f}", "-vf", vf, *venc, "-c:a", "aac", "-b:a", "32k", "-ac", "1",
           "-f", "mpegts", "-muxrate", str(int(bitrate)), "-pcr_period", "40", "-sdt_period", "0.5",
           "-metadata", f"service_name={call}", "-metadata", f"service_provider={provider}", str(path)]
    subprocess.run(cmd, check=True)


def encode_frames(e: "s2.Encoder", ts: bytes, nframes: int):
    """come Encoder.encode_loop ma per un numero qualsiasi di PLFRAME (TS allungato con pacchetti nulli)."""
    nb = e.dfl // 8
    need = -(-nframes * nb // 188) + 1
    if len(ts) // 188 < need:
        ts += NULL_PACKET * (need - len(ts) // 188)
    pk = np.frombuffer(ts, np.uint8).reshape(-1, 188)
    stream = bytearray()
    crc_prev = 0
    for row in pk:
        stream.append(crc_prev)
        stream += row[1:].tobytes()
        crc_prev = s2.crc8(row[1:].tobytes())
    out_i, out_q = [], []
    for f in range(nframes):
        pos = f * nb
        first = -(-pos // 188) * 188
        syncd = (first - pos) * 8 if first < pos + nb else 65535
        i, q = e.plframe(e.bbframe(bytes(stream[pos:pos + nb]), syncd))
        out_i.append(i)
        out_q.append(q)
    return np.concatenate(out_i), np.concatenate(out_q), ts


def modulate(si, sq, sr, fs, ppm, df, esn0_db, tau0, phase0, lead_s, tail_s, rms, rng, pn_deg=0.0):
    sym = (si.astype(np.float64) + 1j * sq) * SQ2
    sps = 4
    n4 = len(sym) * sps
    up = np.zeros(n4, np.complex128)
    up[::sps] = sym
    U = np.fft.fft(up)
    k = np.fft.fftfreq(n4) * n4                                      # indice di bin con segno
    U *= rrc_H(np.fft.fftfreq(n4, 1 / (sps * sr)), sr)
    U *= np.exp(-2j * np.pi * k / n4 * sps * tau0)                  # ritardo frazionario (in simboli)
    sr_t = sr * (1 + ppm * 1e-6)
    nout = int(round(len(sym) * fs / sr_t))
    Y = np.zeros(nout, np.complex128)
    h = min(n4, nout) // 2
    Y[:h] = U[:h]
    Y[-h:] = U[-h:]
    y = np.fft.ifft(Y) * (nout / n4)
    n = np.arange(nout)
    ph = 2 * np.pi * df * n / fs + phase0
    if pn_deg:                                                       # rumore di fase a passeggiata casuale
        ph += np.cumsum(rng.normal(0, np.deg2rad(pn_deg), nout))
    y *= np.exp(1j * ph)
    p = np.mean(np.abs(y) ** 2)
    lead, tail = int(lead_s * fs), int(tail_s * fs)
    x = np.concatenate((np.zeros(lead, np.complex128), y, np.zeros(tail, np.complex128)))
    sig2 = p * fs / (sr * 10 ** (esn0_db / 10))
    x += np.sqrt(sig2 / 2) * (rng.standard_normal(len(x)) + 1j * rng.standard_normal(len(x)))
    x *= rms / np.sqrt(np.mean(np.abs(x) ** 2))
    q = np.empty(2 * len(x), np.int8)
    q[0::2] = np.clip(np.round(x.real), -127, 127)
    q[1::2] = np.clip(np.round(x.imag), -127, 127)
    return q, lead


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--call", default="IZ7XYZ")
    p.add_argument("--provider", default="PORTSDOWN 4")
    p.add_argument("--sr", type=int, default=333000)
    p.add_argument("--fec", default="3/4")
    p.add_argument("--short", action="store_true")
    p.add_argument("--nopilots", action="store_true")
    p.add_argument("--codec", choices=["h264", "h265"], default="h264")
    p.add_argument("--esn0", type=float, default=10.0, help="Es/N0 in dB")
    p.add_argument("--df", type=float, default=20000.0, help="scarto di frequenza, Hz")
    p.add_argument("--ppm", type=float, default=10.0, help="errore del clock dei simboli, ppm")
    p.add_argument("--tau", type=float, default=0.37, help="ritardo frazionario, simboli")
    p.add_argument("--pn", type=float, default=0.0, help="rumore di fase, gradi rms per campione (passeggiata)")
    p.add_argument("--seconds", type=float, default=3.0, help="durata del segnale")
    p.add_argument("--lead", type=float, default=0.3)
    p.add_argument("--tail", type=float, default=0.1)
    p.add_argument("--fs", type=float, default=1e6)
    p.add_argument("--rms", type=float, default=25.0, help="livello rms in conteggi int8")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--keep-ts", help="salva il TS sorgente")
    a = p.parse_args()

    e = s2.Encoder(a.fec, "short" if a.short else "normal", not a.nopilots)
    bitrate = e.ts_bitrate(a.sr)
    nframes = max(2, int(a.seconds * a.sr / e.frame_symbols))
    with tempfile.TemporaryDirectory() as td:
        tsp = Path(td) / "src.ts"
        make_ts(a.call, a.provider, bitrate, nframes * e.frame_symbols / a.sr + 0.5, a.codec, tsp)
        ts = tsp.read_bytes()
    si, sq, ts = encode_frames(e, ts, nframes)
    rng = np.random.default_rng(a.seed)
    q, lead = modulate(si, sq, a.sr, a.fs, a.ppm, a.df, a.esn0, a.tau, 1.0, a.lead, a.tail, a.rms, rng, a.pn)
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    q.tofile(out)
    meta = {k: getattr(a, k) for k in ("call", "provider", "sr", "fec", "short", "nopilots", "codec", "esn0", "df", "ppm",
                                       "tau", "pn", "fs", "lead")}
    meta.update(frames=nframes, frame_symbols=e.frame_symbols, ts_bitrate=bitrate, samples=len(q) // 2)
    out.with_suffix(".json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    if a.keep_ts:
        Path(a.keep_ts).write_bytes(ts)
    print(f"{out}: {len(q) // 2} campioni a {a.fs / 1e6:g} Msps, {nframes} PLFRAME QPSK {a.fec} "
          f"{'short' if a.short else 'normal'} pilot {'no' if a.nopilots else 'si'}, SR {a.sr}, Es/N0 {a.esn0} dB, "
          f"df {a.df:+.0f} Hz, {a.ppm:+g} ppm, TS {bitrate:.0f} bit/s")


if __name__ == "__main__":
    main()
