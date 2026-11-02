"""ESP32-S3_PDX Parrot (IZ7PDX, GPL-3.0) - P0b: catture realistiche e scelta del materiale per il PiP.

Casi: cattura che parte a metà frame e a metà GOP, GOP lungo senza immagine chiave nella cattura, raffiche di disturbo
che rovinano alcuni frame, H.265 (sull'S3 niente decodificatore: costellazione), scelta della migliore tra 3 catture.
Ogni immagine chiave estratta viene confrontata con quelle del TS trasmesso (deve essere identica byte per byte).
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import s2_rx  # noqa: E402

PY = sys.executable
OUT = HERE / "out"


def synth(name, **kw):
    cap, src = OUT / f"p0b_{name}.cs8", OUT / f"p0b_{name}_src.ts"
    args = [PY, str(HERE / "s2_synth.py"), "-o", str(cap), "--keep-ts", str(src)]
    for k, v in kw.items():
        if k == "burst":
            for b in v:
                args += ["--burst", b]
        else:
            args += [f"--{k}", str(v)]
    subprocess.run(args, check=True, capture_output=True)
    return cap, src


def source_keyframes(src: Path):
    """flussi elementari di tutte le immagini chiave del TS trasmesso, per il confronto byte per byte."""
    pk = src.read_bytes()
    packets = [pk[i:i + 188] for i in range(0, len(pk), 188)]
    info = s2_rx.parse_ts(packets)
    vs = [s for s in info["streams"] if s["type"] in (0x1B, 0x24)][0]
    out, pes = [], None
    end = bytes([0x47, 0x40 | (vs["pid"] >> 8), vs["pid"] & 0xFF, 0x10]) + bytes(184)   # chiude l'ultimo PES
    for p in packets + [end]:
        if (((p[1] & 0x1F) << 8) | p[2]) != vs["pid"]:
            continue
        afc = (p[3] >> 4) & 3
        off = 4 + (1 + p[4] if afc & 2 else 0)
        if p[1] & 0x40:
            if pes is not None:
                r = s2_rx._irap(bytes(pes), vs["type"])
                if r:
                    out.append(r["es"])
            pes = bytearray()
        if pes is not None and afc & 1 and off < 188:
            pes += p[off:]
    return out

def run(name, label, expect_kf, expect_call=True, **kw):
    cap, src = synth(name, **kw)
    rep, ts, const = s2_rx.receive(s2_rx.load_cs8(cap), 1e6, log=lambda *_: None)
    kf = rep.pop("_kf")
    srckf = source_keyframes(src)
    identical = kf is not None and any(kf["es"] == s for s in srckf)
    pip = s2_rx.pip_png(kf, const, OUT / f"p0b_{name}_pip.png")
    call_ok = rep.get("call") == "IZ7XYZ" if expect_call else rep.get("call") in (None, "IZ7XYZ")
    kf_ok = (expect_kf is None or (kf is not None) == expect_kf) and (kf is None or identical)
    ok = call_ok and kf_ok
    print(f"{label:46s} QRZ={rep.get('call')} MER={rep.get('mer_db')} CRC errati={rep.get('ts_crc_bad')} "
          f"chiave={rep.get('keyframe')} identica={identical if kf else '-'} PiP={pip} -> {'OK' if ok else 'ERRORE'}",
          flush=True)
    return {"case": label, "ok": ok, "pip": pip, "identical": identical, "rx": rep}


def main():
    OUT.mkdir(exist_ok=True)
    res = [
        run("midgop", "H.264, cattura a metà frame e a metà GOP", True,
            esn0=9, skip=0.43, seconds=2.0, seed=11),
        run("longgop", "H.264, GOP di 3 s, cattura di 1,2 s", False,
            esn0=9, skip=0.5, seconds=1.2, keyint=75, seed=12),
        run("bursts", "Caso peggiore: raffiche +6 dB ogni 0,25 s (40% frame)", None, expect_call=False,
            esn0=9, skip=0.2, seconds=2.5, seed=13,
            burst=[f"{0.1 + 0.25 * k:.2f}:0.03:6" for k in range(10)]),
        run("rbursts", "H.264, 5 raffiche casuali +6 dB", True,
            esn0=9, skip=0.31, seconds=2.5, seed=15,
            burst=["0.17:0.03:6", "0.71:0.02:6", "1.05:0.04:6", "1.66:0.03:6", "2.21:0.02:6"]),
        run("h265", "H.265, cattura a metà GOP", True,
            esn0=9, skip=0.37, seconds=2.0, codec="h265", seed=14),
    ]
    # migliore tra 3 catture dello stesso trasmettitore con condizioni diverse
    caps = []
    for k, (es, sk) in enumerate([(5.0, 0.1), (10.0, 0.7), (7.0, 1.3)]):
        cap, _ = synth(f"best{k}", esn0=es, skip=sk, seconds=1.6, seed=20 + k)
        r = s2_rx.receive(s2_rx.load_cs8(cap), 1e6, log=lambda *_: None)
        caps.append(r)
    b = s2_rx.best_capture(caps)
    mers = [c[0].get("mer_db") for c in caps]
    ok = b == 1
    print(f"{'Migliore tra 3 catture (Es/N0 5, 10, 7 dB)':46s} MER={mers} scelta={b + 1} -> {'OK' if ok else 'ERRORE'}")
    res.append({"case": "best of 3", "ok": ok, "chosen": b, "mer": mers})
    # due catture successive: la prima perde le SDT nelle raffiche, la seconda no -> si sceglie la seconda
    two = []
    for k, bursts in enumerate([[f"{0.1 + 0.25 * j:.2f}:0.03:6" for j in range(10)], []]):
        cap, _ = synth(f"two{k}", esn0=9, skip=0.2 + 0.9 * k, seconds=2.5, seed=13 + 30 * k, burst=bursts)
        two.append(s2_rx.receive(s2_rx.load_cs8(cap), 1e6, log=lambda *_: None))
    b2 = s2_rx.best_capture(two)
    calls = [c[0].get("call") for c in two]
    ok2 = b2 == 1 and calls[1] == "IZ7XYZ"
    print(f"{'Seconda cattura dopo una senza QRZ':46s} QRZ={calls} scelta={b2 + 1} -> {'OK' if ok2 else 'ERRORE'}")
    res.append({"case": "second capture", "ok": ok2, "chosen": b2, "calls": calls})
    (OUT / "p0b_results.json").write_text(json.dumps(res, indent=1, default=str), encoding="utf-8")
    print(f"\n{sum(r['ok'] for r in res)}/{len(res)} casi riusciti")


if __name__ == "__main__":
    main()
