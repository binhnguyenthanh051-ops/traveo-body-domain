#!/usr/bin/env python3
"""md_to_medium.py -- turn a blog draft (Markdown) into HTML that pastes cleanly into Medium.

    python host_tools/blog/md_to_medium.py docs/blogs/m5-secoc-blog-draft.md
    # -> docs/blogs/m5-secoc-medium.html ; open it in a browser, Ctrl+A, Ctrl+C,
    #    paste into a new Medium story.

What it does, because Medium's editor can't:
- Mermaid blocks -> <img> rendered by mermaid.ink (a public renderer). Medium fetches
  and re-hosts the image when you paste or import.
- Tables -> bullet lists (Medium has no tables): first column bold, the rest as
  "Header: value" pairs.
- `TODO(...)` -> highlighted, so nothing unfinished gets published by accident.
- Drops the editor-only parts: the draft-status note, the Mermaid/colour-key note,
  HTML comments, and the "Figures list" section.
Only the Markdown this repo's drafts use is supported (headings, paragraphs, lists,
blockquotes, fenced code, tables, bold/italic/code/links). No third-party packages.
"""
import base64
import html
import pathlib
import re
import sys

MERMAID_INK = "https://mermaid.ink/img/{}?type=png&bgColor=FFFFFF&width=1400"

SKIP_QUOTES = ("> **Draft status", "> *Diagrams are Mermaid", "> *Colour key")


def mermaid_url(code: str) -> str:
    b64 = base64.urlsafe_b64encode(code.strip().encode("utf-8")).decode("ascii")
    return MERMAID_INK.format(b64)


def inline(text: str) -> str:
    """Escape, then apply `code`, [links](url), **bold**, *italic*, TODO highlight."""
    codes = []

    def keep_code(m):
        codes.append("<code>" + html.escape(m.group(1)) + "</code>")
        return f"\x00{len(codes) - 1}\x00"

    t = re.sub(r"`([^`]+)`", keep_code, text)
    t = html.escape(t, quote=False)
    t = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', t)
    t = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<em>\1</em>", t)
    t = re.sub(r"\x00(\d+)\x00", lambda m: codes[int(m.group(1))], t)
    t = re.sub(r"(<code>)?(TODO\([^)]*\)[^<`]*)(</code>)?",
               lambda m: f'<mark>{m.group(2)}</mark>', t)
    return t


def table_to_list(rows: list[list[str]]) -> str:
    head, data = rows[0], rows[1:]
    numbered = head[0].strip() in ("#", "No", "No.")
    items = []
    for r in data:
        r = r + [""] * (len(head) - len(r))
        if numbered and len(r) > 1:
            label, rest, rest_head = f"{r[0]} · {r[1]}", r[2:], head[2:]
        else:
            label, rest, rest_head = r[0], r[1:], head[1:]
        parts = [f"{inline(h)}: {inline(v)}" if h.strip() else inline(v)
                 for h, v in zip(rest_head, rest) if v.strip()]
        items.append(f"<li><strong>{inline(label)}</strong>"
                     + (" — " + "; ".join(parts) if parts else "") + "</li>")
    return "<ul>\n" + "\n".join(items) + "\n</ul>"


def split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def convert(md: str, img_base: str | None = None, fig_dir: str = "") -> str:
    """img_base: public URL of the draft's folder (e.g. raw GitHub .../docs/blogs).
    When set, ![](rel/path) resolves to <img_base>/rel/path, and Mermaid block N to
    <img_base>/<fig_dir>/fig-NN.png (pre-rendered by render_diagrams.py) instead of
    a mermaid.ink URL."""
    md = re.sub(r"<!--.*?-->", "", md, flags=re.S)
    md = md.split("\n## Figures list", 1)[0]
    lines = md.split("\n")
    out, i, title, fig = [], 0, "Draft", 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if not s:
            i += 1
            continue
        if s.startswith("```"):
            lang = s[3:].strip()
            j = i + 1
            while j < len(lines) and not lines[j].strip().startswith("```"):
                j += 1
            code = "\n".join(lines[i + 1:j])
            if lang == "mermaid":
                fig += 1
                src = f"{img_base}/{fig_dir}/fig-{fig:02d}.png" if img_base else mermaid_url(code)
                out.append(f'<figure><img src="{src}" alt="diagram"></figure>')
            else:
                out.append("<pre><code>" + html.escape(code) + "</code></pre>")
            i = j + 1
            continue
        im = re.match(r"^!\[([^\]]*)\]\(([^)\s]+)\)$", s)
        if im:
            src = im.group(2)
            if img_base and not re.match(r"^https?://", src):
                src = f"{img_base}/{src}"
            out.append(f'<figure><img src="{src}" alt="{html.escape(im.group(1))}"></figure>')
            i += 1
            continue
        m = re.match(r"^(#{1,3}) (.*)$", s)
        if m:
            text = re.sub(r"\s*\*\([^)]*\)\*\s*$", "", m.group(2))
            level = len(m.group(1))
            if level == 1:
                title = text
            out.append(f"<h{level}>{inline(text)}</h{level}>")
            i += 1
            continue
        if s == "---":
            out.append("<hr>")
            i += 1
            continue
        if s.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                if not re.match(r"^\|[\s:|-]+\|$", lines[i].strip()):
                    rows.append(split_row(lines[i]))
                i += 1
            out.append(table_to_list(rows))
            continue
        if s.startswith(">"):
            block = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                block.append(lines[i].strip()[1:].strip())
                i += 1
            if any(("> " + block[0]).startswith(p) for p in SKIP_QUOTES):
                continue
            out.append("<blockquote>" + inline(" ".join(b for b in block if b)) + "</blockquote>")
            continue
        if re.match(r"^(- |\d+\. )", s):
            ordered = bool(re.match(r"^\d+\. ", s))
            items = []
            while i < len(lines) and lines[i].strip():
                l2 = lines[i]
                if re.match(r"^(- |\d+\. )", l2.strip()) and not l2.startswith("  "):
                    items.append(re.sub(r"^(- |\d+\. )", "", l2.strip()))
                elif items:
                    items[-1] += " " + l2.strip()
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>\n" + "\n".join(f"<li>{inline(t)}</li>" for t in items) + f"\n</{tag}>")
            continue
        para = []
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#{1,3} |```|\||>|---$|- |\d+\. )", lines[i].strip()):
            para.append(lines[i].strip())
            i += 1
        out.append("<p>" + inline(" ".join(para)) + "</p>")
    body = "\n".join(out)
    return (f"<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>{html.escape(title)}</title>\n"
            "<style>body{max-width:720px;margin:2em auto;font:18px/1.6 Georgia,serif;padding:0 16px}"
            "img{max-width:100%}pre{background:#f4f4f4;padding:12px;overflow-x:auto;font-size:14px}"
            "mark{background:#ffe08a}blockquote{border-left:3px solid #ccc;margin-left:0;padding-left:16px;color:#444}"
            "</style></head><body>\n" + body + "\n</body></html>\n")


def main() -> int:
    args = sys.argv[1:]
    img_base = None
    if len(args) == 3 and args[1] == "--img-base":
        img_base = args[2].rstrip("/")
        args = args[:1]
    if len(args) != 1:
        print(__doc__.splitlines()[0])
        return 2
    src = pathlib.Path(args[0])
    dst = src.with_name(src.stem.replace("-blog-draft", "") + "-medium.html")
    fig_dir = "img/" + src.stem.split("-", 1)[0]          # m5-secoc-blog-draft -> img/m5
    dst.write_text(convert(src.read_text(encoding="utf-8"), img_base, fig_dir), encoding="utf-8")
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
