"""Beacon DATV IZ7PDX - tabelle DVB-S2 estratte dalla norma ETSI EN 302 307-1 V1.4.1.

Legge il testo della norma (estratto dal PDF con pypdf in ref/etsi/en302307.txt), ricostruisce le tabelle degli indirizzi
degli accumulatori di parità LDPC (annessi B e C) e le salva in ldpc_tables.json. Ogni tabella è verificata:
  1. numero di righe = k_ldpc / 360;
  2. le righe hanno due lunghezze (le prime "lunghe", in sequenza continua, poi 3, ognuna a inizio riga del testo);
  3. tutti gli indirizzi sono < n_ldpc - k_ldpc;
  4. solo frame normali: ogni classe di resto modulo q riceve lo stesso numero di indirizzi (grado dei nodi di controllo
     costante); alcuni codici corti non sono regolari e per loro vale il confronto con un codificatore indipendente.

Uso: python etsi_tables.py ../../ref/etsi/en302307.txt
"""
import json
import re
import sys
from pathlib import Path

# (rate, K_bch, N_bch = k_ldpc, t) dalle tabelle 5a e 5b della norma
NORMAL = {
    "1/4": (16008, 16200, 12), "1/3": (21408, 21600, 12), "2/5": (25728, 25920, 12), "1/2": (32208, 32400, 12),
    "3/5": (38688, 38880, 12), "2/3": (43040, 43200, 10), "3/4": (48408, 48600, 12), "4/5": (51648, 51840, 12),
    "5/6": (53840, 54000, 10), "8/9": (57472, 57600, 8), "9/10": (58192, 58320, 8),
}
SHORT = {
    "1/4": (3072, 3240, 12), "1/3": (5232, 5400, 12), "2/5": (6312, 6480, 12), "1/2": (7032, 7200, 12),
    "3/5": (9552, 9720, 12), "2/3": (10632, 10800, 12), "3/4": (11712, 11880, 12), "4/5": (12432, 12600, 12),
    "5/6": (13152, 13320, 12), "8/9": (14232, 14400, 12),
}
RATES_B = ["1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"]
RATES_C = RATES_B[:-1]

TITLE = re.compile(r"^Table ([BC])\.(\d+): Rate (\d+/\d+) \(nldpc = (64 800|16 200)\)")
NOISE = re.compile(r"^(ETSI|=====PAGE|\s*$)")


def fmt(n: int) -> str:
    """numero con lo spazio delle migliaia, come nella norma (48408 -> '48 408')."""
    s = str(n)
    return s if n < 1000 else f"{s[:-3]} {s[-3:]}"


def check_params(text: str) -> None:
    """i parametri scritti qui sopra devono comparire nelle tabelle 5a/5b del testo."""
    for table, frame in ((NORMAL, "5a"), (SHORT, "5b")):
        for rate, (kb, nb, t) in table.items():
            pat = re.compile(rf"^{re.escape(rate)}\s+{fmt(kb)}\s+{fmt(nb)}\s+{t}\b", re.M)
            if not pat.search(text):
                raise SystemExit(f"tabella {frame}: parametri di {rate} ({kb}, {nb}, t={t}) non trovati nel testo")


# numeri spezzati dall'estrazione del testo, verificati sull'immagine della pagina del PDF:
# (annesso, rate): [(righe del testo estratte, riga corretta)]
FIXES = {
    ("B", "5/6"): [([[29, 734], [7, 8027]], [29, 7347, 8027])],   # pagina 48, ultima riga della tabella B.9
    ("B", "8/9"): [([[10, 534], [8, 5619]], [10, 5348, 5619])],   # pagina 49, ultima riga della tabella B.10
    ("B", "9/10"): [([[4, 515], [5, 3858]], [4, 5155, 3858])],    # pagina 50, terza colonna della tabella B.11
}


def apply_fixes(out):
    for key, fixes in FIXES.items():
        rows = out.get(key, [])
        for bad, good in fixes:
            n = len(bad)
            hits = [i for i in range(len(rows) - n + 1) if rows[i:i + n] == bad]
            if len(hits) != 1:
                raise SystemExit(f"correzione {key} {bad}: trovata {len(hits)} volte (attesa 1)")
            rows[hits[0]:hits[0] + n] = [good]
    return out


def collect(lines):
    """righe numeriche di ogni tabella dell'annesso B e C: {(annex, rate): [[numeri], ...]}."""
    out, cur = {}, None
    for line in lines:
        m = TITLE.match(line.strip())
        if m:
            cur = (m.group(1), m.group(3))
            if cur in out:
                raise SystemExit(f"tabella {cur} ripetuta")
            out[cur] = []
            continue
        if line.startswith("Annex") and cur and cur[0] == "C" and cur[1] == "8/9":
            cur = None
        if cur is None or NOISE.match(line):
            continue
        toks = line.split()
        if not toks or not all(t.isdigit() for t in toks):
            if out[cur]:            # fine della tabella (testo dopo i numeri)
                cur = None
            continue
        out[cur].append([int(t) for t in toks])
    return out


def rebuild(text_lines, rows, long_len):
    """ricostruisce rows righe: le prime r1 da long_len numeri, poi da 3. Nel PDF i numeri delle righe lunghe scorrono come
    un unico testo (gli a capo non coincidono con la fine delle righe, vedi tabella B.2 a pagina 41), quindi la sequenza si
    divide in ordine. Le righe da 3 invece stanno una per riga di testo: lì ogni riga deve cominciare a inizio riga.
    None se la divisione non è coerente."""
    n = sum(len(tl) for tl in text_lines)
    if long_len == 3:                      # tabella con sole righe da 3 (es. C.8, rate 4/5 corto)
        if n != 3 * rows:
            return None
        r1 = 0
    else:
        r1, rem = divmod(n - 3 * rows, long_len - 3)
        if rem or r1 < 1 or r1 > rows:
            return None
    flat, starts = [], set()
    for tl in text_lines:
        starts.add(len(flat))
        flat += tl
    out, p = [], 0
    for i in range(rows):
        L = long_len if i < r1 else 3
        if i >= r1 and p not in starts:
            return None
        out.append(flat[p:p + L])
        p += L
    return out if p == n else None


def verify(rows, n_k, q, balanced):
    for r in rows:
        for x in r:
            if x >= n_k:
                return f"indirizzo {x} >= {n_k}"
    counts = [0] * q
    for r in rows:
        for x in r:
            counts[x % q] += 1
    if balanced and len(set(counts)) != 1:
        return f"classi di resto non bilanciate: min {min(counts)} max {max(counts)}"
    return None


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "../../ref/etsi/en302307.txt")
    text = src.read_text(encoding="utf-8")
    check_params(text)
    raw = apply_fixes(collect(text.splitlines()))
    result = {"source": "ETSI EN 302 307-1 V1.4.1 (2014-11), annex B and C", "normal": {}, "short": {}}
    for annex, rates, frame, params, nldpc in (("B", RATES_B, "normal", NORMAL, 64800), ("C", RATES_C, "short", SHORT, 16200)):
        for rate in rates:
            lines = raw.get((annex, rate))
            if not lines:
                raise SystemExit(f"tabella {annex} {rate} non trovata")
            kldpc = params[rate][1]
            n_k = nldpc - kldpc
            q = n_k // 360
            nrows = kldpc // 360
            # la riga lunga è la riga di testo più lunga della tabella; solo se non basta si provano tutte le lunghezze
            sols = []
            for L in [max(len(tl) for tl in lines)] + list(range(3, 20)):
                rows = rebuild(lines, nrows, L)
                if rows is not None and verify(rows, n_k, q, frame == "normal") is None:
                    sols.append((L, rows))
                if len(sols) == 1 and L == max(len(tl) for tl in lines):
                    break
            if len(sols) != 1:
                raise SystemExit(f"tabella {annex} {rate}: {len(sols)} ricostruzioni valide (attesa 1)")
            L, rows = sols[0]
            r1 = sum(1 for r in rows if len(r) == L)
            counts = [0] * q
            for r in rows:
                for x in r:
                    counts[x % q] += 1
            print(f"{frame:6s} {rate:5s} righe {nrows:3d} ({r1} da {L}, {nrows - r1} da 3)  q {q:3d}  "
                  f"indirizzi per classe {min(counts)}..{max(counts)}  OK")
            kb, nb, t = params[rate]
            result[frame][rate] = {"kbch": kb, "nbch": nb, "t": t, "kldpc": kldpc, "q": q, "rows": rows}
    out = Path(__file__).with_name("ldpc_tables.json")
    out.write_text(json.dumps(result, separators=(",", ":")), encoding="utf-8")
    print(f"scritto {out} ({out.stat().st_size} byte)")


if __name__ == "__main__":
    main()
