"""ESP32-S3-CAM_PDX DATV Beacon (IZ7PDX, GPL-3.0) - Parrot P0: banco di prova del ricevitore su catture sintetiche.

Per ogni caso: genera la cattura (s2_synth), la riceve (s2_rx), confronta il TS ricevuto con quello sorgente pacchetto
per pacchetto e scrive la tabella dei risultati (out/p0_results.md + .json).
"""
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import s2_rx  # noqa: E402

PY = sys.executable
OUT = HERE / "out"

CASES = [
    ("333k 3/4 Es/N0 10", dict(sr=333000, fec="3/4", esn0=10, df=20000, ppm=10)),
    ("333k 3/4 Es/N0 6", dict(sr=333000, fec="3/4", esn0=6, df=-12000, ppm=-8)),
    ("333k 3/4 Es/N0 5 (D~1)", dict(sr=333000, fec="3/4", esn0=5, df=5000, ppm=20)),
    ("333k 3/4 Es/N0 4.3 (soglia)", dict(sr=333000, fec="3/4", esn0=4.3, df=5000, ppm=5)),
    ("333k 1/2 Es/N0 2", dict(sr=333000, fec="1/2", esn0=2, df=30000, ppm=10)),
    ("250k 2/3 -35 kHz -15 ppm", dict(sr=250000, fec="2/3", esn0=8, df=-35000, ppm=-15)),
    ("125k 3/4 +45 kHz", dict(sr=125000, fec="3/4", esn0=9, df=45000, ppm=10, seconds=4)),
    ("500k 3/4", dict(sr=500000, fec="3/4", esn0=9, df=10000, ppm=10)),
    ("333k 3/4 frame corti", dict(sr=333000, fec="3/4", esn0=8, df=15000, ppm=10, short=True)),
    ("333k 3/4 senza pilot", dict(sr=333000, fec="3/4", esn0=8, df=15000, ppm=10, nopilots=True)),
    ("333k 3/4 H.265", dict(sr=333000, fec="3/4", esn0=8, df=15000, ppm=10, codec="h265")),
    ("333k 3/4 rumore di fase", dict(sr=333000, fec="3/4", esn0=8, df=15000, ppm=10, pn=0.05)),
    ("333k 9/10 Es/N0 9", dict(sr=333000, fec="9/10", esn0=9, df=-20000, ppm=10)),
    ("solo rumore", dict(sr=333000, fec="3/4", esn0=-40, df=0, ppm=0)),
]


def run_case(n, name, kw):
    cap = OUT / f"p0_{n:02d}.cs8"
    src = OUT / f"p0_{n:02d}_src.ts"
    args = [PY, str(HERE / "s2_synth.py"), "-o", str(cap), "--keep-ts", str(src), "--seed", str(n + 1),
            "--seconds", str(kw.get("seconds", 2.0))]
    for k in ("sr", "fec", "esn0", "df", "ppm", "pn"):
        if k in kw:
            args += [f"--{k}", str(kw[k])]
    if kw.get("codec"):
        args += ["--codec", kw["codec"]]
    if kw.get("short"):
        args.append("--short")
    if kw.get("nopilots"):
        args.append("--nopilots")
    subprocess.run(args, check=True, capture_output=True)
    x = s2_rx.load_cs8(cap)
    t = time.time()
    rep, ts, const = s2_rx.receive(x, 1e6, log=lambda *_: None)
    dt = time.time() - t
    sp = Path(src).read_bytes()
    spk = {sp[i:i + 188] for i in range(0, len(sp), 188)}
    same = sum(1 for p in ts if p in spk)
    if const is not None:
        s2_rx.const_png(const, OUT / f"p0_{n:02d}_const.png")
    return {"case": name, **{k: kw.get(k) for k in ("sr", "fec", "esn0", "df", "ppm")}, "rx": rep,
            "ts_identical": same, "ts_total": len(ts), "rx_seconds": round(dt, 1)}


def main():
    OUT.mkdir(exist_ok=True)
    res = []
    for n, (name, kw) in enumerate(CASES):
        r = run_case(n, name, kw)
        res.append(r)
        x = r["rx"]
        print(f"{name:30s} rilevato={x.get('detected')} QRZ={x.get('call')} MER={x.get('mer_db')} D={x.get('d_margin_db')} "
              f"df={x.get('freq_offset_hz')} SR={x.get('sr')} LDPC={x.get('ldpc_ok')}/{x.get('frames_demod')} "
              f"it={x.get('ldpc_iter_mean')} TS={r['ts_identical']}/{r['ts_total']} crc_bad={x.get('ts_crc_bad')} "
              f"{r['rx_seconds']} s", flush=True)
    (OUT / "p0_results.json").write_text(json.dumps(res, indent=1, default=str), encoding="utf-8")
    lines = ["| Caso | Es/N0 gen. | QRZ letto | MER mis. | D | scarto mis. (gen.) | SR mis. | LDPC ok | TS identici | tempo PC |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for r in res:
        x = r["rx"]
        lines.append(f"| {r['case']} | {r['esn0']} dB | {x.get('call') or '—'} | {x.get('mer_db') or '—'} | "
                     f"{x.get('d_margin_db') if x.get('d_margin_db') is not None else '—'} | "
                     f"{x.get('freq_offset_hz', '—')} Hz ({r['df']}) | {x.get('sr', '—')} | "
                     f"{x.get('ldpc_ok', 0)}/{x.get('frames_demod', 0)} | {r['ts_identical']}/{r['ts_total']} | {r['rx_seconds']} s |")
    (OUT / "p0_results.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
