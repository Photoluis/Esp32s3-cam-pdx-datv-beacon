"""Beacon DATV IZ7PDX - genera firmware/main/dvbs2_tables.h dalle tabelle LDPC verificate (ldpc_tables.json).

Uso: python gen_c_tables.py ../../firmware/main/dvbs2_tables.h
"""
import json
import sys
from pathlib import Path

RATES = ["1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"]


def main():
    t = json.loads(Path(__file__).with_name("ldpc_tables.json").read_text(encoding="utf-8"))
    out = ["/* Beacon DATV IZ7PDX - tabelle DVB-S2 generate da tools/dvbs2/gen_c_tables.py (non modificare a mano).",
           " * Fonte: ETSI EN 302 307-1 V1.4.1, annessi B e C (estratte e verificate da tools/dvbs2/etsi_tables.py). */",
           "#pragma once", "#include <stdint.h>", "",
           "typedef struct {",
           "    uint32_t kbch, nbch, kldpc, nldpc;  /* bit */",
           "    uint16_t q, nrows;                  /* q = (nldpc - kldpc) / 360; righe = kldpc / 360 */",
           "    uint8_t t, modcod;                  /* capacità BCH; MODCOD QPSK (tabella 12) */",
           "    const uint8_t *rowlen;              /* indirizzi per riga */",
           "    const uint16_t *addr;               /* indirizzi delle righe, in sequenza */",
           "} pdx_s2_table_t;", ""]
    entries = []
    for frame, nldpc in (("normal", 64800), ("short", 16200)):
        for rate in RATES:
            if rate not in t[frame]:
                continue
            d = t[frame][rate]
            name = f"{frame}_{rate.replace('/', '_')}"
            flat = [x for r in d["rows"] for x in r]
            assert max(flat) < 65536
            out.append(f"static const uint8_t s2_rowlen_{name}[{len(d['rows'])}] = {{" +
                       ",".join(str(len(r)) for r in d["rows"]) + "};")
            lines = [",".join(str(x) for x in flat[i:i + 20]) for i in range(0, len(flat), 20)]
            out.append(f"static const uint16_t s2_addr_{name}[{len(flat)}] = {{\n    " + ",\n    ".join(lines) + "};")
            entries.append((frame, rate, d, nldpc, name))
    out += ["", "/* elenco: prima i frame normali (1/4 ... 9/10), poi i corti (1/4 ... 8/9) */",
            "static const pdx_s2_table_t pdx_s2_tables[] = {"]
    for frame, rate, d, nldpc, name in entries:
        out.append(f"    {{{d['kbch']}, {d['nbch']}, {d['kldpc']}, {nldpc}, {d['q']}, {len(d['rows'])}, {d['t']}, "
                   f"{RATES.index(rate) + 1}, s2_rowlen_{name}, s2_addr_{name}}},  /* {frame} {rate} */")
    out += ["};", f"#define PDX_S2_NTABLES {len(entries)}", ""]
    sys.path.insert(0, str(Path(__file__).parent))
    import encoder as enc
    out.append("/* generatori BCH (prodotto dei primi t polinomi delle tabelle 6a/6b): deg+1 coefficienti, x^deg per primo */")
    for frame, t_ in (("normal", 12), ("normal", 10), ("normal", 8), ("short", 12)):
        polys = enc.BCH_NORMAL if frame == "normal" else enc.BCH_SHORT
        g = 1
        for exps in polys[:t_]:
            g = enc.gf2_mul(g, sum(1 << e for e in exps))
        deg = g.bit_length() - 1
        nbytes = (deg + 1 + 7) // 8
        b = (g << (nbytes * 8 - deg - 1)).to_bytes(nbytes, "big")
        out.append(f"static const uint8_t s2_bch_{frame}_t{t_}[{nbytes}] = {{" + ",".join(f"0x{x:02x}" for x in b) + f"}};  /* grado {deg} */")
    out.append("")
    Path(sys.argv[1]).write_text("\n".join(out), encoding="utf-8")
    print(f"scritto {sys.argv[1]}: {len(entries)} tabelle")


if __name__ == "__main__":
    main()
