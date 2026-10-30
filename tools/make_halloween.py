"""ESP32-S3-CAM_PDX DATV Beacon - monoscopio video di Halloween (disegno originale, generato fotogramma per fotogramma).

Cielo al tramonto, luna piena, collina con albero spoglio, zucche intagliate con la candela che tremola, pipistrelli,
un fantasmino e le scritte del progetto, con una musica "spettrale" originale sintetizzata qui: bordone d'organo in re
minore, melodia di theremin con glissati e vibrato, vento e rintocchi di campana. Animazioni e musica hanno periodo
19,2 s: il loop del monoscopio (480 immagini a 25 fps, 900 frame AAC) riparte senza stacchi.

Uso:
  python make_halloween.py -o out/halloween.mp4 [--callsign IL_TUO_QRZ]      (scrive anche out/halloween.wav)
  python make_es.py --video out/halloween.mp4 --audio out/halloween.wav --audio-kbps 48 --no-text --no-band -o out/halloween_es.bin
  python upload_media.py COM3 VIDEO out/halloween_es.bin      (oppure dalla pagina web)
"""
import argparse
import math
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, str(Path(__file__).parent))
from make_symbols import find_ffmpeg  # noqa: E402

W, H, FPS, N = 640, 360, 25, 480          # 19,2 s
T = N / FPS


def font(names, size):
    for n in names:
        for base in ("C:/Windows/Fonts/", "/usr/share/fonts/truetype/dejavu/", "/Library/Fonts/", ""):
            try:
                return ImageFont.truetype(base + n, size)
            except OSError:
                pass
    return ImageFont.load_default()


F_TITLE = font(["impact.ttf", "DejaVuSans-Bold.ttf", "Arial Bold.ttf"], 46)
F_SUB = font(["segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"], 20)
F_SMALL = font(["consolab.ttf", "DejaVuSansMono-Bold.ttf"], 15)


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def background() -> Image.Image:
    """parti fisse: cielo, stelle, luna con alone, collina, albero, recinto"""
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    top, mid, low = (22, 6, 42), (86, 24, 88), (238, 110, 30)
    for y in range(H):
        t = y / (H * 0.78)
        c = lerp(top, mid, min(t * 1.6, 1)) if t < 0.62 else lerp(mid, low, min((t - 0.62) / 0.38, 1))
        d.line([(0, y), (W, y)], fill=c)
    rng = 12345
    for _ in range(70):                                   # stelle (posizioni fisse, pseudo-casuali)
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        x, y = rng % W, (rng // W) % 150
        d.point((x, y), fill=(255, 240, 220))
    glow = Image.new("RGB", (W, H))
    ImageDraw.Draw(glow).ellipse([440, 18, 600, 178], fill=(255, 210, 120))
    im = Image.blend(im, Image.composite(glow, im, glow.convert("L")).filter(ImageFilter.GaussianBlur(28)), 0.55)
    d = ImageDraw.Draw(im)
    d.ellipse([470, 48, 570, 148], fill=(255, 236, 170))  # luna
    for cx, cy, r in ((500, 80, 9), (535, 112, 13), (548, 72, 6)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(236, 210, 140))
    hill = [(0, H)] + [(x, int(292 - 26 * math.sin(x / W * math.pi * 1.2 + 0.3))) for x in range(0, W + 1, 8)] + [(W, H)]
    d.polygon(hill, fill=(10, 4, 14))
    tree = (90, 276)                                      # albero spoglio
    def branch(x, y, ang, ln, w):
        if ln < 6:
            return
        x2, y2 = x + ln * math.cos(ang), y - ln * math.sin(ang)
        d.line([(x, y), (x2, y2)], fill=(10, 4, 14), width=w)
        branch(x2, y2, ang + 0.45, ln * 0.68, max(1, w - 2))
        branch(x2, y2, ang - 0.38, ln * 0.72, max(1, w - 2))
    branch(*tree, math.pi / 2, 70, 9)
    for x in range(300, 640, 22):                         # recinto
        y = int(292 - 26 * math.sin(x / W * math.pi * 1.2 + 0.3))
        d.polygon([(x, y + 4), (x + 8, y + 4), (x + 8, y - 26), (x + 4, y - 32), (x, y - 26)], fill=(10, 4, 14))
    d.line([(300, 280), (640, 266)], fill=(10, 4, 14), width=4)
    return im


def pumpkin(d, cx, cy, r, glow):
    """zucca intagliata; glow 0..1 = luce della candela"""
    for k, dx in enumerate((-0.55, 0.55, -0.28, 0.28, 0)):  # spicchi
        c = (230, 110, 20) if k < 4 else (250, 130, 30)
        d.ellipse([cx + dx * r - r * 0.62, cy - r * 0.78, cx + dx * r + r * 0.62, cy + r * 0.78], fill=c, outline=(150, 60, 10))
    d.rectangle([cx - r * 0.1, cy - r * 1.05, cx + r * 0.1, cy - r * 0.72], fill=(60, 90, 30))
    light = lerp((150, 60, 0), (255, 240, 120), glow)
    d.polygon([(cx - r * 0.55, cy - r * 0.05), (cx - r * 0.3, cy - r * 0.45), (cx - r * 0.1, cy - r * 0.05)], fill=light)
    d.polygon([(cx + r * 0.55, cy - r * 0.05), (cx + r * 0.3, cy - r * 0.45), (cx + r * 0.1, cy - r * 0.05)], fill=light)
    mouth = [(cx - r * 0.6, cy + r * 0.15)]
    for i in range(1, 8):
        mouth.append((cx - r * 0.6 + i * r * 1.2 / 8, cy + r * (0.15 if i % 2 else 0.32)))
    mouth += [(cx + r * 0.6, cy + r * 0.15), (cx + r * 0.35, cy + r * 0.5), (cx - r * 0.35, cy + r * 0.5)]
    d.polygon(mouth, fill=light)


def bat(d, x, y, s, flap):
    wing = 0.5 + 0.5 * math.sin(flap)                     # 0 = ali basse, 1 = ali alte
    up = -10 * wing * s + 4 * s
    pts = [(x - 16 * s, y + up), (x - 9 * s, y + 2 * s), (x - 5 * s, y + up * 0.4 + 3 * s), (x, y - 2 * s),
           (x + 5 * s, y + up * 0.4 + 3 * s), (x + 9 * s, y + 2 * s), (x + 16 * s, y + up), (x + 7 * s, y + 5 * s),
           (x, y + 4 * s), (x - 7 * s, y + 5 * s)]
    d.polygon(pts, fill=(12, 4, 16))


def ghost(im, x, y, alpha):
    g = Image.new("RGBA", (60, 74), (0, 0, 0, 0))
    gd = ImageDraw.Draw(g)
    gd.ellipse([4, 2, 56, 54], fill=(245, 245, 255, alpha))
    gd.rectangle([4, 28, 56, 62], fill=(245, 245, 255, alpha))
    for i in range(4):
        gd.ellipse([4 + i * 13, 56, 17 + i * 13, 72], fill=(245, 245, 255, alpha) if i % 2 == 0 else (0, 0, 0, 0))
    gd.ellipse([18, 20, 26, 32], fill=(20, 10, 30, alpha))
    gd.ellipse([34, 20, 42, 32], fill=(20, 10, 30, alpha))
    gd.ellipse([25, 38, 35, 48], fill=(20, 10, 30, alpha))
    im.paste(g, (int(x), int(y)), g)


def text_outline(d, xy, s, f, fill, stroke=(10, 4, 14), w=3, anchor="mm"):
    d.text(xy, s, font=f, fill=fill, stroke_width=w, stroke_fill=stroke, anchor=anchor)


def frame(bg, n, call):
    t = n / FPS
    w = 2 * math.pi * t / T                               # una rotazione per loop
    im = bg.copy()
    d = ImageDraw.Draw(im)
    # candela: somma di armoniche intere del loop (si ripete esattamente)
    flick = lambda ph: 0.75 + 0.12 * math.sin(37 * w + ph) + 0.08 * math.sin(91 * w + 2 * ph) + 0.05 * math.sin(173 * w)
    pumpkin(d, 250, 300, 34, flick(0))
    pumpkin(d, 330, 318, 24, flick(1.7))
    pumpkin(d, 560, 300, 28, flick(3.1))
    for k in range(5):                                    # pipistrelli: attraversano lo schermo k+1 volte per loop
        laps = (1, 2, 1, 3, 2)[k]
        p = (t / T * laps + k * 0.21) % 1.0
        x = -40 + p * (W + 80)
        y = 70 + 40 * k % 120 + 18 * math.sin(w * laps * 3 + k)
        bat(d, x, y, 0.8 + 0.15 * k % 0.5, w * 48 * (1 + k % 2))
    ghost(im, 150 + 40 * math.sin(w), 150 + 12 * math.sin(4 * w), int(150 + 60 * math.sin(2 * w)))
    d = ImageDraw.Draw(im)
    text_outline(d, (W // 2, 44), "ESP32-S3-CAM_PDX", F_TITLE, (255, 140, 20))
    text_outline(d, (W // 2, 86), "DATV BEACON", F_SUB, (255, 230, 180))
    a = 0.5 + 0.5 * math.sin(2 * w)                       # scritta che "respira"
    text_outline(d, (W // 2, 214), "HAPPY HALLOWEEN", F_TITLE, lerp((200, 70, 10), (255, 170, 40), a))
    if call:
        text_outline(d, (W // 2, 248), call, F_SUB, (240, 240, 255))
    d.rectangle([0, H - 22, W, H], fill=(10, 4, 14))
    d.text((8, H - 19), "DVB-S2 QPSK  SR 333 kS  H.265", font=F_SMALL, fill=(255, 150, 40))
    d.text((W - 8, H - 19), f"FRAME {n + 1:03d}/{N}", font=F_SMALL, fill=(200, 200, 220), anchor="ra")
    return im


# ------------------------------------------------------------------ musica
SR = 48000
NS = int(T * SR)                                         # 921600 campioni = 19,2 s


def loop_hz(f):
    """frequenza arrotondata a un numero intero di cicli nel loop: nessun salto di fase al giro"""
    return round(f * T) / T


def note_hz(name):
    k = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}[name[0]] + (1 if "#" in name else -1 if "b" in name[1:-1] else 0)
    return 440.0 * 2 ** ((k + 12 * (int(name[-1]) - 4)) / 12)


def spooky_audio(seed=1031):
    t = np.arange(NS) / SR
    out = np.zeros(NS)
    # bordone d'organo: re e la gravi, armoniche dispari un po' stonate tra loro (battimenti), tremolo lento
    for base, amp in ((note_hz("D2"), 0.22), (note_hz("A2"), 0.12), (note_hz("D3"), 0.08)):
        for h, ha in ((1, 1.0), (2, 0.45), (3, 0.3), (5, 0.12)):
            for det in (-0.25, 0.25):
                f = loop_hz(base * h + det)
                out += amp * ha * 0.5 * np.sin(2 * np.pi * f * t)
    out *= 0.75 + 0.25 * np.sin(2 * np.pi * (8 / T) * t)
    # theremin: melodia in re minore con glissati, vibrato e dissolvenza agli estremi del loop
    melody = [("D5", 1.6), ("F5", 1.2), ("A5", 2.0), ("G#5", 0.8), ("A5", 1.2), ("F5", 1.2), ("E5", 1.6), ("D5", 1.6),
              ("Bb4", 1.2), ("C#5", 1.6), ("D5", 2.4), ("A4", 1.6), ("D5", 1.2)]
    dur = np.array([d for _, d in melody])
    dur *= T / dur.sum()
    edges = np.concatenate([[0], np.cumsum(dur)])
    fz = np.zeros(NS)
    for i, (n, _) in enumerate(melody):
        a, b = int(edges[i] * SR), int(edges[i + 1] * SR)
        f0 = note_hz(n)
        f1 = note_hz(melody[(i + 1) % len(melody)][0])
        g = np.clip((np.arange(b - a) / SR - (dur[i] - 0.25)) / 0.25, 0, 1)   # glissato negli ultimi 0,25 s
        fz[a:b] = f0 * (f1 / f0) ** (g * g * (3 - 2 * g))
    fz *= 1 + 0.012 * np.sin(2 * np.pi * loop_hz(5.8) * t) * np.clip(t / 2, 0, 1)
    ph = 2 * np.pi * np.cumsum(fz) / SR
    env = np.clip(t / 0.6, 0, 1) * np.clip((T - t) / 0.6, 0, 1)
    out += 0.20 * env * (np.sin(ph) + 0.08 * np.sin(2 * ph))
    # vento: rumore filtrato con raffiche, raccordato a cavallo del giro con una dissolvenza incrociata
    rng = np.random.default_rng(seed)
    xf = SR // 2
    w = rng.standard_normal(NS + xf)
    for _ in range(3):                                   # passa-basso a un polo, tre volte
        y = np.empty_like(w)
        acc = 0.0
        k = 0.02
        for i in range(0, len(w), 4800):                 # a blocchi per non rallentare troppo
            seg = w[i:i + 4800]
            yy = np.empty_like(seg)
            for j, v in enumerate(seg):
                acc += k * (v - acc)
                yy[j] = acc
            y[i:i + 4800] = yy
        w = y
    fade = np.linspace(0, 1, xf)
    w[:xf] = w[:xf] * fade + w[NS:NS + xf] * (1 - fade)
    w = w[:NS]
    w /= np.abs(w).max()
    gust = 0.5 + 0.5 * np.sin(2 * np.pi * (3 / T) * t + 1.0) ** 2
    out += 0.16 * w * gust
    # rintocchi di campana (parziali inarmoniche che decadono) a 1 s e a 10,6 s
    for t0 in (1.0, 10.6):
        tt = t - t0
        m = tt >= 0
        for ratio, amp, tau in ((0.5, 0.5, 4.0), (1.0, 1.0, 2.5), (1.19, 0.6, 2.0), (1.5, 0.4, 1.6), (2.0, 0.35, 1.2), (2.74, 0.2, 0.8)):
            out[m] += 0.10 * amp * np.exp(-tt[m] / tau) * np.sin(2 * np.pi * note_hz("D4") * ratio * tt[m])
    out /= np.abs(out).max() / 0.7                       # circa -3 dBFS
    return out


def write_wav(path, x):
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(SR)
        f.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--callsign", default="", help="nominativo sotto HAPPY HALLOWEEN (predefinito: nessuno)")
    ap.add_argument("-o", "--output", default="out/halloween.mp4")
    ap.add_argument("--png", help="salva anche un fotogramma come immagine")
    a = ap.parse_args()
    bg = background()
    Path(a.output).parent.mkdir(parents=True, exist_ok=True)
    wav = Path(a.output).with_suffix(".wav")
    write_wav(wav, spooky_audio())
    p = subprocess.Popen([find_ffmpeg(), "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-", "-i", str(wav), "-c:v", "libx264", "-crf", "16",
                          "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k", "-shortest", a.output], stdin=subprocess.PIPE)
    for n in range(N):
        im = frame(bg, n, a.callsign)
        if a.png and n == 120:
            im.save(a.png)
        p.stdin.write(im.tobytes())
    p.stdin.close()
    p.wait()
    print(f"{a.output}: {N} immagini, {T:.1f} s, {W}x{H} {FPS} fps, musica in {wav}")


if __name__ == "__main__":
    main()
