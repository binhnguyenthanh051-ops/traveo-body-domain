# Post 2 evidence — 2026-11-17 (host only)

Inputs for Post 2's GIFs and numbers. Run by Claude on the host, no board.

**Image:** Node B `actuator_cm7.elf`, sha256 `10b2b3d0577f…` (the normal image flashed back on 2026-11-16),
with its `.su`/`.ci` files. **Tool:** stack_bound 0.2.0 (`tools/stack_bound`), project file
`node_b_actuator/proj_cm7/stack_bound.toml`.

| File | What | How |
|---|---|---|
| `stack_bound_nodeB.txt`, `.json` | The report: can_task 1464 B of 2048 B (71 %), heartbeat 392/512 (77 %), log 308/512 (60 %), exit 0 | `python -m stack_bound … --json` |
| `can_task_walk.json` | The tree the tool walks for can_task: every node on the worst path with all its callees, own frame and depth | `host_tools/blog/stack_bound_walk.py` (stack_bound as a library; asserts its worst path and 1256 B equal the tool's) |
| `refusal_nobinding.txt` | verify_core's `[[indirect]]` entry deleted: "indirect call at secoc.c:139:10 has no binding", exit 2 | same run, edited copy of the project file |
| `refusal_wrongtarget.txt` | bound to `secoc_rx_process` instead: "never address-taken, so no pointer can hold it", exit 2 | same |

**The function pointer** (`arm-none-eabi-nm -S`, `objdump -d`, `objdump -s` on the same image):

```
10086128 000000ec T crypto_mac
10087eb0 00000004 t g_oracle
1008689e:  bl   100867c4 <build_mac_input>
100868a4:  ldr    r3, [r7, #0]
100868a6:  ldr.w  fp, [r3]
100868b0:  blx    fp
 10087ea0 80000000 00010e28 b07e0810 5e000000   words: 0x00000080 0x280e0100 0x10087eb0 0x0000005e
 10087eb0 29610810 06000000 00002240 02030405   words: 0x10086129 0x00000006 0x40220000 0x05040302
```

`0x10086129` = `crypto_mac` + 1 (Thumb bit): the only `secoc_mac_if_t` in the image, `g_oracle = { crypto_mac }`.
`0x10087eb0` (the word at `0x10087ea8`) is `g_oracle`'s own address: data, even, not code.

Same numbers as the 2026-11-11/12 images (`docs/bench/2026-11-14/stack_bound_asm_examples.md`).
