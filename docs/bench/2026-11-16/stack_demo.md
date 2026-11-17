# Stack demo on Node B — 2026-11-16 (silicon, GDB)

Evidence for the stack blog post, Part VI: a real task stack, word by word, through a call chain, and a
real overflow. Run by Claude on the bench (owner's OK for GDB attach, 2026-11-16: halt, breakpoints, one
RAM write `g_stack_demo_go = 1`; no flash writes besides the normal `program`). Node B only.

## Images

| Image | Switch | `actuator_cm7.elf` sha256 | Built from |
|---|---|---|---|
| demo 1 | `STACK_DEMO=1` | `1a73feae1205…` | `main` firmware (`git diff main HEAD -- node_b_actuator shared` empty) + uncommitted `src/stack_demo.c/.h`, `main.c`, `Makefile` |
| demo 2 | `STACK_DEMO=2` | `8d4e7914a1d3…` | same |
| normal | none | `10b2b3d0577f…` | same; `nm` finds no `stack_demo` symbol |

MTB GCC 14.2.1, `-Og`. Flashed with `bash ./zz_build_actuator_app.sh program all [STACK_DEMO=n]` on probe
`1C09043C03272400` (bench.env): `flash_demo1.log`, `flash_demo2.log`, `flash_normal.log`, each `** Verified OK **`.

## Sequence

| # | Action | Boots on COM6 |
|---|---|---|
| 1 | flash demo 1 | boot 1 |
| 2 | OpenOCD GDB server, kit default (auto-acquire): XRES + test-mode acquire, app stopped (`openocd_try1_acquire.log`, `gdb_try1_console.txt`) | — |
| 3 | `reset_node.sh B` (debug reset) | boot 2 |
| 4 | `gdbserver_node.sh B` with `ENABLE_ACQUIRE 0` (attach, no reset); `stack_demo_capture.gdb` → `stack_demo_capture.txt` | — |
| 5 | flash demo 2 | boot 3 |
| 6 | GDB server; `stack_demo_overflow.gdb` → `stack_demo_overflow.txt` | — |
| 7 | flash normal | boot 4 |

`bench_summary.py nodeB.bin`: 4 boots, 1 banner each, all `reset_reason=0x10010 (XRES | SOFT)` (programmer /
debug resets, not the reset button), 0 lost records. `nodeB.txt` is the folded transcript.

## Check 1 — the stack, word by word (demo 1): PASS

The demo task's stack is `s_demo_stack`, **`0x28007094 … 0x28007293`** (128 words, 512 B);
`s_demo_spill` (512 B) sits right below it at `0x28006e94`, `s_demo_tcb` right above at `0x28007294`.
Breakpoint on the **first instruction** of `stack_demo_mark` (before it pushes its own r4, r5).

`sp` at each stop (`info registers`), and `bt`:

| Step | Where | `sp` | Used from the top (`0x28007290`) | `bt` |
|---|---|---|---|---|
| 1 | task, before `func_a` | `0x28007288` | 8 B | mark ← stack_demo_task ← prvTaskExitError |
| 2 | in `func_a` | `0x28007270` | 32 B | + func_a (x=17) |
| 3 | in `func_b` | `0x28007258` | 56 B | + func_b (x=34) |
| 4 | in `func_c` | `0x28007240` | **80 B** | + func_c (x=51) |
| 5 | task, after the calls | `0x28007288` | 8 B | as step 1 |

**Step 4, every non-0xA5 word** (`stack_demo_capture.txt`, step=4), lowest address first. Frames from the
disassembly of this image (`func_a`/`func_b`: `push {r4, r5, lr}` + `sub sp, #12`; `func_c`: `push {lr}` +
`sub sp, #20`; `stack_demo_task`: `push {r3, lr}`):

```
address     value        whose            what
0x28007240  0xcccc0001   func_c           arr_c[0]                       ← sp
0x28007244  0xcccc0002   func_c           arr_c[1]
0x28007248  0xcccc0003   func_c           arr_c[2]
0x2800724c  0x00000033   func_c           arr_c[3] = x (0x33)
0x28007250  0xa5a5a5a5   func_c           padding (sub sp, #20 keeps sp 8-aligned)
0x28007254  0x10080eef   func_c           saved lr → back into func_b
0x28007258  0xbbbb0001   func_b           arr_b[0]
0x2800725c  0xbbbb0002   func_b           arr_b[1]
0x28007260  0xa5a5a5a5   func_b           padding
0x28007264  0xa5a5a5a5   func_b           saved r4 (func_a's r4: never set, still the fake frame's 0xA5)
0x28007268  0x00000011   func_b           saved r5 = func_a's x (func_a keeps x in r5)
0x2800726c  0x10080f23   func_b           saved lr → back into func_a
0x28007270  0xaaaa0001   func_a           arr_a[0]
0x28007274  0xaaaa0002   func_a           arr_a[1]
0x28007278  0xa5a5a5a5   func_a           padding
0x2800727c  0xa5a5a5a5   func_a           saved r4 (task's)
0x28007280  0xa5a5a5a5   func_a           saved r5 (task's)
0x28007284  0x10080f4f   func_a           saved lr → back into stack_demo_task
0x28007288  0xa5a5a5a5   stack_demo_task  saved r3 (pushed only to keep sp 8-aligned)
0x2800728c  0x100810bd   stack_demo_task  saved lr = prvTaskExitError | 1 (FreeRTOS's fake return address)
0x28007290  0xa5a5a5a5   —                never used (alignment)
```

Everything from `0x28007094` to `0x2800723f` (428 B) is still `0xa5a5a5a5`. Return addresses resolve with
`addr2line`: `0x10080eee` func_b (stack_demo.c:112), `0x10080f22` func_a (:121), `0x10080f4e`
stack_demo_task (:136), `0x100810bc` prvTaskExitError (port.c:233). Arguments `x` never touch the stack:
they travel in `r0` (`bt` shows x=17/34/51 = 0x11/0x22/0x33).

**Before the chain ran** (task waiting in its loop), the words `0x28007264 … 0x28007287` held a context
frame from a task switch: `EXC_RETURN 0xfffffffd`, r0 `0`, r3 `0x28006e90` (&g_stack_demo_go), lr
`0x100810bd`, pc `0x10080f38` (the wait loop), xPSR `0x61000000`, r4–r11 still `0xA5`. The chain later
wrote over it.

**Step 5, the stain.** After all three returned, `sp` is back at `0x28007288`, but `arr_c`, `arr_b`,
`arr_a`, the `0x33` and the saved `0x11` are all still in memory. Three words changed after step 4, each
from a later push into the popped area: `0x28007254` and `0x2800726c` (the saved lr slots) now `0xA5`,
written by `stack_demo_keep`'s `push {r4}` (r4 = 0xA5) in func_b and func_a; `0x2800723c` now `0x22`,
written by `stack_demo_mark`'s `push {r4, r5}` at step 4 (r5 = func_b's x). The deepest non-0xA5 word is
`0x2800723c`: the high-water mark would count everything below it as never used.

## Check 2 — the overflow (demo 2): PASS

`func_c` reserves 644 B (`sub.w sp, sp, #644`) for a 640 B array in a 512 B stack.

- **Stop 1** (step 4, array filled, before the task switch): `sp = psp = 0x28006fd0` = `s_demo_spill+316`,
  **196 B below the stack's first byte**. `arr_c[0]` = `0xcccc0001` at `0x28006fd0`; the stack's own lowest
  16 B (`0x28007094`) hold `0xcccc0032 0xcccc0033 0xcccc0034 0xcccc0035` (same at stop 2).
- **Stop 2**: `vApplicationStackOverflowHook (xTask=0x28007294 <s_demo_tcb>, pcTaskName="stackdemo")`
  ← `vTaskSwitchContext` (tasks.c:3132) ← `PendSV_Handler` (port.c:473). Running on MSP (`0x280dffd0`);
  `psp = 0x28006fb0`.
- The switch frame landed in `s_demo_spill`: hardware frame at `0x28006fb0` (r0 `4`, r1 `0x28006fd0`,
  r2 `0x10000000`, r3 `0xe000e000`, r12 `0xA5`, lr `0x10080ebd`, pc `0x10080ecc` in func_c, xPSR
  `0x61000000`), `EXC_RETURN 0xfffffffd` at `0x28006fac`, r4–r11 below it. The overwrite of the variable
  below the stack happened **before** FreeRTOS's check could see it.

## Where every stack is (normal image, map file)

Normal image flashed at the end of the session (`actuator_cm7.elf` sha256 `10b2b3d0577f…`), its
`actuator_cm7.map` (build dir, 2026-11-16 21:02) and `arm-none-eabi-nm -S -n`:

```
.data               0x28004080     0xc0                 (map line 9706)
.bss                0x28004ad4   0x27e0                 (9803)
.bss.s_stack        0x28005020    0x800  can_task.o     CAN task stack      (9829)
.bss.s_tcb          0x28005820     0xbc  can_task.o     its TCB             (9830)
.bss.s_log_stack    0x280058ec    0x200  log_task.o     log task            (9843)
.bss.s_timer_stack  0x28005bb8    0x400  main.o         FreeRTOS timer task (9858)
.bss.s_idle_stack   0x28006074    0x200  main.o         FreeRTOS idle task  (9862)
.bss.s_hb_stack     0x28006330    0x200  main.o         heartbeat task      (9866)
.heap               0x280072b4  0xd7d4c                 newlib malloc area, unused (10007)
__StackTop   = 0x280e0000  (ORIGIN(ram) + LENGTH(ram))  (10018)
__StackLimit = 0x280df000  (__StackTop - STACK_SIZE)    (10019); STACK_SIZE = 0x1000 (linker.ld:45)
```

Vector table word 0 (`__Vectors` at `0x10080000`) = `0x280e0000`: the initial MSP. No `ucHeap` symbol in the
image: the FreeRTOS heap (`configTOTAL_HEAP_SIZE`) is not linked. In the `STACK_DEMO` images the demo task's
`s_demo_stack` (`0x28007094`) is in `.bss` too, after the heartbeat's TCB.

## Seen on the way

- **OpenOCD's kit default resets the chip.** `kitprog3` + `cyt4bf.cfg` start with "Auto-acquire enabled …
  acquiring device in Test Mode using … 'xres' reset", which stops the app (the log went silent ~10 s after
  boot 1; CM7 halted in boot ROM at `pc 0x1e6`). `tools/bench/gdbserver_node.sh` now sets
  `ENABLE_ACQUIRE 0` and attaches to the running app.
- `stack_demo_mark` and `stack_demo_keep` call nothing but still push (`push {r4, r5}` / `push {r4}`): that
  is why the breakpoint is on the first instruction, and it explains step 5's three changed words.
- GDB can't name `0x100810bc` in `bt` (`??`); `nm`/`addr2line` give `prvTaskExitError`.
- The first try's dump (`gdb_try1_console.txt`, taken after the acquire reset) shows the demo stack exactly as
  `xTaskCreateStatic()` writes it, before the task ran: the 17-word fake frame at `0x2800724c … 0x2800728f`
  (`EXC_RETURN 0xfffffffd`, r0 `0`, lr `0x100810bd`, pc `0x10080f34` = `stack_demo_task`, xPSR `0x01000000`),
  r4–r11 and r1–r3/r12 still `0xA5`. The post's switch GIF starts from it.
