"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - Parrot P0: ricevitore DVB-S2 (QPSK, CCM, TS) su una cattura I/Q.

Prototipo in Python dell'algoritmo che girerà sull'ESP32-S3 in modalità "cattura e decodifica dopo":
  1. spettro medio: presenza del segnale, centro e larghezza a metà potenza -> scarto di frequenza e symbol rate
  2. ricampionamento in frequenza a 4 campioni/simbolo con il filtro adattato RRC 0,35
  3. frequenza fine con la quarta potenza
  4. sincronismo di simbolo Oerder-Meyr a blocchi (con stima e correzione dell'errore di SR), interpolazione cubica
  5. aggancio dei PLHEADER per correlazione differenziale (SOF + coppie del PLS code), decodifica del PLS code (128 parole)
  6. fase da PLHEADER e pilot (oppure quarta potenza senza pilot), descrambling PL, MER, LLR
  7. LDPC min-sum normalizzato (BCH non decodificato: i CRC-8 di BBHEADER e dei pacchetti dicono se il frame è buono)
  8. BBHEADER, ricostruzione dei pacchetti TS, PAT / PMT / SDT -> nominativo, codec, rapporto QRK (MER e margine D)

Uso:
  python s2_rx.py out/cap.cs8 [--fs 1e6] [--ts out/rx.ts] [--png out/const.png] [--json out/rx.json]
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dvbs2"))
import encoder as s2  # noqa: E402

SQ2 = np.sqrt(0.5)
ROLLOFF = 0.35
SR_STD = [66000, 125000, 250000, 333000, 500000, 1000000, 1500000, 2000000]
# soglie Es/N0 ideali QPSK (EN 302 307-1 tabella 13), per il margine "D" dei rapporti DATV
THRESH = {"1/4": -2.35, "1/3": -1.24, "2/5": -0.30, "1/2": 1.00, "3/5": 2.23, "2/3": 3.10, "3/4": 4.03,
          "4/5": 4.68, "5/6": 5.18, "8/9": 6.20, "9/10": 6.42}
STREAM_TYPES = {0x01: "MPEG-1 video", 0x02: "MPEG-2 video", 0x1B: "H.264", 0x24: "H.265", 0x03: "MPEG audio",
                0x04: "MPEG audio", 0x0F: "AAC", 0x11: "AAC LATM", 0x81: "AC-3", 0x06: "privato"}
CALL_RE = re.compile(r"\b(?:[A-Z0-9]{1,3}/)?[A-Z]{0,2}[0-9]?[A-Z]{1,2}[0-9][A-Z0-9]{0,3}[A-Z](?:/[A-Z0-9]{1,4})?\b")


# --------------------------------------------------------------------------------------------- PLHEADER

def pls_bits(v: int) -> np.ndarray:
    """64 bit del PLS code (già con lo scrambling) per v = MODCOD(5) | TYPE(2)."""
    b = s2.bits_msb(v, 7)
    y = 0
    for k in range(6):
        if b[k]:
            y ^= s2.PLS_ROWS[k]
    out = []
    for bit in s2.bits_msb(y, 32):
        out += [bit, bit ^ b[6]]
    return np.array([o ^ int(c) for o, c in zip(out, s2.PLS_SCRAMBLE)], np.int8)


PLS_A = np.array([1 - 2 * pls_bits(v) for v in range(128)], np.float64)          # 128 x 64, valori +-1
SOF_A = 1 - 2 * np.array(s2.bits_msb(s2.SOF, 26), np.float64)
U90 = np.where(np.arange(90) % 2 == 0, 1.0, -1.0) + 1j                         # pi/2 BPSK: h = a * U / sqrt(2)
U90 *= SQ2


def header_symbols(v: int) -> np.ndarray:
    return np.concatenate((SOF_A, PLS_A[v])) * U90


def diff_refs():
    """riferimenti per la correlazione differenziale D[n] = y[n] conj(y[n-1]) sul PLHEADER, per TYPE LSB 0 e 1."""
    refs = []
    sc = np.array([int(c) for c in s2.PLS_SCRAMBLE])
    for b7 in (0, 1):
        r = np.zeros(90, np.complex128)
        for n in range(1, 26):
            r[n] = SOF_A[n] * SOF_A[n - 1] * U90[n] * np.conj(U90[n - 1])
        for i in range(32):
            n = 27 + 2 * i
            sign = 1 - 2 * (sc[2 * i] ^ sc[2 * i + 1] ^ b7)
            r[n] = sign * U90[n] * np.conj(U90[n - 1])
        refs.append(r)
    return refs


DREFS = diff_refs()


def frame_geom(v: int):
    """(lunghezza in simboli, indici dei dati, blocchi pilot, frame, rate) per MODCOD QPSK; MODCOD 0 = DUMMY."""
    modcod, short, pilots = v >> 2, (v >> 1) & 1, v & 1
    if modcod == 0:
        return 90 + 36 * 90, None, [], None, None
    if modcod > 11:
        return None, None, [], None, None                             # 8PSK / APSK: non gestiti
    nldpc = 16200 if short else 64800
    slots = nldpc // 2 // 90
    idx, data, pbl = 90, [], []
    for s in range(0, slots, 16):
        e = min(s + 16, slots)
        data.append(np.arange(idx, idx + 90 * (e - s)))
        idx += 90 * (e - s)
        if pilots and e < slots:
            pbl.append(np.arange(idx, idx + 36))
            idx += 36
    return idx, np.concatenate(data), pbl, ("short" if short else "normal"), s2.RATES[modcod - 1]


_rot_cache, _ldpc_cache = {}, {}
ROT_LUT = np.array([1, -1j, -1, 1j])                                  # j^(-R)


def rotation(n: int) -> np.ndarray:
    if n not in _rot_cache:
        _rot_cache[n] = s2.gold_rotation(0, n).astype(np.int64)
    return _rot_cache[n]


# --------------------------------------------------------------------------------------------- LDPC

class LdpcDecoder:
    """min-sum normalizzato a inondazione, vettorizzato; H dalle tabelle B/C (stessa struttura del codificatore)."""

    def __init__(self, frame: str, rate: str):
        nldpc = 64800 if frame == "normal" else 16200
        L = s2.Ldpc(frame, rate, nldpc)
        k, nk = L.k, L.n_k
        var = np.concatenate([L.info, k + np.arange(nk), k + np.arange(nk - 1)])
        chk = np.concatenate([L.addr, np.arange(nk), np.arange(1, nk)])
        o = np.argsort(chk, kind="stable")
        self.var, chk = var[o].astype(np.int64), chk[o]
        self.st = np.flatnonzero(np.r_[True, np.diff(chk) != 0])
        self.deg = np.diff(np.r_[self.st, len(self.var)])
        self.n, self.k = nldpc, k

    def decode(self, llr: np.ndarray, max_iter: int = 50, alpha: float = 0.78):
        llr = llr.astype(np.float32)
        c2v = np.zeros(len(self.var), np.float32)
        st, deg = self.st, self.deg
        for it in range(max_iter + 1):
            tot = llr + np.bincount(self.var, weights=c2v, minlength=self.n).astype(np.float32)
            hard = tot < 0
            if not (np.add.reduceat(hard[self.var].astype(np.int32), st) & 1).any():
                return hard.astype(np.uint8), it, True
            if it == max_iter:
                break
            v2c = tot[self.var] - c2v
            a = np.abs(v2c)
            neg = v2c < 0
            sgn = np.add.reduceat(neg.astype(np.int32), st) & 1
            m1 = np.minimum.reduceat(a, st)
            m1e = np.repeat(m1, deg)
            ismin = a == m1e
            m2 = np.minimum.reduceat(np.where(ismin, np.float32(np.inf), a), st)
            m2 = np.where(np.add.reduceat(ismin.astype(np.int32), st) > 1, m1, m2)
            mag = np.where(ismin, np.repeat(m2, deg), m1e)
            c2v = (alpha * np.where(np.repeat(sgn, deg).astype(bool) ^ neg, -mag, mag)).astype(np.float32)
        return hard.astype(np.uint8), max_iter, False


def ldpc_for(frame, rate):
    if (frame, rate) not in _ldpc_cache:
        _ldpc_cache[(frame, rate)] = LdpcDecoder(frame, rate)
    return _ldpc_cache[(frame, rate)]


# --------------------------------------------------------------------------------------------- DSP

def rrc_H(f, sr, beta=ROLLOFF):
    a = np.abs(f)
    f1, f2 = (1 - beta) * sr / 2, (1 + beta) * sr / 2
    h = np.zeros_like(a)
    h[a <= f1] = 1.0
    m = (a > f1) & (a < f2)
    h[m] = np.sqrt(0.5 * (1 + np.cos(np.pi / (beta * sr) * (a[m] - f1))))
    return h


def spectrum(x, fs, nfft=4096):
    nb = len(x) // nfft
    w = np.hanning(nfft)
    seg = x[:nb * nfft].reshape(nb, nfft) * w
    p = np.mean(np.abs(np.fft.fft(seg, axis=1)) ** 2, axis=0)
    return np.fft.fftshift(np.fft.fftfreq(nfft, 1 / fs)), np.fft.fftshift(p)


def detect(x, fs):
    """presenza del segnale, centro e larghezza a metà potenza (sopra il rumore) dallo spettro medio."""
    f, p = spectrum(x, fs)
    ps = np.convolve(p, np.ones(9) / 9, mode="same")
    floor = np.percentile(ps, 10)                                     # non la mediana: a 500 kS il segnale occupa 2/3 della banda
    pk = int(np.argmax(ps))
    snr_db = 10 * np.log10(ps[pk] / floor) if floor > 0 else 0.0
    if snr_db < 3.0:
        return None
    lvl = floor + (ps[pk] - floor) / 2
    lo = pk
    while lo > 0 and ps[lo - 1] > lvl:
        lo -= 1
    hi = pk
    while hi < len(ps) - 1 and ps[hi + 1] > lvl:
        hi += 1
    df = f[1] - f[0]
    # bordi interpolati tra i due bin a cavallo della soglia
    flo = f[lo] - df * (ps[lo] - lvl) / max(ps[lo] - ps[lo - 1], 1e-30) if lo > 0 else f[lo]
    fhi = f[hi] + df * (ps[hi] - lvl) / max(ps[hi] - ps[hi + 1], 1e-30) if hi < len(ps) - 1 else f[hi]
    return {"center": (flo + fhi) / 2, "width": fhi - flo, "peak_db": snr_db}


def snap_sr(w):
    best = min(SR_STD, key=lambda s: abs(s - w) / s)
    return best if abs(best - w) / best < 0.12 else round(w / 1000) * 1000


def matched(x, fs, fc, sr):
    """mescola a fc, ricampiona a 4*sr e filtra con l'RRC, tutto nel dominio della frequenza."""
    n = len(x)
    xm = x * np.exp(-2j * np.pi * fc * np.arange(n) / fs)
    X = np.fft.fft(xm)
    fs4 = 4 * sr
    n4 = int(round(n * fs4 / fs))
    Y = np.zeros(n4, np.complex128)
    h = min(n, n4) // 2
    Y[:h] = X[:h]
    Y[-h:] = X[-h:]
    Y *= rrc_H(np.fft.fftfreq(n4, 1 / fs4), sr)
    return np.fft.ifft(Y) * (n4 / n)


def fine_freq(r, fs4, max_hz):
    m = min(len(r), 1 << 20)
    z = r[:m] ** 4
    Z = np.abs(np.fft.fft(z, 1 << 21))
    f = np.fft.fftfreq(1 << 21, 1 / fs4)
    sel = np.abs(f) <= 4 * max_hz
    k = np.flatnonzero(sel)[np.argmax(Z[sel])]
    return f[k] / 4


def om_timing(r, block=1024):
    """Oerder-Meyr a blocchi su 4 campioni/simbolo: ritardo (in simboli) al centro di ogni blocco, svolto."""
    ns = len(r) // 4
    nb = ns // block
    e = np.abs(r[:nb * block * 4]) ** 2
    ph = np.exp(-0.5j * np.pi * np.arange(block * 4))
    v = (e.reshape(nb, block * 4) * ph).sum(axis=1)
    tau = np.unwrap(-np.angle(v)) / (2 * np.pi)
    centers = (np.arange(nb) + 0.5) * block
    return centers, tau


def smooth(v, w=7):
    if len(v) < w:
        return v
    k = np.ones(w) / w
    s = np.convolve(v, k, mode="same")
    c = np.convolve(np.ones_like(v), k, mode="same")
    return s / c


def interp_cubic(x, t):
    i = np.floor(t).astype(np.int64)
    mu = t - i
    i = np.clip(i, 1, len(x) - 3)
    xm1, x0, x1, x2 = x[i - 1], x[i], x[i + 1], x[i + 2]
    a = -0.5 * xm1 + 1.5 * x0 - 1.5 * x1 + 0.5 * x2
    b = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2
    c = -0.5 * xm1 + 0.5 * x1
    return ((a * mu + b) * mu + c) * mu + x0


def symbols(r, block=1024):
    centers, tau = om_timing(r, block)
    tau = smooth(tau)
    ns = len(r) // 4 - 2
    k = np.arange(ns)
    tk = np.interp(k, centers, tau)
    t = 4 * (k + tk)
    ok = (t >= 1) & (t < len(r) - 3)
    return interp_cubic(r, t[ok]), np.polyfit(centers, tau, 1)[0] if len(centers) > 2 else 0.0


# --------------------------------------------------------------------------------------------- frame

def sync_metric(y):
    D = np.empty_like(y)
    D[0] = 0
    D[1:] = y[1:] * np.conj(y[:-1])
    aD = np.abs(D)
    best, b7 = None, None
    for t, r in enumerate(DREFS):
        c = np.abs(np.correlate(D, r, "valid"))
        en = np.correlate(aD, np.abs(r), "valid") + 1e-12
        m = c / en
        if best is None:
            best, b7 = m, np.zeros(len(m), np.int8)
        else:
            sel = m > best
            best = np.where(sel, m, best)
            b7 = np.where(sel, 1, b7)
    return best


def decode_pls(y, m):
    """fase dal SOF, poi correlazione con le 128 parole del PLS code: (v, affidabilità 0..1, fase)."""
    if m < 0 or m + 90 > len(y):
        return None, 0.0, 0.0
    h = y[m:m + 90]
    ph = np.angle(np.sum(h[:26] * np.conj(SOF_A * U90[:26])))
    a = np.real(h[26:] * np.exp(-1j * ph) * np.conj(U90[26:]))
    c = PLS_A @ a
    v = int(np.argmax(c))
    return v, float(c[v] / (np.sum(np.abs(a)) + 1e-12)), ph


def find_headers(y, metric, thr=0.30, search=4):
    """catena dei PLHEADER: dal picco più alto avanti e indietro, con ricerca locale di +-search simboli."""
    m0 = int(np.argmax(metric))
    if metric[m0] < thr:
        return []
    v0, rel, _ = decode_pls(y, m0)
    L0 = frame_geom(v0)[0]
    if L0 is None:
        return [(m0, v0)]

    def refine(m):
        """header vicino a m: correlazione sufficiente, PLS code affidabile e coerente (CCM: stesso MODCOD, o DUMMY)."""
        lo, hi = max(0, m - search), min(len(metric), m + search + 1)
        if lo >= hi:
            return None, None
        k = lo + int(np.argmax(metric[lo:hi]))
        if metric[k] < thr * 0.8:
            return None, None
        v, rel, _ = decode_pls(y, k)
        if v is None or rel < 0.45 or (v != v0 and v >> 2 != 0):
            return None, None
        return k, v

    out = [(m0, v0)]
    m, v = m0, v0
    while True:                                                       # avanti
        L = frame_geom(v)[0]
        k, v = refine(m + L)
        if k is None:
            break
        out.append((k, v))
        m = k
    m = m0
    while True:                                                       # indietro (CCM: stessa lunghezza, o DUMMY)
        k = None
        for L in (L0, 3330):
            k, v = refine(m - L)
            if k is not None:
                break
        if k is None:
            break
        out.insert(0, (k, v))
        m = k
    return out


def frame_phase(y, m, v, geom, next_hdr):
    L, data, pbl, frame, rate = geom
    R = rotation(L - 90)
    pts, phs = [], []
    h = header_symbols(v)
    pts.append(45.0)
    phs.append(np.angle(np.sum(y[m:m + 90] * np.conj(h))))
    pil = (1 + 1j) * SQ2
    for b in pbl:
        known = pil * np.conj(ROT_LUT[R[b - 90]])                     # pilot dopo lo scrambling: p * j^R
        pts.append(float(b[0] + 17.5))
        phs.append(np.angle(np.sum(y[m + b] * np.conj(known))))
    if next_hdr is not None and next_hdr[0] + 90 <= len(y):
        k, vn = next_hdr
        pts.append(float(k - m + 45))
        phs.append(np.angle(np.sum(y[k:k + 90] * np.conj(header_symbols(vn)))))
    pts, phs = np.array(pts), np.unwrap(np.array(phs))
    if not pbl:                                                       # senza pilot: quarta potenza per slot, ancorata
        s = y[m + data] * ROT_LUT[R[data - 90]]
        slots = s.reshape(-1, 90)
        q4 = -np.sum(slots ** 4, axis=1)
        p4 = np.angle(q4) / 4
        rel = np.abs(q4) / (np.sum(np.abs(slots) ** 4, axis=1) + 1e-12)   # 1 = slot pulito, ~0 = disturbato
        Q = np.pi / 2

        def chain(start, order):
            out, c = np.empty(len(p4)), start
            for i in order:
                p = p4[i] + np.round((c - p4[i]) / Q) * Q
                out[i] = c = p
            return out

        sv = chain(phs[0], range(len(p4)))                            # in avanti dal PLHEADER
        sp = data[::90] + 45.0
        tail = phs[1:]
        if len(phs) > 1:
            # il PLHEADER successivo ha fase assoluta: se la catena in avanti arriva su un altro quadrante c'è stato un
            # salto di 90 gradi (tipicamente in uno slot colpito da un disturbo): dopo lo slot meno affidabile si usa la
            # catena all'indietro, che parte dal PLHEADER successivo
            nh = phs[-1]
            k = np.round((nh - sv[-1]) / Q)
            if int(k) % 4 != 0:
                sb = chain(nh, range(len(p4) - 1, -1, -1))
                cut = int(np.argmin(rel))
                sv = np.r_[sv[:cut], sb[cut:]]
            tail = phs[1:] + 2 * np.pi * np.round((sv[-1] - phs[1:]) / (2 * np.pi))
        allp = np.r_[pts[0], sp, pts[1:]]
        allv = np.r_[phs[0], sv, tail]
        o = np.argsort(allp)
        pts, phs = allp[o], allv[o]
    return np.interp(data.astype(np.float64), pts, phs)


_enc_ldpc = {}


def syndrome_weight(s, frame, rate):
    """equazioni di parità LDPC non soddisfatte dalle decisioni grezze sui simboli s (già derotati)."""
    if (frame, rate) not in _enc_ldpc:
        _enc_ldpc[(frame, rate)] = s2.Ldpc(frame, rate, 64800 if frame == "normal" else 16200)
    L = _enc_ldpc[(frame, rate)]
    hard = np.empty(2 * len(s), np.uint8)
    hard[0::2] = s.real < 0
    hard[1::2] = s.imag < 0
    bits, p = hard[:L.k], hard[L.k:]
    syn = (np.bincount(L.addr[bits[L.info] == 1], minlength=L.n_k) & 1).astype(np.uint8)
    syn ^= p
    syn[1:] ^= p[:-1]
    return int(syn.sum())


def fix_quadrants(s, frame, rate, thr=0.6):
    """senza pilot un disturbo può far saltare la fase di 90 gradi: dopo ogni tratto disturbato si provano le 4
    rotazioni del resto del frame e si tiene quella con meno equazioni di parità non soddisfatte."""
    sl = s.reshape(-1, 90)
    rel = np.abs(np.sum(sl ** 4, axis=1)) / (np.sum(np.abs(sl) ** 4, axis=1) + 1e-12)
    bad = np.flatnonzero(rel < thr)
    if not len(bad):
        return s
    cuts = [int(bad[0])] + [int(b) for a, b in zip(bad, bad[1:]) if b != a + 1]   # inizio di ogni tratto
    best = syndrome_weight(s, frame, rate)
    if best == 0:
        return s
    for c in cuts:
        k0 = c * 90
        trial = []
        for r in (1j, -1, -1j):
            t = s.copy()
            t[k0:] *= r
            trial.append((syndrome_weight(t, frame, rate), t))
        w, t = min(trial, key=lambda x: x[0])
        if w < best:
            best, s = w, t
    return s


def demod_frame(y, m, v, geom, next_hdr, ldpc_iter):
    L, data, pbl, frame, rate = geom
    if m + L > len(y):
        return None
    R = rotation(L - 90)
    ph = frame_phase(y, m, v, geom, next_hdr)
    s = y[m + data] * np.exp(-1j * ph) * ROT_LUT[R[data - 90]]
    if not pbl:
        s = fix_quadrants(s, frame, rate)
    # momenti M2/M4 (stima di SNR non distorta anche sotto soglia)
    m2 = np.mean(np.abs(s) ** 2)
    m4 = np.mean(np.abs(s) ** 4)
    S = np.sqrt(max(2 * m2 * m2 - m4, 1e-12 * m2 * m2))
    N = max(m2 - S, 1e-9 * m2)
    A = np.sqrt(S / 2)
    sigma2 = N / 2
    mer = 10 * np.log10(S / N)
    # rumore stimato tratto per tratto (finestra di 2 slot): i disturbi impulsivi, per esempio i beacon Wi-Fi ogni
    # 102,4 ms, abbassano l'affidabilità solo dei simboli colpiti e l'LDPC li ricostruisce dagli altri
    ideal = A * (np.sign(s.real) + 1j * np.sign(s.imag))
    e2 = np.abs(s - ideal) ** 2 / 2
    win = 180
    loc = np.convolve(e2, np.ones(win) / win, mode="same")
    sig_loc = np.maximum(loc, sigma2)                                  # mai più fiducia della media del frame
    llr = np.empty(2 * len(s), np.float32)
    llr[0::2] = 2 * A * s.real / sig_loc
    llr[1::2] = 2 * A * s.imag / sig_loc
    dec = ldpc_for(frame, rate)
    cw, it, ok = dec.decode(llr, ldpc_iter)
    return {"mer": mer, "iter": it, "ldpc_ok": ok, "bits": cw, "const": s / A}


def parse_bb(cw, frame, rate):
    e = _enc_cache(frame, rate)
    bb = cw[:e.kbch] ^ e.prbs
    hdr = np.packbits(bb[:80]).tobytes()
    ok = s2.crc8(hdr[:9]) == hdr[9]
    dfl = (hdr[4] << 8) | hdr[5]
    info = {"crc_ok": ok, "matype": hdr[0], "upl": (hdr[2] << 8) | hdr[3], "dfl": dfl,
            "sync": hdr[6], "syncd": (hdr[7] << 8) | hdr[8]}
    if ok and dfl % 8 == 0 and dfl <= e.kbch - 80:
        info["data"] = np.packbits(bb[80:80 + dfl]).tobytes()
    return info


_enc = {}


def _enc_cache(frame, rate):
    if (frame, rate) not in _enc:
        _enc[(frame, rate)] = s2.Encoder(rate, frame, True)
    return _enc[(frame, rate)]


# --------------------------------------------------------------------------------------------- TS

class TsAssembler:
    """ricostruisce i pacchetti TS; flags[i] = True (CRC-8 buono), False (errato), None (non verificabile: primo pacchetto
    dopo un buco)."""

    def __init__(self):
        self.buf = bytearray()
        self.aligned = False
        self.prev = None
        self.packets, self.flags, self.crc_ok, self.crc_bad = [], [], 0, 0

    def push(self, bb):
        if bb is None or "data" not in bb:
            self.buf.clear()
            self.aligned = False
            self.prev = None
            return
        d = bb["data"]
        if not self.aligned:
            if bb["syncd"] == 65535 or bb["syncd"] % 8:
                return
            self.buf = bytearray(d[bb["syncd"] // 8:])
            self.aligned = True
        else:
            self.buf += d
        while len(self.buf) >= 188:
            u = bytes(self.buf[:188])
            del self.buf[:188]
            flag = None
            if self.prev is not None:
                flag = s2.crc8(self.prev[1:]) == u[0]
                if flag:
                    self.crc_ok += 1
                else:
                    self.crc_bad += 1
            self.prev = u
            self.packets.append(b"\x47" + u[1:])
            # il CRC nel pacchetto i protegge il pacchetto i-1: si assegna l'esito al precedente
            if flag is not None and self.flags:
                self.flags[-1] = flag
            self.flags.append(None)


def _dvb_str(b):
    if b and b[0] < 0x20:
        b = b[1:]
    return b.decode("latin-1", "replace").strip()


def parse_ts(packets):
    sections = {}
    info = {"services": [], "streams": [], "pmt_pid": None}
    for p in packets:
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid not in (0x0000, 0x0011) and pid != info["pmt_pid"]:
            continue
        afc = (p[3] >> 4) & 3
        off = 4
        if afc & 2:
            off += 1 + p[4]
        if not afc & 1 or off >= 188:
            continue
        pay = p[off:]
        if p[1] & 0x40:
            sections[pid] = bytearray(pay[1 + pay[0]:])
        elif pid in sections:
            sections[pid] += pay
        else:
            continue
        s = sections[pid]
        if len(s) < 3:
            continue
        slen = ((s[1] & 0x0F) << 8) | s[2]
        if len(s) < 3 + slen:
            continue
        sec = bytes(s[:3 + slen])
        del sections[pid]
        tid = sec[0]
        if tid == 0x00 and info["pmt_pid"] is None:
            for i in range(8, 3 + slen - 4, 4):
                prog = (sec[i] << 8) | sec[i + 1]
                if prog:
                    info["pmt_pid"] = ((sec[i + 2] & 0x1F) << 8) | sec[i + 3]
                    break
        elif tid == 0x02 and not info["streams"]:
            pil = ((sec[10] & 0x0F) << 8) | sec[11]
            i = 12 + pil
            while i + 5 <= 3 + slen - 4:
                st = sec[i]
                epid = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2]
                el = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
                info["streams"].append({"type": st, "codec": STREAM_TYPES.get(st, hex(st)), "pid": epid})
                i += 5 + el
        elif tid == 0x42 and not info["services"]:
            i = 11
            while i + 5 <= 3 + slen - 4:
                dl = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
                j, end = i + 5, i + 5 + dl
                while j + 2 <= end:
                    tag, ln = sec[j], sec[j + 1]
                    if tag == 0x48 and ln >= 3:
                        d = sec[j + 2:j + 2 + ln]
                        pl = d[1]
                        prov = _dvb_str(d[2:2 + pl])
                        nl = d[2 + pl]
                        name = _dvb_str(d[3 + pl:3 + pl + nl])
                        info["services"].append({"service_type": d[0], "provider": prov, "name": name})
                    j += 2 + ln
                i = end
    return info


def _irap(pes: bytes, st: int):
    """se il PES contiene un'immagine chiave (H.264 IDR, H.265 IRAP) restituisce il flusso elementare dell'immagine."""
    if len(pes) < 9 or pes[:3] != b"\x00\x00\x01":
        return None
    es = bytes(pes[9 + pes[8]:])
    i = es.find(b"\x00\x00\x01")
    while 0 <= i < len(es) - 3:
        h = es[i + 3]
        if (st == 0x1B and (h & 0x1F) == 5) or (st == 0x24 and 16 <= ((h >> 1) & 0x3F) <= 21):
            return {"codec": "H.264" if st == 0x1B else "H.265", "es": es}
        i = es.find(b"\x00\x00\x01", i + 3)
    return None


def video_keyframe(packets, flags, info):
    """prima immagine chiave del video con TUTTI i pacchetti del suo PES a CRC buono e contatori di continuità in ordine;
    None se nella cattura non ce n'è una integra (il PiP userà la costellazione)."""
    vs = [s for s in info["streams"] if s["type"] in (0x1B, 0x24)]
    if not vs:
        return None
    vpid, st = vs[0]["pid"], vs[0]["type"]
    pes, good, cc_prev = None, True, None
    for p, f in zip(packets, flags):
        if (((p[1] & 0x1F) << 8) | p[2]) != vpid:
            continue
        afc, cc = (p[3] >> 4) & 3, p[3] & 0x0F
        off = 4 + (1 + p[4] if afc & 2 else 0)
        if p[1] & 0x40:                                               # inizio di un nuovo PES: chiude il precedente
            if pes is not None and good:
                r = _irap(pes, st)
                if r:
                    return r
            pes, good = bytearray(), True
        elif pes is None:
            continue                                                  # cattura iniziata a metà di un PES
        if f is not True:
            good = False
        if afc & 1:
            if cc_prev is not None and cc != (cc_prev + 1) & 0x0F:
                good = False
            cc_prev = cc
            if off < 188:
                pes += p[off:]
    return None


def best_capture(results):
    """tra più catture: prima quelle con QRZ letto, poi senza errori CRC, poi con immagine chiave, poi MER più alto."""
    def key(r):
        rep = r[0]
        return (rep.get("call") is not None, rep.get("ts_crc_bad", 1) == 0, rep.get("keyframe") is not None,
                rep.get("mer_db") or -99)
    return max(range(len(results)), key=lambda i: key(results[i]))


def pip_png(kf, const, path, size=(160, 90)):
    """PiP del segnale ricevuto: fotogramma chiave H.264 se integro (sull'S3: esp_h264), altrimenti la costellazione
    (per l'H.265 l'S3 non ha un decodificatore). Sul PC il fotogramma si decodifica con ffmpeg."""
    import shutil
    import subprocess
    import tempfile
    if kf and kf["codec"] == "H.264" and shutil.which("ffmpeg"):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "kf.h264"
            src.write_bytes(kf["es"])
            r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src), "-frames:v", "1",
                                "-vf", f"scale={size[0]}:{size[1]}", str(path)], capture_output=True)
            if r.returncode == 0 and Path(path).exists():
                return "fotogramma H.264"
    if const is not None:
        const_png(const, path, size[1])
        return "costellazione"
    return None


def find_call(info):
    for svc in info["services"]:
        for field in ("name", "provider"):
            m = CALL_RE.search(svc[field].upper())
            if m:
                return m.group(0)
    return None


# --------------------------------------------------------------------------------------------- catena

def pick_sr(x, fs, det, probe_s=0.25):
    """symbol rate: se la larghezza a -3 dB è vicina (5%) a un valore standard si usa quello; altrimenti (calo del
    filtro del ricevitore, segnale reale) si provano gli SR standard entro +-35% e si tiene quello con l'aggancio dei
    PLHEADER più forte su un tratto breve della cattura."""
    w = det["width"]
    near = min(SR_STD, key=lambda s: abs(s - w) / s)
    if abs(near - w) / near < 0.05:
        return near
    cands = [s for s in SR_STD if 0.65 * w <= s <= 1.35 * w and 2 * s <= fs] or [snap_sr(w)]
    seg = x[: int(probe_s * fs)]
    best, best_m = cands[0], -1.0
    for s in cands:
        r = matched(seg, fs, det["center"], s)
        d = fine_freq(r, 4 * s, max_hz=0.03 * s)
        r *= np.exp(-2j * np.pi * d * np.arange(len(r)) / (4 * s))
        y, _ = symbols(r)
        if len(y) < 200:
            continue
        y = y / np.sqrt(np.mean(np.abs(y) ** 2))
        m = float(np.max(sync_metric(y)))
        if m > best_m:
            best, best_m = s, m
    return best


def receive(x, fs, sr_hint=None, max_frames=None, ldpc_iter=50, log=print):
    t0 = time.time()
    rep = {"detected": False}
    det = detect(x, fs)
    if det is None:
        log("nessun segnale")
        return rep, [], None
    sr = sr_hint or pick_sr(x, fs, det)
    rep.update(detected=True, peak_db=round(det["peak_db"], 1), width_hz=round(det["width"]), sr_coarse=sr,
               fc_coarse=round(det["center"]))
    log(f"segnale: centro {det['center']:+.0f} Hz, larghezza -3 dB {det['width']:.0f} Hz -> SR {sr}, "
        f"picco {det['peak_db']:.1f} dB sul rumore")
    fc = det["center"]
    for _ in range(2):                                                # secondo passo con l'SR misurato dal timing
        r = matched(x, fs, fc, sr)
        dfine = fine_freq(r, 4 * sr, max_hz=0.03 * sr)
        r *= np.exp(-2j * np.pi * dfine * np.arange(len(r)) / (4 * sr))
        y, slope = symbols(r)
        if abs(slope) < 30e-6:
            break
        sr = sr / (1 + slope)
        fc += dfine
    foff = fc + dfine
    sr_meas = sr / (1 + slope)
    rep.update(freq_offset_hz=round(foff, 1), sr=round(sr_meas))
    log(f"frequenza {foff:+.1f} Hz, SR misurato {sr_meas:.0f} S/s ({len(y)} simboli)")
    y = y / np.sqrt(np.mean(np.abs(y) ** 2))
    metric = sync_metric(y)
    hdrs = find_headers(y, metric)
    if len(hdrs) < 3:                                                 # meno di 3 PLHEADER concatenati: non è DVB-S2
        log("nessun PLHEADER" if not hdrs else f"solo {len(hdrs)} PLHEADER: segnale non riconosciuto")
        rep["detected"] = False
        return rep, [], None
    v0 = hdrs[len(hdrs) // 2][1]
    geom0 = frame_geom(v0)
    rep.update(plframes=len(hdrs), modcod=v0 >> 2, frame=geom0[3], fec=geom0[4], pilots=bool(v0 & 1))
    log(f"{len(hdrs)} PLHEADER, MODCOD {v0 >> 2} = QPSK {geom0[4]} {geom0[3]} pilot {'si' if v0 & 1 else 'no'}")
    asm = TsAssembler()
    mers, iters, okf, bbok, consts = [], [], 0, 0, []
    done = 0
    for n, (m, v) in enumerate(hdrs):
        geom = frame_geom(v)
        if geom[1] is None:                                           # DUMMY o non gestito
            asm.push(None)
            continue
        nxt = hdrs[n + 1] if n + 1 < len(hdrs) else None
        res = demod_frame(y, m, v, geom, nxt, ldpc_iter)
        if res is None:
            break
        mers.append(res["mer"])
        iters.append(res["iter"])
        okf += res["ldpc_ok"]
        bb = parse_bb(res["bits"], geom[3], geom[4])
        bbok += bb["crc_ok"]
        asm.push(bb if bb["crc_ok"] else None)
        if len(consts) < 2:
            consts.append(res["const"])
        done += 1
        if max_frames and done >= max_frames:
            break
    ts = asm.packets
    tsi = parse_ts(ts)
    call = find_call(tsi)
    kf = video_keyframe(ts, asm.flags, tsi)
    rep["keyframe"] = kf["codec"] if kf else None
    rep["_kf"] = kf
    mer = float(np.median(mers)) if mers else None
    rep.update(frames_demod=done, ldpc_ok=okf, bbheader_ok=bbok, ldpc_iter_mean=round(float(np.mean(iters)), 1) if iters else None,
               mer_db=round(mer, 1) if mer is not None else None,
               d_margin_db=round(mer - THRESH[geom0[4]], 1) if mer is not None and geom0[4] else None,
               ts_packets=len(ts), ts_crc_ok=asm.crc_ok, ts_crc_bad=asm.crc_bad,
               services=tsi["services"], streams=tsi["streams"], call=call, seconds=round(time.time() - t0, 1))
    rep["qrk"] = qrk_text(rep)
    log(f"MER {rep['mer_db']} dB, D {rep['d_margin_db']}, LDPC ok {okf}/{done} (iterazioni medie "
        f"{rep['ldpc_iter_mean']}), BBHEADER ok {bbok}, TS {len(ts)} pacchetti (CRC ok {asm.crc_ok}, errati {asm.crc_bad})")
    log(f"servizi {tsi['services']}  flussi {[s['codec'] for s in tsi['streams']]}  QRZ {call}")
    log(f"QRK: {rep['qrk']}   ({rep['seconds']} s)")
    return rep, ts, (np.concatenate(consts) if consts else None)


def qrk_text(r):
    if not r.get("call") and not r.get("mer_db"):
        return "-"
    sr = r.get("sr", 0)
    srt = f"{sr / 1000:.0f}kS" if sr else ""
    return (f"{r.get('call') or '?'}  MER {r.get('mer_db')} dB  D{r.get('d_margin_db')}  QPSK {r.get('fec')} {srt}  "
            f"{r.get('freq_offset_hz', 0) / 1000:+.1f} kHz")


def const_png(c, path, size=240):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (size, size), (8, 12, 20))
    d = ImageDraw.Draw(img)
    for g in (size // 4, size // 2, 3 * size // 4):
        d.line([(g, 0), (g, size)], fill=(40, 50, 70))
        d.line([(0, g), (size, g)], fill=(40, 50, 70))
    c = c[: 20000]
    px = np.clip(((c.real / 3.2) + 0.5) * size, 0, size - 1).astype(int)
    py = np.clip(((-c.imag / 3.2) + 0.5) * size, 0, size - 1).astype(int)
    arr = np.array(img)
    arr[py, px] = (255, 200, 40)
    Image.fromarray(arr).save(path)


def load_cs8(path):
    q = np.fromfile(path, np.int8).astype(np.float32)
    return (q[0::2] + 1j * q[1::2]).astype(np.complex128)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("capture")
    p.add_argument("--fs", type=float, default=1e6)
    p.add_argument("--sr", type=int, help="symbol rate noto (altrimenti stimato)")
    p.add_argument("--max-frames", type=int)
    p.add_argument("--iter", type=int, default=50)
    p.add_argument("--ts", help="salva il TS ricevuto")
    p.add_argument("--png", help="salva la costellazione (PNG 240x240)")
    p.add_argument("--json", help="salva il rapporto")
    a = p.parse_args()
    x = load_cs8(a.capture)
    rep, ts, const = receive(x, a.fs, a.sr, a.max_frames, a.iter)
    if a.ts and ts:
        Path(a.ts).write_bytes(b"".join(ts))
    if a.png and const is not None:
        const_png(const, a.png)
    if a.json:
        Path(a.json).write_text(json.dumps({k: v for k, v in rep.items() if not k.startswith("_")}, indent=1, default=str),
                                encoding="utf-8")


if __name__ == "__main__":
    main()
