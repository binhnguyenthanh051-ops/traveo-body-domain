#!/usr/bin/env python3
"""stack_bound_gifs.py -- Post 2's GIFs: how stack_bound walks a task, finds a function pointer, refuses.

    <python with Pillow> host_tools/blog/stack_bound_gifs.py docs/bench/2026-11-17 docs/blogs/img/stack-2

Writes stack-walk.gif, stack-fnptr.gif and stack-refusal.gif; frame PNGs go to <out>/_gif/ (gitignored).
Needs Pillow and Microsoft Edge (render_diagrams.shot).

Data, all from docs/bench/2026-11-17/ (see stack_bound_post2.md there):
- can_task_walk.json: the call tree stack_bound walks for can_task (stack_bound_walk.py, cross-checked
  against the tool's own worst path), Node B image sha256 10b2b3d0577f;
- stack_bound_nodeB.txt: the real report (1464 B of 2048 B, 71 %, ok);
- refusal_nobinding.txt / refusal_wrongtarget.txt: the two real refusals (exit 2);
- the flash words and instructions quoted below: objdump of the same image (stack_bound_post2.md).
"""
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import render_diagrams as rd                                  # noqa: E402
from stack_demo_figs import INK, MONO, MUTED, SANS, t          # noqa: E402

W, H = 960, 600
ORANGE, GREEN, RED, BLUE, GREY = ("#fff7ed", "#f59e0b"), ("#dcfce7", "#22c55e"), ("#fee2e2", "#ef4444"), \
    ("#dbeafe", "#3b82f6"), ("#f3f4f6", "#9ca3af")


def frame(body: list[str]) -> str:
    return "\n".join([f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
                      f'<rect width="{W}" height="{H}" fill="#ffffff"/>'] + body + ["</svg>"]) + "\n"


def box(x, y, w, h, col, dash=False, sw=1.2):
    d = ' stroke-dasharray="6 4"' if dash else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="4" fill="{col[0]}" stroke="{col[1]}" stroke-width="{sw}"{d}/>'


def header(n, total, title, lines):
    o = [t(40, 40, f"{n} / {total}", 14, MUTED, font=MONO), t(40, 74, title, 22, INK, weight="bold")]
    for k, line in enumerate(lines):
        o.append(t(40, 104 + 24 * k, line, 16, INK))
    return o


# --------------------------------------------------------------------------------------------- walk

def walk_frames(d: dict, refuse: str | None = None) -> list[str]:
    levels = d["levels"]
    path_x, path_y, row = 40, 190, 30
    out = []

    def path_panel(k, upto_total, highlight_last=True):
        o = [t(path_x, path_y - 14, "the deepest path so far", 14, MUTED)]
        acc = 0
        for i, lv in enumerate(levels[:k]):
            acc += lv["frame"]
            col = ORANGE if (i == k - 1 and highlight_last) else BLUE
            via_blx = i > 0 and any(c["name"] == lv["name"] and c["via"] == "listed" for c in levels[i - 1]["callees"])
            o.append(box(path_x, path_y + i * row, 330, row - 4, col, dash=via_blx))
            if via_blx:
                o.append(t(path_x + 250, path_y + i * row + 18, "blx", 12, ORANGE[1], "end", weight="bold"))
            o.append(t(path_x + 10, path_y + i * row + 18, lv["name"], 14, INK, font=MONO))
            o.append(t(path_x + 320, path_y + i * row + 18, f'{lv["frame"]} B', 14, INK, "end", MONO))
        if k:
            o.append(t(path_x, path_y + k * row + 22, f"frames so far: {acc} B", 15, INK, weight="bold"))
        return o

    def calls_panel(lv, pick_listed_note=True, unknown=False):
        cx, cy = 420, 190
        o = [t(cx, cy - 14, f'what {lv["name"]} calls · depth = frame + deepest below', 14, MUTED)]
        kids = lv["callees"][:6]
        scale = 330 / max(c["depth"] for c in levels[0]["callees"])
        for i, c in enumerate(kids):
            y = cy + i * 40
            best = i == 0
            listed = c["via"] == "listed"
            if unknown and listed:
                o.append(box(cx, y, 500, 32, RED, dash=True))
                o.append(t(cx + 10, y + 21, "blx fp  →  ?   (no binding in the project file)", 14, RED[1], font=MONO, weight="bold"))
                continue
            o.append(f'<rect x="{cx}" y="{y}" width="500" height="32" rx="4" fill="#fafafa" stroke="#e5e7eb"/>')
            if c["depth"]:
                bar = ORANGE if best else ("#e5e7eb", "#cbd5e1")
                o.append(box(cx, y, c["depth"] * scale, 32, bar, dash=listed))
            o.append(t(cx + 10, y + 21, c["name"], 14, INK, font=MONO, weight="bold" if best else "normal"))
            if listed:
                o.append(t(cx + 420, y + 21, "via blx", 13, ORANGE[1], "end", weight="bold"))
            o.append(t(cx + 500, y + 21, f'{c["depth"]} B', 14, INK, "end", MONO, "bold" if best else "normal"))
        more = len(lv["callees"]) - len(kids)
        if more > 0:
            o.append(t(cx, cy + len(kids) * 40 + 16, f"… and {more} more, all smaller", 13, MUTED))
        return o

    n_total = len(levels) + 2 if refuse is None else 0
    if refuse is None:
        out.append(frame(header(1, n_total, "How deep can can_task's stack get?",
                                ["Its stack is 2048 B. stack_bound reads the build, not the chip.",
                                 "Every function gets a number: its own frame + the deepest call below it."])
                         + path_panel(0, 0) + calls_panel({"name": "can_task", "callees": levels[0]["callees"]}, False)))
        for k, lv in enumerate(levels, start=1):
            nxt = lv["callees"][0]["name"] if lv["callees"] else None
            why = [f'In {lv["name"]} ({lv["frame"]} B), the deepest call is {nxt}.' if nxt else
                   f'{lv["name"]} calls nothing deeper: the end of the path.',
                   ("It's reached only through blx (a function pointer): the project file binds it."
                    if lv["callees"] and lv["callees"][0]["via"] == "listed" else
                    "Every other branch is smaller, so the walk follows this one.") if nxt else
                   f'Total of all frames on the path: {d["path_bytes"]} B.']
            out.append(frame(header(k + 1, n_total, f"Step {k}: {lv['name']}", why)
                             + path_panel(k, 0) + (calls_panel(lv) if lv["callees"] else [])))
        used = d["required"] / d["configured"]
        o = header(n_total, n_total, "The answer, before anything is flashed",
                   [f'{d["path_bytes"]} B of frames + {d["switch_bytes"]} B for a task switch = {d["required"]} B.',
                    f'That is {used:.0%} of the {d["configured"]} B stack. The rule is: at most 80%.'])
        o += path_panel(len(levels), 0, False)
        o.append(box(420, 300, 500, 90, GREEN, sw=2))
        o.append(t(670, 340, f'{d["required"]} B / {d["configured"]} B = {used:.0%}', 20, INK, "middle", MONO, "bold"))
        o.append(t(670, 372, "ok: the build goes on", 16, GREEN[1], "middle", weight="bold"))
        out.append(frame(o))
        return out
    return out


# --------------------------------------------------------------------------------------------- function pointer

WORDS = [(0x10087EA0, 0x00000080), (0x10087EA4, 0x280E0100), (0x10087EA8, 0x10087EB0), (0x10087EAC, 0x0000005E),
         (0x10087EB0, 0x10086129), (0x10087EB4, 0x00000006), (0x10087EB8, 0x40220000), (0x10087EBC, 0x05040302)]
FUNCS = {0x10086128: "crypto_mac"}            # the function starts the scan compares against (nm, same image)


def fnptr_frames() -> list[str]:
    out, n = [], 6

    def code(y, lines, col=GREY):
        o = [box(40, y, 880, 30 * len(lines) + 20, col)]
        for k, (txt, note, c) in enumerate(lines):
            o.append(t(60, y + 32 + 30 * k, txt, 15, INK, font=MONO))
            if note:
                o.append(t(560, y + 32 + 30 * k, note, 15, c, weight="bold"))
        return o

    out.append(frame(header(1, n, "Two calls inside verify_core", ["Both call a function. Only one says which."])
                     + code(150, [("1008689e  bl   100867c4 <build_mac_input>", "target written in the instruction", GREEN[1]),
                                  ("100868b0  blx  fp", "target in a register: unknown", RED[1])])))
    out.append(frame(header(2, n, "Where does fp come from?",
                            ["Two loads from memory. In C it's one line: cy->oracle->mac(...)",
                             "So the answer is somewhere in the image's data. The tool goes looking."])
                     + code(170, [("100868a4  ldr    r3, [r7, #0]", "r3 = cy->oracle", INK),
                                  ("100868a6  ldr.w  fp, [r3]", "fp = oracle->mac", INK),
                                  ("100868b0  blx    fp", "call whatever fp holds", RED[1])])))

    def words(cursor, verdict):
        o = [t(40, 214, "words in flash (objdump -s of the same image)", 14, MUTED)]
        for i, (a, v) in enumerate(WORDS):
            y = 230 + i * 36
            col = GREY
            if i == cursor:
                col = GREEN if (v - 1) in FUNCS else RED
            o.append(box(40, y, 520, 30, col, sw=2 if i == cursor else 1.2))
            o.append(t(56, y + 20, f"{a:08x}", 14, MUTED, font=MONO))
            o.append(t(200, y + 20, f"{v:08x}", 15, INK, font=MONO, weight="bold" if i == cursor else "normal"))
        if verdict:
            o.append(box(590, 230, 330, 120, GREEN if "crypto_mac" in verdict[0] else RED, sw=2))
            for k, line in enumerate(verdict):
                o.append(t(606, 262 + 26 * k, line, 15, INK, weight="bold" if k == 0 else "normal"))
        return o

    out.append(frame(header(3, n, "Scan: is this word a function's address?",
                            ["A function address on Cortex-M is odd: its start + 1 (the Thumb bit).",
                             "This one points at the data the code reads, not at code."])
                     + words(2, ["0x10087eb0 is even", "→ not a code address", "(it's g_oracle itself)"])))
    out.append(frame(header(4, n, "Found one",
                            ["0x10086129 − 1 = 0x10086128, the first instruction of crypto_mac.",
                             "So crypto_mac's address is stored here: it can be called through a pointer."])
                     + words(4, ["0x10086129 = crypto_mac + 1", "→ crypto_mac is address-taken", "at 0x10087eb0 (g_oracle)"])))
    o = header(5, n, "The binding, checked against the binary",
               ["I write which function the blx can reach. The tool checks I'm not wrong:",
                "the target must exist, and its address must be stored somewhere."])
    o += code(170, [('[[indirect]]', "", INK), ('function = "verify_core"', "", INK), ('targets  = ["crypto_mac"]', "address-taken ✓", GREEN[1])], BLUE)
    out.append(frame(o))
    o = header(6, n, "Now the walk can go on",
               ["The hidden edge is in the graph, with its 568 B frame.",
                "A tool that ignored the blx would come up 704 B short."])
    o.append(box(40, 190, 260, 50, BLUE)); o.append(t(170, 221, "verify_core · 136 B", 15, INK, "middle", MONO))
    o.append(f'<line x1="300" y1="215" x2="420" y2="215" stroke="{ORANGE[1]}" stroke-width="3" stroke-dasharray="8 5"/>')
    o.append(t(360, 205, "blx", 14, ORANGE[1], "middle", MONO, "bold"))
    o.append(box(420, 190, 260, 50, ORANGE, sw=2)); o.append(t(550, 221, "crypto_mac · 568 B", 15, INK, "middle", MONO, "bold"))
    o.append(t(40, 290, "depth below verify_core: 136 + 728 = 864 B, not 136 + 24 = 160 B", 16, INK))
    out.append(frame(o))
    return out


# --------------------------------------------------------------------------------------------- refusal

def refusal_frames(d: dict, msg_none: str, msg_wrong: str) -> list[str]:
    out, n = [], 4
    levels = d["levels"]
    vc = next(i for i, lv in enumerate(levels) if lv["name"] == "verify_core")

    def path(k, end_col):
        o = [t(40, 176, "the walk so far", 14, MUTED)]
        for i, lv in enumerate(levels[:k]):
            o.append(box(40, 190 + i * 30, 330, 26, end_col if i == k - 1 else BLUE))
            o.append(t(50, 208 + i * 30, lv["name"], 14, INK, font=MONO))
        return o

    def msg(y, title, text, col):
        o = [box(420, y, 500, 150, col, sw=2), t(436, y + 30, title, 16, INK, weight="bold")]
        words, line, k = text.split(), "", 0
        for w_ in words:
            if len(line) + len(w_) > 52:
                o.append(t(436, y + 58 + 22 * k, line, 13, INK, font=MONO)); k += 1; line = ""
            line += w_ + " "
        o.append(t(436, y + 58 + 22 * k, line, 13, INK, font=MONO))
        return o

    out.append(frame(header(1, n, "What if I forget the binding?",
                            ["Delete verify_core's entry from the project file and run it again."])
                     + path(vc + 1, ORANGE)))
    out.append(frame(header(2, n, "It stops, and says where",
                            ["A blx with no binding means the depth is unknown.",
                             "No number at all is safer than a wrong one."])
                     + path(vc + 1, RED) + msg(190, "exit 2: no result", msg_none.split(": ", 1)[1], RED)))
    out.append(frame(header(3, n, "What if I bind it to the wrong function?",
                            ["Say I claim the blx can reach secoc_rx_process."])
                     + path(vc + 1, RED) + msg(190, "exit 2: no result", msg_wrong, RED)))
    o = header(4, n, "The right binding: an answer again",
               [f'crypto_mac is address-taken, so the walk goes on: {d["required"]} B of {d["configured"]} B.'])
    o += path(len(levels), BLUE)
    o.append(box(420, 190, 500, 90, GREEN, sw=2))
    o.append(t(670, 230, f'exit 0 · {d["required"] / d["configured"]:.0%} used', 20, INK, "middle", MONO, "bold"))
    o.append(t(670, 262, "ok", 16, GREEN[1], "middle", weight="bold"))
    out.append(frame(o))
    return out


def build(svgs: list[str], out: pathlib.Path, name: str, pil, first=3200, each=2400, last=4200) -> None:
    work = out / "_gif" / name
    work.mkdir(parents=True, exist_ok=True)
    imgs = []
    for i, s in enumerate(svgs, start=1):
        svg = work / f"f{i:02d}.svg"
        svg.write_text(s, encoding="utf-8", newline="\n")
        ET.fromstring(svg.read_text(encoding="utf-8"))   # a broken frame would bake a browser error page into the GIF
        png = work / f"f{i:02d}.png"
        png.unlink(missing_ok=True)
        rd.shot(svg, png, W, H, out / "_edge-profile")
        im = pil.open(png).convert("RGB").resize((W, H), pil.LANCZOS)
        imgs.append(im.quantize(colors=96, method=pil.Quantize.MEDIANCUT, dither=pil.Dither.NONE))
    dur = [each] * len(imgs)
    dur[0], dur[-1] = first, last
    gif = out / f"{name}.gif"
    imgs[0].save(gif, save_all=True, append_images=imgs[1:], duration=dur, loop=0, optimize=True)
    print(f"{gif}: {len(imgs)} frames, {gif.stat().st_size // 1024} KB")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    from PIL import Image as pil
    bench, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    d = json.loads((bench / "can_task_walk.json").read_text(encoding="utf-8"))
    report = (bench / "stack_bound_nodeB.txt").read_text(encoding="utf-8")
    assert f'task can_task             {d["required"]} B      {d["configured"]} B' in report, "walk data != report"
    none = (bench / "refusal_nobinding.txt").read_text(encoding="utf-8").splitlines()[1].strip(" -")
    wrong = (bench / "refusal_wrongtarget.txt").read_text(encoding="utf-8").splitlines()[1].strip(" -")
    none = none.replace("C:/00_Projects/00_Portfolio/00_TraveoBody/traveo-body-domain/", "")
    build(walk_frames(d), out, "stack-walk", pil)
    build(fnptr_frames(), out, "stack-fnptr", pil)
    build(refusal_frames(d, none, wrong), out, "stack-refusal", pil)
    return 0


if __name__ == "__main__":
    sys.exit(main())
