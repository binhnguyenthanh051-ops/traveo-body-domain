#!/usr/bin/env python3
"""stack_bound_walk.py -- dump the call tree stack_bound walks for one task, for Post 2's GIF.

    python host_tools/blog/stack_bound_walk.py \
        node_b_actuator/proj_cm7/build/APP_KIT_T2G-B-H_LITE/Debug/actuator_cm7.elf \
        node_b_actuator/proj_cm7/build node_b_actuator/proj_cm7/stack_bound.toml can_task \
        docs/bench/2026-11-17/can_task_walk.json

Uses stack_bound itself (tools/stack_bound/src, as a library, read-only): the same frames, the same graph
and the same [[indirect]] bindings as a normal run. Writes, for the worst path, every node's callees with
their own frame and their depth (frame + deepest callee). The GIF shows that tree being walked; the worst
path and its total must equal what the tool reports (checked below).
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "stack_bound" / "src"))
from stack_bound import analyze, elfread, gcc, project   # noqa: E402


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__)
        return 2
    elf, su, proj_file, entry, out = sys.argv[1:]
    proj = project.load(pathlib.Path(proj_file))
    image = elfread.load(pathlib.Path(elf))
    witness = gcc.load_dir(pathlib.Path(su))
    analyze.attach_frames(image, witness, proj)
    g = analyze.build_graph(image, witness, proj)

    memo: dict[int, int] = {}

    def callees(addr: int) -> list[tuple[int, str]]:
        out = list(g.calls.get(addr, []))
        if addr in g.listed:
            out += [(t, "listed") for t in g.listed[addr][0]]
        return out

    def depth(addr: int, stack: tuple[int, ...] = ()) -> int:
        if addr in memo:
            return memo[addr]
        if addr in stack:
            raise SystemExit(f"recursion at {image.functions[addr].name}")
        fn = image.functions[addr]
        assert fn.frame is not None, f"{fn.name}: no frame"
        best = max((depth(c, stack + (addr,)) for c, _ in callees(addr)), default=0)
        memo[addr] = fn.frame + best
        return memo[addr]

    root = image.by_name(entry)[0]
    total = depth(root.addr)
    levels, node = [], root.addr
    while True:
        kids = sorted({c: v for c, v in callees(node)}.items(), key=lambda kv: -depth(kv[0]))
        fn = image.functions[node]
        levels.append({"name": fn.name, "frame": fn.frame, "depth": depth(node),
                       "indirect_sites": g.sites.get(node, []),
                       "callees": [{"name": image.functions[c].name, "frame": image.functions[c].frame,
                                    "depth": depth(c), "via": v} for c, v in kids]})
        if not kids:
            break
        node = kids[0][0]
    # cross-check against the tool's own answer
    result = analyze.check_task(image, g, proj, next(t for t in proj.tasks if t.entry == entry))
    assert result.path_bytes == total, (result.path_bytes, total)
    assert [s.fn.name for s in result.path] == [lv["name"] for lv in levels]
    json.dump({"image_sha256": image.sha256 if hasattr(image, "sha256") else None, "entry": entry,
               "path_bytes": total, "switch_bytes": result.switch_bytes, "required": result.required,
               "configured": result.configured, "levels": levels}, open(out, "w", encoding="utf-8"), indent=1)
    print(f"{entry}: {total} B over {len(levels)} levels -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
