#!/usr/bin/env python3
"""render_diagrams.py -- render a draft's Mermaid blocks (and hero.svg) to PNG, locally.

Two steps, because headless Edge here can screenshot but not dump the DOM:

    # 1. write the pages; open docs/blogs/img/<post>/_measure.html in any browser and
    #    copy the JSON it shows ({"fig-01": [w, h], ...}) into sizes.json next to it
    python host_tools/blog/render_diagrams.py pages docs/blogs/m5-secoc-blog-draft.md docs/blogs/img/m5
    # 2. screenshot each figure at its own size (2x) with headless Microsoft Edge
    python host_tools/blog/render_diagrams.py shoot docs/blogs/img/m5

Output: fig-01.png, fig-02.png, ... in block order, plus hero.png from hero.svg.
The Mermaid library comes from its CDN; nothing is downloaded to disk.
"""
import html
import json
import pathlib
import re
import subprocess
import sys
import time

EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
MERMAID_JS = "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"
SCALE = 2
PAD = 16
INIT = ("mermaid.initialize({startOnLoad:true, theme:'default', "
        "themeVariables:{fontFamily:'Segoe UI, Arial, sans-serif', fontSize:'16px'}});")

FIG_PAGE = """<!doctype html><html><head><meta charset="utf-8"><script src="{js}"></script>
</head><body style="margin:0;padding:{pad}px;background:#fff"><pre class="mermaid" style="margin:0">{code}</pre><style>svg{{max-width:none!important}}</style>
<script>{init}</script></body></html>"""

MEASURE_PAGE = """<!doctype html><html><head><meta charset="utf-8"><script src="{js}"></script>
</head><body style="margin:0;background:#fff">{blocks}
<pre id="out" style="font:14px monospace;padding:16px"></pre>
<script>{init}
mermaid.run().then(() => {{
  const r = {{}};
  document.querySelectorAll('div[data-fig]').forEach(d => {{
    const vb = d.querySelector('svg').viewBox.baseVal;   // natural size, not the
    r[d.dataset.fig] = [Math.ceil(vb.width), Math.ceil(vb.height)];   // narrow-window size
  }});
  document.getElementById('out').textContent = JSON.stringify(r);
  window.__sizes = r;
}});</script></body></html>"""


def svg_size(svg: str) -> tuple[int, int]:
    vb = re.search(r'viewBox="([-\d.]+) ([-\d.]+) ([\d.]+) ([\d.]+)"', svg)
    return int(float(vb.group(3))), int(float(vb.group(4)))


def shot(page: pathlib.Path, png: pathlib.Path, w: int, h: int, profile: pathlib.Path) -> None:
    subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--no-first-run", "--disable-extensions",
                    "--hide-scrollbars", f"--user-data-dir={profile}", "--virtual-time-budget=10000",
                    f"--force-device-scale-factor={SCALE}", f"--window-size={w},{h}",
                    f"--screenshot={png}", page.as_uri()], capture_output=True, timeout=120)
    # msedge.exe is a launcher: it can return before its child has written the file.
    for _ in range(120):
        if png.exists() and png.stat().st_size > 0:
            time.sleep(0.5)
            break
        time.sleep(0.5)
    if not png.exists():
        raise RuntimeError(f"{png.name}: screenshot failed")


def pages(md_path: str, out: str) -> int:
    out_dir = pathlib.Path(out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    blocks = re.findall(r"```mermaid\n(.*?)```", pathlib.Path(md_path).read_text(encoding="utf-8"), flags=re.S)
    for i, code in enumerate(blocks, 1):
        (out_dir / f"_fig-{i:02d}.html").write_text(
            FIG_PAGE.format(js=MERMAID_JS, pad=PAD, code=html.escape(code), init=INIT), encoding="utf-8")
    divs = "\n".join(f'<div data-fig="fig-{i:02d}" style="display:inline-block">'
                     f'<pre class="mermaid" style="margin:0">{html.escape(c)}</pre></div><br>'
                     for i, c in enumerate(blocks, 1))
    (out_dir / "_measure.html").write_text(MEASURE_PAGE.format(js=MERMAID_JS, blocks=divs, init=INIT),
                                           encoding="utf-8")
    print(f"{len(blocks)} figures; now measure {out_dir / '_measure.html'} -> sizes.json")
    return 0


def shoot(out: str) -> int:
    out_dir = pathlib.Path(out).resolve()
    profile = out_dir / "_edge-profile"
    sizes = json.loads((out_dir / "sizes.json").read_text(encoding="utf-8"))
    for name, (w, h) in sorted(sizes.items()):
        png = out_dir / f"{name}.png"
        png.unlink(missing_ok=True)
        shot(out_dir / f"_{name}.html", png, w + 2 * PAD, h + 2 * PAD, profile)
        print(f"{png.name}  {png.stat().st_size // 1024} KB")
    hero = out_dir / "hero.svg"
    if hero.exists():
        w, h = svg_size(hero.read_text(encoding="utf-8"))
        (out_dir / "hero.png").unlink(missing_ok=True)
        shot(hero, out_dir / "hero.png", w, h, profile)
        print(f"hero.png  {(out_dir / 'hero.png').stat().st_size // 1024} KB")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "pages":
        sys.exit(pages(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 3 and sys.argv[1] == "shoot":
        sys.exit(shoot(sys.argv[2]))
    print(__doc__)
    sys.exit(2)
