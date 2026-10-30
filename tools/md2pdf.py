"""Beacon DATV IZ7PDX - PDF della documentazione dai file Markdown di docs/ (stesso testo del repository).

Uso: python md2pdf.py [-o ../docs/Beacon_DATV_13cm_ESP32-S3_IZ7PDX.pdf]
Converte docs/PROGETTO.md e le appendici in un HTML da stampa e lo fa stampare in PDF da Microsoft Edge o Google Chrome
senza interfaccia (--headless --print-to-pdf). Il convertitore copre solo il Markdown usato nei documenti del progetto:
titoli, paragrafi, elenchi, tabelle, blocchi di codice, citazioni, grassetto, codice, collegamenti e immagini.
"""
import argparse
import html
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

DOCS = Path(__file__).resolve().parent.parent / "docs"
PARTS = [("PROGETTO.md", None),
         ("MONOSCOPIO.md", "Appendice A"),
         ("TEST_MER_20261030.md", "Appendice B")]

CSS = """
@page { size: A4; margin: 18mm 16mm 18mm 16mm; }
body { font-family: "Segoe UI", Roboto, Arial, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1b2733; }
h1 { font-size: 21pt; color: #0b4f75; border-bottom: 2px solid #36c2ff; padding-bottom: 4px; margin: 0 0 10px; }
h2 { font-size: 14.5pt; color: #0b4f75; margin: 18px 0 6px; break-after: avoid; }
h3 { font-size: 12pt; color: #14384f; margin: 14px 0 4px; break-after: avoid; }
p { margin: 5px 0 7px; }
ul, ol { margin: 4px 0 8px; padding-left: 20px; } li { margin: 2px 0; }
table { border-collapse: collapse; width: 100%; margin: 6px 0 10px; font-size: 9.5pt; break-inside: avoid; }
th, td { border: 1px solid #c4d0da; padding: 3px 6px; vertical-align: top; }
th { background: #e8f3fa; text-align: left; }
td.r, th.r { text-align: right; }
code { font-family: Consolas, "Courier New", monospace; font-size: 9pt; background: #eef2f5; padding: 0 3px; border-radius: 3px; }
pre { background: #f2f5f8; border: 1px solid #d5dee6; padding: 7px 9px; font-size: 8pt; line-height: 1.3; white-space: pre;
      overflow: hidden; break-inside: avoid; }
pre code { background: none; padding: 0; font-size: inherit; }
blockquote { border-left: 3px solid #36c2ff; margin: 8px 0; padding: 2px 10px; color: #3c4b57; background: #f4f9fc; }
img { max-width: 100%; max-height: 105mm; display: block; margin: 6px auto; border-radius: 3px; }
td img { max-height: 60mm; }
a { color: #0b6fa4; text-decoration: none; }
.part { break-before: page; }
.tag { color: #36a3d9; font-size: 11pt; font-weight: 600; letter-spacing: .08em; text-transform: uppercase; margin-bottom: 2px; }
.cover { height: 250mm; display: flex; flex-direction: column; justify-content: center; text-align: center; }
.cover h1 { border: 0; font-size: 28pt; }
.cover .sub { font-size: 14pt; color: #3c4b57; margin: 6px 0 18px; }
.cover img { max-height: 110mm; }
.cover .who { font-size: 13pt; margin-top: 16px; } .cover .lic { color: #6b7a86; font-size: 10pt; }
"""


def inline(s: str) -> str:
    """grassetto, codice, immagini, collegamenti (testo già senza HTML)"""
    codes = []

    def keep_code(m):
        codes.append(m.group(1))
        return f"\x00{len(codes) - 1}\x00"

    s = re.sub(r"`([^`]+)`", keep_code, s)
    s = html.escape(s, quote=False)
    s = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", lambda m: f'<img src="{m.group(2)}" alt="{m.group(1)}">', s)
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', s)
    s = re.sub(r"&lt;(https?://[^&]+)&gt;", r'<a href="\1">\1</a>', s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<i>\1</i>", s)
    return re.sub(r"\x00(\d+)\x00", lambda m: f"<code>{html.escape(codes[int(m.group(1))])}</code>", s)


def table(rows: list[str]) -> str:
    cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
    align = ["r" if c.endswith(":") and not c.startswith(":") else "" for c in cells[1]]
    out = ["<table>"]
    if any(cells[0]):                                    # intestazione vuota ("| | |") = tabella senza titoli
        out.append("<tr>" + "".join(f'<th class="{align[i] if i < len(align) else ""}">{inline(c)}</th>'
                                    for i, c in enumerate(cells[0])) + "</tr>")
    for r in cells[2:]:
        out.append("<tr>" + "".join(f'<td class="{align[i] if i < len(align) else ""}">{inline(c)}</td>'
                                    for i, c in enumerate(r)) + "</tr>")
    return "\n".join(out) + "</table>"


def md_to_html(md: str) -> str:
    lines, out, i = md.splitlines(), [], 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            out.append("<pre><code>" + html.escape("\n".join(lines[i + 1:j])) + "</code></pre>")
            i = j + 1
        elif m := re.match(r"(#{1,4}) (.*)", ln):
            n = len(m.group(1))
            out.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
            i += 1
        elif ln.startswith("|"):
            j = i
            while j < len(lines) and lines[j].startswith("|"):
                j += 1
            out.append(table(lines[i:j]))
            i = j
        elif re.match(r"\s*([-*]|\d+\.) ", ln):
            tag = "ol" if re.match(r"\s*\d+\.", ln) else "ul"
            items, j = [], i
            while j < len(lines) and (re.match(r"\s*([-*]|\d+\.) ", lines[j]) or
                                      (lines[j].startswith("  ") and lines[j].strip() and items)):
                if re.match(r"\s*([-*]|\d+\.) ", lines[j]):
                    items.append(re.sub(r"\s*([-*]|\d+\.) ", "", lines[j], count=1))
                else:
                    items[-1] += " " + lines[j].strip()
                j += 1
            out.append(f"<{tag}>" + "".join(f"<li>{inline(t)}</li>" for t in items) + f"</{tag}>")
            i = j
        elif ln.startswith(">"):
            j = i
            while j < len(lines) and lines[j].startswith(">"):
                j += 1
            out.append("<blockquote>" + inline(" ".join(x.lstrip("> ").strip() for x in lines[i:j])) + "</blockquote>")
            i = j
        elif not ln.strip():
            i += 1
        else:
            j = i
            while j < len(lines) and lines[j].strip() and not re.match(r"(#{1,4} |\||```|>|\s*([-*]|\d+\.) )", lines[j]):
                j += 1
            text = " ".join(x.strip() for x in lines[i:j])
            img = re.fullmatch(r"!\[([^\]]*)\]\(([^)]+)\)", text)
            out.append(f'<figure>{inline(text)}<figcaption style="text-align:center;font-size:9pt;color:#5a6a76">'
                       f"{html.escape(img.group(1))}</figcaption></figure>" if img and img.group(1)
                       else f"<p>{inline(text)}</p>")
            i = j
    return "\n".join(out)


def find_browser() -> str:
    for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Google\Chrome\Application\chrome.exe"):
        if Path(p).exists():
            return p
    for n in ("msedge", "google-chrome", "chromium", "chrome"):
        if shutil.which(n):
            return shutil.which(n)
    raise SystemExit("serve Microsoft Edge o Google Chrome per stampare il PDF")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default=str(DOCS / "Beacon_DATV_13cm_ESP32-S3_IZ7PDX.pdf"))
    a = ap.parse_args()
    body = ['<div class="cover"><div class="tag">DVB-S2 · 13 cm · ESP32-S3</div><h1>Beacon DATV 13 cm su ESP32-S3</h1>'
            '<div class="sub">Trasmettitore DVB-S2 autonomo con camera, su una sola scheda</div>'
            '<img src="img/test_mer_20261030/05_m9.6dB_MER26.0.jpg" alt="">'
            '<div class="who"><b>Luigi D\'Arcangelo, IZ7PDX</b></div><div class="lic">Ottobre 2026 · GPL-3.0</div></div>']
    for name, tag in PARTS:
        md = (DOCS / name).read_text(encoding="utf-8")
        body.append(f'<div class="part">{f"<div class=tag>{tag}</div>" if tag else ""}{md_to_html(md)}</div>')
    page = (f'<!doctype html><html lang="it"><head><meta charset="utf-8"><title>Beacon DATV 13 cm su ESP32-S3 - IZ7PDX</title>'
            f"<style>{CSS}</style></head><body>{''.join(body)}</body></html>")
    with tempfile.NamedTemporaryFile("w", suffix=".html", dir=DOCS, delete=False, encoding="utf-8") as f:
        f.write(page)
        tmp = Path(f.name)
    try:
        with tempfile.TemporaryDirectory() as prof:
            subprocess.run([find_browser(), "--headless=new", "--disable-gpu", "--no-first-run", f"--user-data-dir={prof}",
                            "--no-pdf-header-footer", f"--print-to-pdf={Path(a.output).resolve()}", tmp.as_uri()],
                           check=True, capture_output=True, timeout=180)
    finally:
        tmp.unlink(missing_ok=True)
    print(f"{a.output}: {Path(a.output).stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
