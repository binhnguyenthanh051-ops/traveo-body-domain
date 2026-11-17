#!/usr/bin/env python3
"""stack_demo_figs.py -- draw Post 1's stack figures from the bench capture itself.

    python host_tools/blog/stack_demo_figs.py docs/bench/2026-11-16 docs/blogs/img/stack-1
    python host_tools/blog/render_diagrams.py shoot docs/blogs/img/stack-1   # -> *.png

Reads stack_demo_capture.txt and stack_demo_overflow.txt (GDB `x/128wx` dumps of Node B's demo
task stack, 2026-11-16) and draws every word from them: no value is typed in by hand. What each
word *is* (whose frame, which slot) comes from the disassembly of the same image, recorded in
docs/bench/2026-11-16/stack_demo.md.

Memory is drawn the way a debugger's memory window lists it: lowest address at the top. So the
stack fills from the bottom (its first word, 0x28007290) upwards, like water in a tank.
"""
import pathlib
import re
import sys

STACK_LO, STACK_HI = 0x28007094, 0x28007294       # s_demo_stack, 512 B
TOP_WORD = 0x28007290                             # never used (alignment); the task starts with sp here
SPILL_LO = 0x28006E94                             # s_demo_spill, right below the stack
SP = {1: 0x28007288, 2: 0x28007270, 3: 0x28007258, 4: 0x28007240, 5: 0x28007288}

# address -> (owner, what). From the disassembly (stack_demo.md "Check 1").
SLOTS = {
    0x28007240: ("func_c", "arr_c[0]"), 0x28007244: ("func_c", "arr_c[1]"),
    0x28007248: ("func_c", "arr_c[2]"), 0x2800724C: ("func_c", "arr_c[3] = x"),
    0x28007250: ("func_c", "padding"), 0x28007254: ("func_c", "way back → func_b"),
    0x28007258: ("func_b", "arr_b[0]"), 0x2800725C: ("func_b", "arr_b[1]"),
    0x28007260: ("func_b", "padding"), 0x28007264: ("func_b", "saved r4"),
    0x28007268: ("func_b", "saved r5 (func_a's x)"), 0x2800726C: ("func_b", "way back → func_a"),
    0x28007270: ("func_a", "arr_a[0]"), 0x28007274: ("func_a", "arr_a[1]"),
    0x28007278: ("func_a", "padding"), 0x2800727C: ("func_a", "saved r4"),
    0x28007280: ("func_a", "saved r5"), 0x28007284: ("func_a", "way back → task"),
    0x28007288: ("task", "saved r3 (alignment)"), 0x2800728C: ("task", "way back → prvTaskExitError"),
    0x28007290: ("", "never used"),
}
FRAMES = [("task", 0x28007288, TOP_WORD), ("func_a", 0x28007270, 0x28007288),
          ("func_b", 0x28007258, 0x28007270), ("func_c", 0x28007240, 0x28007258)]
LIVE = {1: 1, 2: 2, 3: 3, 4: 4, 5: 1}             # how many FRAMES are live at each step

COL = {"task": ("#ede9fe", "#8b5cf6"), "func_a": ("#dbeafe", "#3b82f6"),
       "func_b": ("#dcfce7", "#22c55e"), "func_c": ("#fff7ed", "#f59e0b")}
DRY = ("#f3f4f6", "#d1d5db")
RED = ("#fee2e2", "#ef4444")
INK, MUTED = "#1f2937", "#6b7280"
MONO = "Consolas, 'Courier New', monospace"
SANS = "'Segoe UI', Arial, sans-serif"
A5 = 0xA5A5A5A5


def parse(path: pathlib.Path) -> list[dict[int, int]]:
    """Every `x/128wx` block in a GDB log, in order, as {address: value}."""
    dumps, cur = [], None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^(0x[0-9a-f]+) <[^>]*>:\s+(.*)$", line)
        if not m:
            if cur:
                dumps.append(cur)
                cur = None
            continue
        cur = cur or {}
        addr = int(m.group(1), 16)
        for i, v in enumerate(m.group(2).split()):
            cur[addr + 4 * i] = int(v, 16)
    if cur:
        dumps.append(cur)
    return dumps


def svg(w: int, h: int, body: list[str]) -> str:
    return "\n".join([
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">',
        "<defs>",
        '<pattern id="stale" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
        '<rect width="8" height="8" fill="#eef1f5"/><line x1="0" y1="0" x2="0" y2="8" stroke="#b6c0cc" stroke-width="3"/></pattern>',
        f'<marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
        f'<path d="M0,0 L10,5 L0,10 z" fill="{INK}"/></marker>',
        "</defs>",
        f'<rect width="{w}" height="{h}" fill="#ffffff"/>'] + body + ["</svg>"]) + "\n"


def t(x, y, s, size=14, fill=INK, anchor="start", font=SANS, weight="normal"):
    return (f'<text x="{x}" y="{y:.1f}" font-family="{font}" font-size="{size}" fill="{fill}" '
            f'text-anchor="{anchor}" font-weight="{weight}">{s}</text>')


def state(dump: dict[int, int], step: int, addr: int) -> str:
    """'live:<owner>', 'stale' (below sp, not 0xA5), or 'dry' (still 0xA5)."""
    if addr >= SP[step]:
        for owner, lo, hi in FRAMES[:LIVE[step]]:
            if lo <= addr < hi:
                return "live:" + owner
    return "dry" if dump[addr] == A5 else "stale"


# ---------------------------------------------------------------------------------------------
# Figure: four tanks, steps 1-4 (and the stain, steps 4-5), drawn from the dumps

WIN_LO = 0x28007230                     # the window drawn word by word; above it: collapsed, all 0xA5
CELL = 16


def tank(dump, step, x, top, label, show_addr, notes=()):
    out, w = [], 150
    y0 = top + 40                       # collapsed part: 0x28007094 .. WIN_LO
    out.append(t(x + w / 2, top - 14, label, 15, INK, "middle", weight="bold"))
    dry_above = all(dump[a] == A5 for a in range(STACK_LO, WIN_LO, 4))
    out.append(f'<rect x="{x}" y="{top}" width="{w}" height="34" fill="{DRY[0]}"/>')
    out.append(t(x + w / 2, top + 22, f"{(WIN_LO - STACK_LO) // 4} more words, all 0xA5" if dry_above
                 else "(not all 0xA5)", 11, MUTED, "middle"))
    out.append(f'<path d="M{x},{top + 34} l{w / 4},-6 l{w / 4},6 l{w / 4},-6 l{w / 4},6" fill="none" stroke="#9ca3af"/>')
    for i, a in enumerate(range(WIN_LO, STACK_HI, 4)):
        y = y0 + i * CELL
        st = state(dump, step, a)
        if st.startswith("live:"):
            fill, stroke = COL[st[5:]]
            out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{CELL}" fill="{fill}" stroke="{stroke}" stroke-width="0.8"/>')
        elif st == "stale":
            out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{CELL}" fill="url(#stale)" stroke="#b6c0cc" stroke-width="0.6"/>')
        else:
            out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{CELL}" fill="{DRY[0]}" stroke="{DRY[1]}" stroke-width="0.6"/>')
        out.append(t(x + w / 2, y + 12, f"{dump[a]:08x}", 11, INK if st != "dry" else "#b0b5bc", "middle", MONO))
        if show_addr and (a % 0x10 == 0 or a in (STACK_HI - 4,)):
            out.append(t(x - 8, y + 12, f"0x{a:08x}", 11, MUTED, "end", MONO))
    # walls
    floor = y0 + ((STACK_HI - WIN_LO) // 4) * CELL
    out.append(f'<path d="M{x - 2},{top - 2} L{x - 2},{floor + 2} L{x + w + 2},{floor + 2} L{x + w + 2},{top - 2}" '
               f'fill="none" stroke="{INK}" stroke-width="3" stroke-linejoin="round"/>')
    # sp
    sy = y0 + ((SP[step] - WIN_LO) // 4) * CELL
    out.append(f'<line x1="{x + w + 34}" y1="{sy}" x2="{x + w + 6}" y2="{sy}" stroke="{INK}" stroke-width="2" marker-end="url(#arr)"/>')
    out.append(t(x + w + 38, sy + 5, "sp", 13, INK, font=MONO, weight="bold"))
    for a, text, colour in notes:
        ny = y0 + ((a - WIN_LO) // 4) * CELL
        out.append(f'<line x1="{x - 6}" y1="{ny}" x2="{x + w + 6}" y2="{ny}" stroke="{colour}" stroke-width="2.5"/>')
        out.append(t(x + w + 38, ny + 5, text, 13, colour, weight="bold"))
    return out, floor


def fig_fill(dumps) -> str:
    body, xs = [], (120, 340, 560, 780)
    labels = ("1 · task starts", "2 · in func_a", "3 · in func_b", "4 · in func_c")
    floor = 0
    for k, (x, lab) in enumerate(zip(xs, labels), start=1):
        part, floor = tank(dumps[k], k, x, 60, lab, k == 1)
        body += part
    ly = floor + 34
    for i, (owner, name) in enumerate((("task", "the task"), ("func_a", "func_a"), ("func_b", "func_b"),
                                       ("func_c", "func_c"))):
        fill, stroke = COL[owner]
        lx = 120 + i * 150
        body.append(f'<rect x="{lx}" y="{ly}" width="18" height="14" fill="{fill}" stroke="{stroke}"/>')
        body.append(t(lx + 26, ly + 12, name, 13))
    body.append(f'<rect x="{120 + 600}" y="{ly}" width="18" height="14" fill="url(#stale)" stroke="#b6c0cc"/>')
    body.append(t(120 + 626, ly + 12, "left over, below sp", 13))
    return svg(1010, int(ly + 34), body)


def fig_stain(dumps) -> str:
    body = []
    p, floor = tank(dumps[4], 4, 140, 60, "4 · deepest, in func_c", True)
    body += p
    deepest = min(a for a in range(STACK_LO, STACK_HI, 4) if dumps[5][a] != A5)
    p, _ = tank(dumps[5], 5, 560, 60, "5 · back in the task", False,
                notes=[(deepest, "high-water mark", RED[1])])
    body += p
    body.append(t(760, floor - 120, "The arrays are still there.", 14, INK))
    body.append(t(760, floor - 98, "Popping only moved sp.", 14, MUTED))
    return svg(1010, int(floor + 30), body)


# ---------------------------------------------------------------------------------------------
# Figure: the memory window at step 4, every word, with whose it is

def fig_window(dump) -> str:
    row, x0 = 30, 40
    body = [t(x0, 30, "address", 13, MUTED, font=MONO), t(x0 + 150, 30, "value", 13, MUTED, font=MONO),
            t(x0 + 290, 30, "whose", 13, MUTED), t(x0 + 400, 30, "what", 13, MUTED)]
    y = 44
    n_dry = (0x28007240 - STACK_LO) // 4
    body.append(f'<rect x="{x0 - 10}" y="{y}" width="840" height="{row}" fill="{DRY[0]}"/>')
    body.append(t(x0, y + 20, "0x28007094", 14, MUTED, font=MONO))
    body.append(t(x0 + 150, y + 20, "0xa5a5a5a5", 14, MUTED, font=MONO))
    body.append(t(x0 + 290, y + 20, f"… {n_dry} words like this, up to 0x2800723c: never used yet", 14, MUTED))
    y += row + 6
    for a in range(0x28007240, STACK_HI, 4):
        owner, what = SLOTS[a]
        fill, stroke = COL.get(owner, DRY)
        body.append(f'<rect x="{x0 - 10}" y="{y}" width="840" height="{row}" fill="{fill}" stroke="#ffffff" stroke-width="2"/>')
        bold = "bold" if what.startswith("way back") else "normal"
        body.append(t(x0, y + 20, f"0x{a:08x}", 14, INK, font=MONO))
        body.append(t(x0 + 150, y + 20, f"0x{dump[a]:08x}", 14, INK, font=MONO, weight=bold))
        body.append(t(x0 + 290, y + 20, owner or "—", 14, INK, font=MONO))
        body.append(t(x0 + 400, y + 20, what, 14, INK, weight=bold))
        if a == 0x28007240:
            body.append(t(x0 + 850, y + 20, "← sp", 14, INK, font=MONO, weight="bold"))
        y += row
    return svg(930, y + 16, body)


# ---------------------------------------------------------------------------------------------
# Figure: the overflow, around the stack's first byte

def fig_overflow(dump) -> str:
    row, x0, w = 30, 40, 760
    body, y = [], 20
    hw = ("r0", "r1", "r2", "r3", "r12", "lr", "pc", "xPSR")

    def line(addr, value, label, fill, right="", bold=False):
        nonlocal y
        body.append(f'<rect x="{x0 - 10}" y="{y}" width="{w}" height="{row}" fill="{fill}" stroke="#ffffff" stroke-width="2"/>')
        body.append(t(x0, y + 20, addr, 14, INK, font=MONO))
        body.append(t(x0 + 150, y + 20, value, 14, INK, font=MONO))
        body.append(t(x0 + 300, y + 20, label, 14, INK, weight="bold" if bold else "normal"))
        if right:
            body.append(t(x0 + w, y + 20, right, 14, INK, font=MONO, weight="bold"))
        y += row

    grey = "#e5e7eb"
    assert dump[0x28006FAC] == 0xFFFFFFFD
    line("0x28006f8c", "8 words", "r4–r11, saved by FreeRTOS's switch code", grey)
    line("0x28006fac", f"0x{dump[0x28006FAC]:08x}", "EXC_RETURN", grey)
    for i, r in enumerate(hw):
        a = 0x28006FB0 + 4 * i
        line(f"0x{a:08x}", f"0x{dump[a]:08x}", r + ("   ← saved by the CPU when the switch interrupt hit" if i == 0 else ""), grey)
    a = 0x28006FD0
    line(f"0x{a:08x}", f"0x{dump[a]:08x}", "arr_c[0]", COL["func_c"][0], "← sp in func_c", bold=True)
    n = (STACK_LO - a) // 4 - 1
    last = dump[STACK_LO - 4]
    line("", "⋮", f"arr_c[1] … arr_c[{n}]: {n} more words, up to 0x{last:08x}", COL["func_c"][0])
    body.append(f'<line x1="{x0 - 20}" y1="{y}" x2="{x0 + w + 140}" y2="{y}" stroke="{INK}" stroke-width="3"/>')
    body.append(t(x0 + w + 10, y - 8, "the stack starts here", 13, INK, weight="bold"))
    for i in range(4):
        a = STACK_LO + 4 * i
        line(f"0x{a:08x}", f"0x{dump[a]:08x}", "lowest 16 B: FreeRTOS expects 0xA5 here" if i == 0 else "", RED[0])
    line("", "⋮", "the rest of arr_c, then the frames of func_b, func_a, the task", "#ffffff")
    body.append(t(x0, y + 22, "Above the line: s_demo_spill, the variable that sits below the stack in RAM.", 14, MUTED))
    return svg(x0 + w + 160, y + 40, body)


def fig_hero(dump) -> str:
    """The post's hero (1400x734, the M4/M5 look): the step-4 stack, every word, from the capture."""
    W, H = 1400, 734
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
         '<defs><pattern id="grid" width="28" height="28" patternUnits="userSpaceOnUse">'
         '<path d="M 28 0 L 0 0 0 28" fill="none" stroke="#18292f" stroke-width="1"/></pattern></defs>',
         f'<rect width="{W}" height="{H}" fill="#0f1a1f"/>', f'<rect width="{W}" height="{H}" fill="url(#grid)"/>',
         t(90, 175, "TRAVEO T2G · STACKS 1/2", 17, "#d4a24c", font=MONO),
         t(88, 270, "Inside a stack,", 64, "#f3f1ea", font="Georgia, serif", weight="bold"),
         t(88, 346, "word by word.", 64, "#f3f1ea", font="Georgia, serif", weight="bold"),
         '<rect x="90" y="381" width="96" height="4" fill="#d4a24c"/>',
         t(90, 436, "Three function calls, 80 bytes,", 23, "#9fb3bb", font="Georgia, serif"),
         t(90, 468, "read off the chip with a debugger.", 23, "#9fb3bb", font="Georgia, serif")]
    dark = {"func_c": ("#3a2e14", "#f59e0b", "#f6d39a"), "func_b": ("#14392a", "#22c55e", "#a8e6c6"),
            "func_a": ("#142c45", "#3b82f6", "#a9cdf2"), "task": ("#2a2145", "#8b5cf6", "#cfc2f7")}
    x, y0, rh = 780, 96, 26
    o.append(t(x, y0 - 16, "NODE B · TASK STACK · BREAKPOINT IN func_c", 13, "#3fb6a8", font=MONO))
    y = y0
    groups = []
    for a in range(0x28007240, 0x28007290, 4):
        owner, what = SLOTS[a]
        bg, edge, ink = dark[owner]
        o.append(f'<rect x="{x}" y="{y}" width="330" height="{rh - 3}" fill="{bg}" stroke="{edge}" stroke-width="1"/>')
        o.append(t(x + 12, y + 17, f"{a:08x}", 13, "#7f949c", font=MONO))
        o.append(t(x + 110, y + 17, f"{dump[a]:08x}", 14, ink, font=MONO, weight="bold" if what.startswith("way") else "normal"))
        o.append(t(x + 210, y + 17, "← way back" if what.startswith("way") else "", 12, ink, font=MONO))
        if not groups or groups[-1][0] != owner:
            groups.append([owner, y, y])
        groups[-1][2] = y + rh - 3
        y += rh
    for owner, ya, yb in groups:
        _, edge, ink = dark[owner]
        o.append(f'<path d="M{x + 344},{ya + 2} h8 V{yb - 2} h-8" fill="none" stroke="{edge}" stroke-width="2"/>')
        o.append(t(x + 362, (ya + yb) / 2 + 5, owner, 15, ink, font=MONO, weight="bold"))
    o.append(f'<path d="M{x - 40},{y0} h30" stroke="#f3f1ea" stroke-width="2"/>')
    o.append(t(x - 40, y0 - 6, "sp", 14, "#f3f1ea", font=MONO, weight="bold"))
    o.append(t(x, y + 26, "the stack fills from the bottom up ↑", 13, "#7f949c", font=MONO))
    return "\n".join(o + ["</svg>"]) + "\n"


# Node B's RAM, from the normal image's map file (docs/bench/2026-11-16/stack_demo.md, "Where every stack is").
RAM_TASKS = [("can_task", 0x28005020, "2 KB"), ("log task", 0x280058EC, "512 B"),
             ("timer task", 0x28005BB8, "1 KB"), ("idle task", 0x28006074, "512 B"),
             ("heartbeat", 0x28006330, "512 B")]


def fig_ram_map() -> str:
    """Where every stack lives in Node B's CM7 RAM. Not to scale; lowest address at the top."""
    W, H = 960, 600
    bx, bw = 170, 280                    # the RAM column
    zx, zw = 560, 330                    # the zoom on .bss
    o = [t(bx, 28, "Node B · CM7 RAM", 15, INK, weight="bold"), t(zx, 28, "inside .bss", 15, INK, weight="bold")]

    def band(y, h, label, sub, addr, fill, stroke, dash=False):
        d = ' stroke-dasharray="6 5"' if dash else ""
        o.append(f'<rect x="{bx}" y="{y}" width="{bw}" height="{h}" rx="4" fill="{fill}" stroke="{stroke}" stroke-width="1.2"{d}/>')
        o.append(t(bx + bw / 2, y + h / 2 - 2, label, 16, INK, "middle", weight="bold"))
        o.append(t(bx + bw / 2, y + h / 2 + 20, sub, 14, MUTED, "middle"))
        o.append(t(bx - 14, y + 16, addr, 14, MUTED, "end", MONO))

    band(48, 56, "vectors, .data, .noinit", "", "0x28004000", "#f3f4f6", "#9ca3af")
    band(116, 190, ".bss", "all static variables", "0x28004ad4", "#e0f2f1", "#14b8a6")
    band(318, 150, "free gap, ~860 KB", "the .heap area, unused", "0x280072b4", "#ffffff", "#9ca3af", dash=True)
    band(480, 70, "main stack (MSP), 4 KB", "main() and every interrupt", "0x280df000", "#fee2e2", "#ef4444")
    o.append(t(bx - 14, 566, "0x280e0000", 14, INK, "end", MONO, "bold"))
    o.append(t(bx, 586, "↑ sp starts here at reset and grows up", 14, INK))
    # zoom
    o.append(f'<line x1="{bx + bw}" y1="116" x2="{zx}" y2="48" stroke="#9ca3af" stroke-dasharray="4 4"/>')
    o.append(f'<line x1="{bx + bw}" y1="306" x2="{zx}" y2="408" stroke="#9ca3af" stroke-dasharray="4 4"/>')
    o.append(f'<rect x="{zx}" y="48" width="{zw}" height="360" rx="8" fill="none" stroke="#9ca3af" stroke-dasharray="4 4"/>')
    for i, (name, addr, size) in enumerate(RAM_TASKS):
        y = 64 + i * 66
        o.append(f'<rect x="{zx + 16}" y="{y}" width="{zw - 32}" height="54" rx="4" fill="#dbeafe" stroke="#3b82f6" stroke-width="1.2"/>')
        o.append(t(zx + 30, y + 23, f"{name} stack · {size}", 16, INK, weight="bold"))
        o.append(t(zx + 30, y + 43, f"0x{addr:08x} · then its TCB", 14, MUTED, font=MONO))
    o.append(t(zx, 432, "Each task's stack is just a static array,", 14, MUTED))
    o.append(t(zx, 452, "placed by the linker like any other variable.", 14, MUTED))
    return svg(W, H, o)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    bench, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    d = parse(bench / "stack_demo_capture.txt")      # [before, step1 .. step5]
    assert len(d) == 6, f"expected 6 dumps, got {len(d)}"
    steps = {k: d[k] for k in range(1, 6)}
    ov = parse(bench / "stack_demo_overflow.txt")      # [stop1 spill, stop1 stack, stop2 spill, stop2 stack]
    stop2 = {**ov[2], **ov[3]}
    # fig_fill (four static snapshots) is kept for reuse; the post shows stack-calls.gif instead.
    for name, text in (("stack-window", fig_window(steps[4])),
                       ("stack-stain", fig_stain(steps)), ("stack-overflow", fig_overflow(stop2)),
                       ("hero", fig_hero(steps[4])), ("stack-ram-map", fig_ram_map())):
        (out / f"{name}.svg").write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {out / name}.svg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
