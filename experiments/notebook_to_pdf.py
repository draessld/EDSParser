#!/usr/bin/env python3
"""Execute a Jupyter notebook and render it to PDF without jupyter/nbconvert.

Runs each code cell in-process (capturing stdout, matplotlib figures, DataFrame
tables and display()/last-expression output), renders markdown cells with pandoc,
assembles a self-contained HTML file, and converts it to PDF with weasyprint.

Usage:
    python notebook_to_pdf.py results/vcf2eds/vcf2eds_evaluation.ipynb
    python notebook_to_pdf.py <notebook.ipynb> [output.pdf]

Requirements: pandas + matplotlib (kernel deps), `pandoc` and `weasyprint` on PATH
(or set WEASYPRINT env var to the binary). The notebook is executed with the CWD
set to its own directory-or-repo-root so its relative paths resolve.
"""
import ast, base64, html, io, json, os, shutil, subprocess, sys
from contextlib import redirect_stdout
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    nb_path = Path(argv[0]).resolve()
    if not nb_path.exists():
        print(f"Notebook not found: {nb_path}", file=sys.stderr)
        return 1
    pdf_out = Path(argv[1]).resolve() if len(argv) > 1 else nb_path.with_suffix(".pdf")
    html_out = pdf_out.with_suffix(".html")

    nb = json.loads(nb_path.read_text())

    # Execute relative to the notebook's directory (falls back to repo root behaviour
    # since the notebook itself probes a few candidate result dirs).
    os.chdir(nb_path.parent)

    ns = {"__name__": "__nb__"}
    pending = []  # rich outputs captured via display() within the current cell

    def render_value(v):
        if isinstance(v, (pd.DataFrame, pd.Series)):
            df = v.to_frame() if isinstance(v, pd.Series) else v
            return '<div class="df">' + df.to_html(border=0, max_rows=60) + "</div>"
        if v is None:
            return ""
        return f'<pre class="result">{html.escape(repr(v))}</pre>'

    ns["display"] = lambda obj: pending.append(render_value(obj))
    plt.show = lambda *a, **k: None  # figures are grabbed after each cell instead

    def fig_to_html():
        chunks = []
        for num in plt.get_fignums():
            buf = io.BytesIO()
            plt.figure(num).savefig(buf, format="png", bbox_inches="tight", dpi=130)
            b64 = base64.b64encode(buf.getvalue()).decode()
            chunks.append(f'<img class="figure" src="data:image/png;base64,{b64}">')
        plt.close("all")
        return chunks

    def md_to_html(src):
        if shutil.which("pandoc"):
            try:
                return subprocess.run(["pandoc", "-f", "gfm", "-t", "html"], input=src,
                                      capture_output=True, text=True, check=True).stdout
            except Exception:
                pass
        return "<pre>" + html.escape(src) + "</pre>"

    body = []
    for cell in nb["cells"]:
        src = "".join(cell["source"])
        if cell["cell_type"] == "markdown":
            body.append('<div class="md">' + md_to_html(src) + "</div>")
            continue
        if cell["cell_type"] != "code":
            continue

        pending.clear()
        body.append('<div class="code"><pre>' + html.escape(src) + "</pre></div>")
        stdout_buf, out_html, result = io.StringIO(), [], None
        try:
            tree = ast.parse(src)
            last_expr = tree.body.pop() if tree.body and isinstance(tree.body[-1], ast.Expr) else None
            with redirect_stdout(stdout_buf):
                exec(compile(tree, "<cell>", "exec"), ns)
                if last_expr is not None:
                    result = eval(compile(ast.Expression(last_expr.value), "<cell>", "eval"), ns)
        except Exception:
            import traceback
            out_html.append('<pre class="err">' + html.escape(traceback.format_exc()) + "</pre>")

        text = stdout_buf.getvalue()
        if text.strip():
            out_html.append('<pre class="stdout">' + html.escape(text) + "</pre>")
        out_html.extend(pending)
        out_html.extend(fig_to_html())
        if result is not None:
            out_html.append(render_value(result))
        if out_html:
            body.append('<div class="out">' + "\n".join(out_html) + "</div>")

    css = """
@page { size: A4; margin: 16mm 14mm; }
body { font-family: -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif;
       font-size: 10.5pt; color: #111; line-height: 1.45; }
h1 { font-size: 20pt; border-bottom: 2px solid #2a78d6; padding-bottom: 4px; }
h2 { font-size: 15pt; color: #184f95; margin-top: 22px; }
h3 { font-size: 12.5pt; color: #333; }
.md table { border-collapse: collapse; margin: 8px 0; }
.md th, .md td { border: 1px solid #ccc; padding: 3px 7px; }
.code pre { background: #f6f8fa; border: 1px solid #e1e4e8; border-left: 3px solid #2a78d6;
            border-radius: 4px; padding: 8px 10px; font-family: 'DejaVu Sans Mono', monospace;
            font-size: 8.6pt; white-space: pre-wrap; overflow-wrap: anywhere; }
.out { margin: 4px 0 14px; }
.stdout, .result { background: #fbfbfb; border: 1px solid #eee; padding: 6px 9px;
            font-family: 'DejaVu Sans Mono', monospace; font-size: 8.6pt; white-space: pre-wrap; }
.err { background: #fff5f5; border: 1px solid #f0b0b0; color: #a00; padding: 6px 9px;
       font-family: monospace; font-size: 8.4pt; white-space: pre-wrap; }
.figure { max-width: 100%; height: auto; display: block; margin: 8px auto; }
.df { overflow-x: auto; }
.df table { border-collapse: collapse; font-size: 8pt; margin: 4px 0; }
.df th, .df td { border: 1px solid #ddd; padding: 2px 6px; text-align: right; }
.df th { background: #f0f4fa; }
"""
    doc = ("<!doctype html><html><head><meta charset='utf-8'><style>" + css +
           "</style></head><body>" + "\n".join(body) + "</body></html>")
    html_out.write_text(doc)

    weasy = os.environ.get("WEASYPRINT") or shutil.which("weasyprint") \
        or str(Path.home() / "Lib/miniforge3/bin/weasyprint")
    if not Path(weasy).exists() and not shutil.which(weasy):
        print(f"weasyprint not found (tried {weasy!r}); HTML written to {html_out}", file=sys.stderr)
        return 2
    r = subprocess.run([weasy, str(html_out), str(pdf_out)], capture_output=True, text=True)
    if r.returncode != 0:
        print("weasyprint failed:\n" + r.stderr, file=sys.stderr)
        return 1
    print(f"Wrote {pdf_out} ({pdf_out.stat().st_size // 1024} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
