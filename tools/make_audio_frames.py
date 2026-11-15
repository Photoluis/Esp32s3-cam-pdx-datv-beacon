"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - frame AAC precodificati per il suono generato dal firmware (tono, CW ID).

Sull'ESP non c'è un encoder AAC: ffmpeg codifica qui il tono e i blocchi della telegrafia e il firmware (audio_gen.c) li
mette in fila. Un frame AAC sono 1024 campioni a 48 kHz: con frequenze che fanno un numero intero di periodi per frame
(CW 750 Hz = 16, tono 1031,25 Hz = 22) la fase all'inizio di ogni frame è sempre la stessa e i frame si ripetono.

L'encoder ha una memoria (bit reservoir, finestre MDCT sovrapposte): lo stesso contenuto dà byte diversi secondo cosa
c'è prima. Per questo si usano blocchi interi che cominciano e finiscono nel silenzio, codificati in mezzo al silenzio:
  SIL   1 frame di silenzio (ripetuto per le pause)
  DOT   punto + 1 unità di pausa (6 frame; unità = 3 frame = 64 ms, 18,75 WPM)
  DASH  linea + 1 unità di pausa (12 frame)
  TONE0 inizio del tono (frame dal silenzio fino al regime), TONE il frame del tono a regime, ripetuto
Ogni elemento ha fronti a coseno rialzato di 5 ms (niente click), come il CW ID di ESP32-ATV (OK1CDJ).

Verifica: una sequenza di blocchi viene decodificata con ffmpeg e confrontata con il segnale ideale e con la stessa
sequenza codificata tutta d'un pezzo; se i blocchi giuntati sono molto peggio, lo script si ferma.
  python make_audio_frames.py            (scrive firmware/main/audio_frames.h)
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

import numpy as np

RATE = 48000
N = 1024                                   # campioni per frame AAC
CW_HZ = 750.0                              # 16 periodi per frame
TONE_HZ = 1031.25                          # 22 periodi per frame
CW_LEVEL = 0.25                            # -12 dBFS
TONE_LEVEL = 0.125                         # -18 dBFS, come il sine di ffmpeg del monoscopio
EDGE_S = 0.005
UNIT = 3                                   # frame per unità Morse
PAD = 20                                   # frame di silenzio attorno a ciò che si codifica


def frame(kind: str, hz: float, level: float) -> np.ndarray:
    """S silenzio, U salita e tono, T tono, D tono e discesa"""
    n = np.arange(N)
    s = level * np.sin(2 * np.pi * hz * n / RATE)
    e = int(EDGE_S * RATE)
    w = 0.5 - 0.5 * np.cos(np.pi * (np.arange(e) + 0.5) / e)
    env = np.ones(N)
    if kind == "S":
        env[:] = 0
    elif kind == "U":
        env[:e] = w
    elif kind == "D":
        env[N - e:] = w[::-1]
    return s * env


def pcm(kinds: str, hz: float, level: float) -> np.ndarray:
    return np.concatenate([frame(k, hz, level) for k in kinds])


def split_adts(d: bytes) -> list:
    out, i = [], 0
    while i + 7 <= len(d):
        assert d[i] == 0xFF and d[i + 1] & 0xF0 == 0xF0, "sync ADTS perso"
        fl = ((d[i + 3] & 3) << 11) | (d[i + 4] << 3) | (d[i + 5] >> 5)
        assert d[i + 6] & 3 == 0, "un solo raw_data_block per frame"
        out.append(bytes(d[i:i + fl]))
        i += fl
    return out


def encode(x: np.ndarray, kbps: int, td: Path) -> list:
    raw, aac = td / "in.raw", td / "out.aac"
    raw.write_bytes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(RATE), "-ac", "1", "-i", str(raw),
                    "-c:a", "aac", "-b:a", f"{kbps}k", "-f", "adts", str(aac)], check=True)
    return split_adts(aac.read_bytes())


def decode(frames: list, td: Path) -> np.ndarray:
    aac, raw = td / "dec.aac", td / "dec.raw"
    aac.write_bytes(b"".join(frames))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "aac", "-i", str(aac), "-f", "s16le", "-ar", str(RATE),
                    "-ac", "1", str(raw)], check=True)
    return np.frombuffer(raw.read_bytes(), "<i2").astype(np.float64) / 32767


def chunk(kinds: str, hz: float, level: float, kbps: int, td: Path) -> list:
    """frame codificati di un blocco in mezzo al silenzio (il frame 0 dell'encoder è il priming: il frame k contiene il
    frame k dell'ingresso, più la coda della finestra MDCT nel successivo)"""
    fr = encode(pcm("S" * PAD + kinds + "S" * PAD, hz, level), kbps, td)
    return fr[PAD:PAD + len(kinds)]


def steady(fr: list, start: int) -> int:
    """indice del primo frame da cui in poi i frame sono tutti uguali"""
    k = len(fr) - 2
    while k > start and fr[k - 1][7:] == fr[-2][7:]:
        k -= 1
    return k


MORSE = {"A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....", "I": "..",
         "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
         "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-", "Y": "-.--", "Z": "--..", "0": "-----",
         "1": ".----", "2": "..---", "3": "...--", "4": "....-", "5": ".....", "6": "-....", "7": "--...", "8": "---..",
         "9": "----.", "/": "-..-."}


def cw_blocks(text: str) -> list:
    """come audio_gen.c: 'D' punto, 'L' linea, 'S' un frame di silenzio"""
    out = []
    for i, ch in enumerate(text.upper()):
        if ch == " ":
            out += ["S"] * (4 * UNIT)                       # 7 unità tra le parole (3 già dopo la lettera)
            continue
        if ch not in MORSE:
            continue
        for e in MORSE[ch]:
            out.append("D" if e == "." else "L")            # ogni blocco porta già 1 unità di pausa
        out += ["S"] * (2 * UNIT)                           # 3 unità tra le lettere
    return out


def kinds_of(blocks: list) -> str:
    k = {"S": "S", "D": "UTD" + "S" * UNIT, "L": "UTTTTTTTD" + "S" * UNIT}
    return "".join(k[b] for b in blocks)


def snr_db(ref: np.ndarray, x: np.ndarray) -> float:
    """SNR dopo l'allineamento (ritardo del decoder cercato fino a 3 frame)"""
    best = -99.0
    x = np.concatenate([x, np.zeros(3 * N)])                # il decoder ritarda di un frame: senza il priming manca la coda
    for lag in range(0, 3 * N):
        y = x[lag:lag + len(ref)]
        if len(y) < len(ref):
            break
        err = np.sum((y - ref) ** 2)
        best = max(best, 10 * np.log10(np.sum(ref ** 2) / max(err, 1e-12)))
    return best


def c_array(name: str, frames: list) -> str:
    data = b"".join(frames)
    lines = [f"static const uint16_t {name}_len[{len(frames)}] = {{{', '.join(str(len(f)) for f in frames)}}};",
             f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kbps", type=int, default=12, help="bitrate AAC per l'encoder (tono a regime ~21 kbit/s nel TS)")
    ap.add_argument("--test", default="OK1KCI JN79", help="testo della verifica")
    ap.add_argument("-o", "--output", default=str(Path(__file__).parent.parent / "firmware/main/audio_frames.h"))
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as tds:
        td = Path(tds)
        sil = encode(pcm("S" * 3 * PAD, CW_HZ, CW_LEVEL), a.kbps, td)[PAD]
        dot = chunk("UTD" + "S" * UNIT, CW_HZ, CW_LEVEL, a.kbps, td)
        dash = chunk("UTTTTTTTD" + "S" * UNIT, CW_HZ, CW_LEVEL, a.kbps, td)
        tfr = encode(pcm("S" * PAD + "U" + "T" * 200, TONE_HZ, TONE_LEVEL), a.kbps, td)
        k = steady(tfr, PAD)
        tone0, tone = tfr[PAD:k], tfr[k]
        print(f"SIL {len(sil)} B, DOT {[len(f) for f in dot]}, DASH {[len(f) for f in dash]}, "
              f"TONE0 {len(tone0)} frame, TONE {len(tone)} B")

        # verifica: blocchi giuntati contro la stessa sequenza codificata d'un pezzo
        blocks = ["S"] * PAD + cw_blocks(a.test) + ["S"] * PAD
        spliced = []
        for b in blocks:
            spliced += [sil] if b == "S" else dot if b == "D" else dash
        ref = pcm(kinds_of(blocks), CW_HZ, CW_LEVEL)
        s_cw = snr_db(ref, decode(spliced, td))
        s_cw1 = snr_db(ref, decode(encode(ref, a.kbps, td), td))
        tref = pcm("S" * PAD + "U" + "T" * 300, TONE_HZ, TONE_LEVEL)
        s_t = snr_db(tref, decode([sil] * PAD + tone0 + [tone] * (300 + 1 - len(tone0)), td))
        s_t1 = snr_db(tref, decode(encode(tref, a.kbps, td), td))
        print(f"verifica CW \"{a.test}\": giuntato {s_cw:.1f} dB, d'un pezzo {s_cw1:.1f} dB; "
              f"tono: giuntato {s_t:.1f} dB, d'un pezzo {s_t1:.1f} dB")
        if s_cw < min(15.0, s_cw1 - 3.0) or s_t < min(15.0, s_t1 - 3.0):
            raise SystemExit("i frame giuntati non corrispondono al segnale: non scritto")

    h = [f"/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0\n"
         f" * Generato da tools/make_audio_frames.py (AAC-LC mono 48 kHz, ADTS, {a.kbps} kbit/s all'encoder): non modificare.\n"
         f" * CW {CW_HZ:g} Hz, unità {UNIT} frame; tono {TONE_HZ:g} Hz. */",
         "#pragma once", "#include <stdint.h>", "",
         f"#define PDX_AF_UNIT {UNIT}",
         c_array("pdx_af_sil", [sil]), c_array("pdx_af_dot", dot), c_array("pdx_af_dash", dash),
         c_array("pdx_af_tone0", tone0), c_array("pdx_af_tone", [tone]), ""]
    Path(a.output).write_text("\n".join(h))
    print(f"{a.output} scritto")


if __name__ == "__main__":
    main()
