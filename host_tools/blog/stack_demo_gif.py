#!/usr/bin/env python3
"""stack_demo_gif.py -- animate Post 1's stack (a context switch, then three calls) as two GIFs.

    <python with Pillow> host_tools/blog/stack_demo_gif.py docs/bench/2026-11-16 docs/blogs/img/stack-1

Needs Pillow (the GIF writer) and Microsoft Edge (renders each frame, via render_diagrams.shot).
Writes stack-switch.gif and stack-calls.gif; the frame PNGs go to <out>/_gif/ (gitignored).

Every value shown comes from the GDB dumps of Node B's demo task (docs/bench/2026-11-16/):
- gdb_try1_console.txt: the stack as xTaskCreateStatic() left it (the fake frame), before the task ran;
- stack_demo_capture.txt: before the chain (a real switch-out frame), and steps 1 to 5.
The in-between frames only replay what the disassembly says each instruction writes, and each one
is checked against the next real dump (assert below). docs/bench/2026-11-16/stack_demo.md explains
every word.
"""
import pathlib
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import render_diagrams as rd          # noqa: E402  (shot(): headless Edge screenshot)
from stack_demo_figs import A5, MONO, SANS, INK, MUTED, parse, t   # noqa: E402

WIN_LO, WIN_HI = 0x28007230, 0x28007294          # the window drawn: the stack's last 25 words
ROW, X0, Y0, CW = 21, 120, 62, 220
W, H = 960, 640

COL = {"task": ("#ede9fe", "#8b5cf6"), "func_a": ("#dbeafe", "#3b82f6"), "func_b": ("#dcfce7", "#22c55e"),
       "func_c": ("#fff7ed", "#f59e0b"), "cpu": ("#ccfbf1", "#14b8a6"), "rtos": ("#fce7f3", "#ec4899"),
       "dry": ("#f3f4f6", "#d1d5db")}
NAMES = {"cpu": "saved by the CPU", "rtos": "saved by FreeRTOS", "task": "the task", "func_a": "func_a",
         "func_b": "func_b", "func_c": "func_c"}


class Stack:
    """Memory (address -> value) plus who owns each live word, and sp."""

    def __init__(self, mem: dict[int, int], owners: dict[int, str], sp: int):
        self.mem, self.own, self.sp = dict(mem), dict(owners), sp

    def write(self, owner: str, words: dict[int, int]) -> set[int]:
        for a, v in words.items():
            self.mem[a] = v
            self.own[a] = owner
        return set(words)

    def claim(self, owner: str, lo: int, hi: int) -> None:
        for a in range(lo, hi, 4):
            self.own[a] = owner

    def drop_below(self, sp: int) -> None:
        self.sp = sp
        for a in list(self.own):
            if a < sp:
                del self.own[a]


def frame_svg(st: Stack, new: set[int], n: int, total: int, title: str, lines: list[str], code: str,
              legend: list[str]) -> str:
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">',
         '<defs><pattern id="stale" width="8" height="8" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
         '<rect width="8" height="8" fill="#eef1f5"/><line x1="0" y1="0" x2="0" y2="8" stroke="#b6c0cc" stroke-width="3"/>'
         '</pattern><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
         f'<path d="M0,0 L10,5 L0,10 z" fill="{INK}"/></marker></defs>',
         f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
         t(X0, 34, "Node B · the demo task's stack (last 25 of 128 words)", 13, MUTED)]
    for i, a in enumerate(range(WIN_LO, WIN_HI, 4)):
        y = Y0 + i * ROW
        v = st.mem[a]
        if a in st.own:
            fill, stroke = COL[st.own[a]]
            o.append(f'<rect x="{X0}" y="{y}" width="{CW}" height="{ROW}" fill="{fill}" stroke="{stroke}" stroke-width="0.8"/>')
        elif v != A5:
            o.append(f'<rect x="{X0}" y="{y}" width="{CW}" height="{ROW}" fill="url(#stale)" stroke="#b6c0cc" stroke-width="0.6"/>')
        else:
            o.append(f'<rect x="{X0}" y="{y}" width="{CW}" height="{ROW}" fill="{COL["dry"][0]}" stroke="{COL["dry"][1]}" stroke-width="0.6"/>')
        if a in new:
            o.append(f'<rect x="{X0 + 1.5}" y="{y + 1.5}" width="{CW - 3}" height="{ROW - 3}" fill="none" stroke="#eab308" stroke-width="3"/>')
        live = a in st.own or v != A5
        o.append(t(X0 + CW / 2, y + 15, f"{v:08x}", 13, INK if live else "#b0b5bc", "middle", MONO,
                   "bold" if a in new else "normal"))
        o.append(t(X0 - 10, y + 15, f"{a:08x}", 12, MUTED, "end", MONO))
    floor = Y0 + ((WIN_HI - WIN_LO) // 4) * ROW
    o.append(f'<path d="M{X0 - 2},{Y0 - 6} L{X0 - 2},{floor + 2} L{X0 + CW + 2},{floor + 2} L{X0 + CW + 2},{Y0 - 6}" '
             f'fill="none" stroke="{INK}" stroke-width="3" stroke-linejoin="round"/>')
    o.append(t(X0 + CW / 2, Y0 - 12, "⋮ 103 more words of 0xa5a5a5a5", 12, MUTED, "middle", MONO))
    sy = Y0 + ((st.sp - WIN_LO) // 4) * ROW
    o.append(f'<line x1="{X0 + CW + 44}" y1="{sy}" x2="{X0 + CW + 6}" y2="{sy}" stroke="{INK}" stroke-width="2.5" marker-end="url(#arr)"/>')
    o.append(t(X0 + CW + 50, sy + 5, f"sp = 0x{st.sp:08x}", 14, INK, font=MONO, weight="bold"))
    # the story panel
    px = 560
    o.append(t(px, 92, f"{n} / {total}", 14, MUTED, font=MONO))
    o.append(t(px, 126, title, 22, INK, weight="bold"))
    for k, line in enumerate(lines):
        o.append(t(px, 162 + 26 * k, line, 16, INK))
    if code:
        cy = 162 + 26 * len(lines) + 18
        o.append(f'<rect x="{px - 10}" y="{cy}" width="{W - px - 20}" height="{24 * code.count(chr(10)) + 36}" rx="4" fill="#f4f4f5"/>')
        for k, cl in enumerate(code.split("\n")):
            o.append(t(px + 4, cy + 24 + 24 * k, cl, 15, INK, font=MONO))
    ly = floor - 24 * len(legend) + 10
    for k, key in enumerate(legend):
        y = ly + 24 * k
        if key == "stale":
            o.append(f'<rect x="{px}" y="{y - 12}" width="18" height="14" fill="url(#stale)" stroke="#b6c0cc"/>')
            o.append(t(px + 28, y, "left over: below sp, not erased", 14))
        elif key == "new":
            o.append(f'<rect x="{px}" y="{y - 12}" width="18" height="14" fill="none" stroke="#eab308" stroke-width="3"/>')
            o.append(t(px + 28, y, "written in this step", 14))
        else:
            fill, stroke = COL[key]
            o.append(f'<rect x="{px}" y="{y - 12}" width="18" height="14" fill="{fill}" stroke="{stroke}"/>')
            o.append(t(px + 28, y, NAMES[key], 14))
    return "\n".join(o + ["</svg>"]) + "\n"


def same(st: Stack, dump: dict[int, int], what: str) -> None:
    """A replayed frame must match the real dump word for word, over the whole window."""
    bad = [a for a in range(WIN_LO, WIN_HI, 4) if st.mem[a] != dump[a]]
    assert not bad, f"{what}: differs from the capture at " + ", ".join(f"0x{a:08x}" for a in bad)


def switch_frames(created: dict[int, int], before: dict[int, int]) -> list[tuple]:
    leg = ["cpu", "rtos", "task", "stale", "new"]
    F = []
    st = Stack(created, {}, 0x2800724C)
    same(st, created, "created")
    st.claim("rtos", 0x2800724C, 0x28007270)
    st.claim("cpu", 0x28007270, 0x28007290)
    F.append((st, set(), "The task is created",
              ["Before it ever runs, FreeRTOS writes a frame", "that looks like a task switched out.",
               "pc = 0x10080f34: the task's first instruction."], "TCB.sp = 0x2800724c", leg))
    st = Stack(st.mem, st.own, st.sp); st.drop_below(0x28007270)
    F.append((st, set(), "First switch-in, part 1",
              ["FreeRTOS pops r4–r11 and EXC_RETURN.", "The words stay in RAM: only sp moves."], "", leg))
    st = Stack(st.mem, st.own, st.sp); st.drop_below(0x28007290)
    F.append((st, set(), "First switch-in, part 2",
              ["The CPU pops r0–r3, r12, lr, pc, xPSR.", "pc = 0x10080f34: the task starts running."], "", leg))
    st = Stack(st.mem, st.own, st.sp)
    new = st.write("task", {0x28007288: before[0x28007288], 0x2800728C: before[0x2800728C]})
    st.drop_below(0x28007288)
    F.append((st, new, "The task saves its way back",
              ["Its first instruction pushes 2 registers.", "lr = 0x100810bd: prvTaskExitError."], "push {r3, lr}", leg))
    st = Stack(st.mem, st.own, st.sp)
    new = st.write("cpu", {a: before[a] for a in range(0x28007268, 0x28007288, 4)})
    st.drop_below(0x28007268)
    F.append((st, new, "Switched out: the CPU saves 8",
              ["A tick interrupt hits while the task waits.", "The CPU pushes r0–r3, r12, lr, pc, xPSR",
               "onto the task's own stack. pc = 0x10080f38."], "", leg))
    st = Stack(st.mem, st.own, st.sp)
    new = st.write("rtos", {a: before[a] for a in range(0x28007244, 0x28007268, 4)})
    st.drop_below(0x28007244)
    same(st, before, "switched out")
    F.append((st, new, "Switched out: FreeRTOS saves 9",
              ["The switch code pushes r4–r11 and", "EXC_RETURN, then stores sp in the TCB.",
               "Another task runs now."], "TCB.sp = 0x28007244", leg))
    st = Stack(st.mem, st.own, st.sp); st.drop_below(0x28007288)
    F.append((st, set(), "Switched back in",
              ["Everything is popped again: sp is back.", "The saved registers stay behind as",
               "leftovers until something writes over them."], "", leg))
    return F


def call_frames(step: dict[int, dict[int, int]]) -> list[tuple]:
    leg = ["task", "func_a", "func_b", "func_c", "stale", "new"]
    F = []
    st = Stack(step[1], {0x28007288: "task", 0x2800728C: "task"}, 0x28007288)
    F.append((st, set(), "The task is about to call", ["It has used 8 bytes so far.", "Below sp: leftovers of a task switch."],
              "bl func_a", leg))

    def call(st, owner, saved, sub_lo, sp_after_push, array, nxt, title, code_push, code_sub):
        st = Stack(st.mem, st.own, st.sp)
        new = st.write(owner, {a: nxt[a] for a in saved})
        st.drop_below(sp_after_push)
        F.append((st, new, f"{title}: push", ["Save the way back (lr) and the registers", "it will borrow."],
                  code_push, leg))
        st = Stack(st.mem, st.own, st.sp)
        st.claim(owner, sub_lo, sp_after_push)
        st.drop_below(sub_lo)
        F.append((st, set(), f"{title}: make room", ["Move sp up for the local array.",
                                                       "Nothing is written yet: old values stay."], code_sub, leg))
        st = Stack(st.mem, st.own, st.sp)
        new = st.write(owner, {a: nxt[a] for a in array})
        same(st, nxt, title)
        F.append((st, new, f"{title}: fill the array", ["The local array is now on the stack.", "(The padding word is never written.)"], "", leg))
        return st

    st = call(st, "func_a", (0x2800727C, 0x28007280, 0x28007284), 0x28007270, 0x2800727C,
              (0x28007270, 0x28007274), step[2], "func_a", "push {r4, r5, lr}", "sub sp, #12")
    st = call(st, "func_b", (0x28007264, 0x28007268, 0x2800726C), 0x28007258, 0x28007264,
              (0x28007258, 0x2800725C), step[3], "func_b", "push {r4, r5, lr}", "sub sp, #12")
    st = call(st, "func_c", (0x28007254,), 0x28007240, 0x28007254,
              (0x28007240, 0x28007244, 0x28007248, 0x2800724C), step[4], "func_c", "push {lr}",
              "sub sp, #20")
    F[-1] = (F[-1][0], F[-1][1], "func_c: the deepest point", ["80 bytes in use.", "Every frame holds its way back."],
             "", leg)
    # returns: a helper's push (stack_demo_mark / stack_demo_keep) also lands below sp each time; its
    # value is replayed so the last frame equals the step-5 dump, but not highlighted
    st = Stack(st.mem, st.own, st.sp)
    st.mem[0x2800723C] = step[5][0x2800723C]
    st.drop_below(0x28007258)
    F.append((st, set(), "func_c returns", ["add sp · pop the way back into pc.", "Its words are not erased."],
              "ldr pc, [sp], #4", leg))
    st = Stack(st.mem, st.own, st.sp)
    st.mem[0x28007254] = step[5][0x28007254]
    st.drop_below(0x28007270)
    F.append((st, set(), "func_b returns", ["Same again: only sp moves back."], "pop {r4, r5, pc}", leg))
    st = Stack(st.mem, st.own, st.sp)
    st.mem[0x2800726C] = step[5][0x2800726C]
    st.drop_below(0x28007288)
    same(st, step[5], "back in the task")
    F.append((st, set(), "Back in the task", ["sp is where it started.", "The arrays are still in RAM:",
                                                     "that stain is the high-water mark."], "pop {r4, r5, pc}", leg))
    return F


def build(frames: list[tuple], out: pathlib.Path, name: str, pil) -> None:
    work = out / "_gif" / name
    work.mkdir(parents=True, exist_ok=True)
    profile = out / "_edge-profile"
    imgs = []
    for i, (st, new, title, lines, code, leg) in enumerate(frames, start=1):
        svg = work / f"f{i:02d}.svg"
        svg.write_text(frame_svg(st, new, i, len(frames), title, lines, code, leg), encoding="utf-8", newline="\n")
        ET.fromstring(svg.read_text(encoding="utf-8"))   # a broken frame would bake a browser error page into the GIF
        png = work / f"f{i:02d}.png"
        png.unlink(missing_ok=True)
        rd.shot(svg, png, W, H, profile)
        im = pil.open(png).convert("RGB").resize((W, H), pil.LANCZOS)
        imgs.append(im.quantize(colors=96, method=pil.Quantize.MEDIANCUT, dither=pil.Dither.NONE))
    durations = [1800] * len(imgs)
    durations[0] = durations[-1] = 3200
    gif = out / f"{name}.gif"
    imgs[0].save(gif, save_all=True, append_images=imgs[1:], duration=durations, loop=0, optimize=True)
    print(f"{gif}: {len(imgs)} frames, {gif.stat().st_size // 1024} KB")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    from PIL import Image as pil
    bench, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]).resolve()
    created = parse(bench / "gdb_try1_console.txt")[0]
    d = parse(bench / "stack_demo_capture.txt")       # [before, step1 .. step5]
    build(switch_frames(created, d[0]), out, "stack-switch", pil)
    build(call_frames({k: d[k] for k in range(1, 6)}), out, "stack-calls", pil)
    return 0


if __name__ == "__main__":
    sys.exit(main())
