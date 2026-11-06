# ADR-0025: Static worst-case stack analysis — the stack drill (W46)

**Status:** proposed · **Date:** 2026-11-06 (spike pulled forward from Wed 11)

> How each FreeRTOS task's stack size is **checked at build time**, not guessed and then
> watched at runtime. A host tool (`host_tools/stack_drill.py`) reads GCC's own per-function
> frame sizes and call graph, walks from each task entry, adds the context-switch frame, and
> compares the result with the task's `*_STACK_WORDS`. This ADR fixes the method, what "worst
> case" means, the annotation file, the margin rule, and how the bench cross-checks it.

## Context

- **What went wrong (W40, 2026-09-28):** Node B's `can` task overflowed on its first
  `crypto_mac()`, whose frame alone is over 500 B (two `crypto_msg_t` plus two wire buffers).
  The stack had been sized from a runtime high-water taken *before* that path existed. A 30-line
  prototype found the 856 B worst case against the 768 B stack in seconds, with no debugger.
- **How stacks are sized today:** a runtime high-water mark (`uxTaskGetStackHighWaterMark`),
  plus hand notes next to each `#define` ("crypto_mac() alone needs ~540 B"). A high-water mark
  only measures paths that actually ran. A failure path that never ran on the bench is
  invisible, and failure paths are exactly where the extra frames live (F-018 added one today).
- **What the bench can see today:** Node A's `LOG_EVT_APP_ALIVE` carries the **health** task's
  high-water. `g_hw_app` / `g_hw_can` exist only in RAM, for a debugger. Node B's carries only
  the **heartbeat** task's. **Neither node's `can` task, the one that overflowed, is observable
  from the normal image** (CLAUDE.md rule 7).
- **Constraints:** no change to the shipped code for the sake of analysis; host tool in Python
  3.11 with `pytest` (CLAUDE.md); CI cannot build firmware, so the real gate is local; MTB GCC
  14.2.1 on both nodes.

### The spike (2026-11-06, Node B `proj_cm7`, MTB GCC 14.2.1)

| Check | Result |
|---|---|
| Build with `CFLAGS="-fstack-usage -fcallgraph-info=su"` | **Code bytes identical** to the normal build: `objcopy -O binary` SHA-256 equal, and the rebuilt normal `.hex` byte-identical to the one taken before the spike |
| Output | 215 `.su` + 215 `.ci` files (every compiled unit, including PDL and FreeRTOS) |
| Frames | All 2,326 **static** (no `dynamic` / `bounded` qualifiers, so no VLAs or `alloca` anywhere in the image) |
| Calls with no data | `memcpy`, `memset`: newlib, prebuilt, no `.su` |
| Indirect calls (`__indirect_call` in `.ci`) | 7 functions: the SecOC oracle call sites (`verify_core`, `secoc_secure`, `secoc_rx_process`, `secoc_rx_accept`, `secoc_tx_next`, `secoc_tx_adopt_floor`) and `ipc_transact` (the port's function table) |
| `can_task`, call graph as GCC sees it | **552 B**: the walk stops at the oracle function pointer, so `crypto_mac()` is **not counted** |
| `can_task`, oracle annotated → `crypto_mac` | **1256 B**: `can_task 168 → secoc_app_verify_and_decode 104 → secoc_rx_process 64 → secoc_verify_len 56 → verify_core 136 → crypto_mac 568 → crypto_report 8 → log_evt 80 → log_try_enqueue 48 → log_ring_copy_in 24` |
| `heartbeat_task` | 168 B (`→ log_evt → log_try_enqueue → log_ring_copy_in`) |

Two lessons came straight out of the spike:

1. **The unannotated answer is dangerously wrong.** It reports 552 B for a task whose real
   worst path is 1256 B, because the deepest frame sits behind a function pointer. A tool that
   silently drops indirect calls would have passed the W40 overflow. Indirect calls must
   **fail the run** until annotated (D3).
2. **Failure paths are the deep paths.** The worst path ends in this morning's F-018 edge
   (`crypto_mac → crypto_report → log_evt`): +160 B below `crypto_mac`, on a path the bench
   only hits when the M0+ fails.

## Decisions

### D1. Frame sizes and call graph come from GCC (`-fstack-usage -fcallgraph-info=su`); disassembly is the fallback

GCC knows each function's frame exactly, after optimisation and inlining, and says whether it
is static. `.ci` gives its call graph for the same build, including the calls it could *not*
resolve (`__indirect_call`). The tool walks that graph. For the few objects with no `.su`
(newlib's `memcpy`/`memset`, startup assembly), the frame comes from the annotation file (D3),
where an entry may cite a disassembly measurement (`push`/`vpush`/`sub sp` in `objdump -d`).

**The flags go in permanently,** in `CFLAGS` of every app project (Node A `app`, Node B
`proj_cm7`), so the data is always current with the image. The spike shows the code bytes don't
change. Re-check that on any toolchain bump (`objcopy -O binary` equal with and without the
flags). It's a one-line part of the `bench-test` build check, not a reason to keep a separate
build.

### D2. Worst case = deepest path from the task entry **+ 204 B for the context-switch frame**

On the M4F (Node A) and M7 (Node B), with an FPU-using task, being switched out stores this on
the **task's own stack (PSP)**:

| Pushed by | What | Bytes |
|---|---|---|
| Hardware, on exception entry | the extended frame: `r0–r3, r12, lr, pc, xPSR` + `s0–s15, FPSCR` + 1 reserved word (lazy stacking reserves the space) | 104 |
| FreeRTOS `xPortPendSVHandler`, CM4F/CM7 GCC port | `vstmdbeq r0!, {s16-s31}` (only if the task used the FPU) | 64 |
| FreeRTOS `xPortPendSVHandler` | `stmdb r0!, {r4-r11, r14}` | 36 |
| **Total, per task** | | **204** |

It is counted **once**. An ISR that preempts the task stacks the same 104 B hardware frame,
and PendSV then tail-chains on that frame rather than adding another. Nested interrupts run in
handler mode on the **main stack (MSP)**, not the task's. This corrects two earlier figures:
the stack-drill idea's "up to 104 B" (the hardware part only) and ADR-0010's "~136 B" (the FP
registers only). ADR-0010 is amended by reference, not edited.

The **MSP budget** (ISRs, nested) is a separate number with a separate entry point (the vector
table). It is out of scope for W46 and listed under Open items.

### D3. What GCC can't see goes in one annotation file, and anything un-annotated fails the run

`host_tools/stack_drill/annotations.toml` (read with the standard library's `tomllib`). Per node:

- **`[tasks]`:** entry function, the stack symbol or `*_STACK_WORDS` value, and whether the task
  uses the FPU. A task that never touches it switches out with 68 B (the 32 B basic frame + 36 B) instead of 204 B. The default is "uses it".
- **`[indirect]`:** for each function with an `__indirect_call`, the possible targets. On both
  nodes the SecOC call sites → `crypto_mac`, and `ipc_transact` → that node's `ipc_port_if_t`
  members (`port_*` in `port_crypto.c`). Each entry gives a **reason** (which table or
  struct binds the pointer).
- **`[frames]`:** fixed frames for functions with no `.su` (`memcpy`, `memset`), each with its
  source (a disassembly read, or the library's documented frame).
- **`[ignore]`:** calls that provably never happen on a task stack (none yet). Each needs a
  reason.

An unresolved call (no `.su`, no annotation), an `__indirect_call` site with no `[indirect]`
entry, or **recursion** fails the run with the call site named. The report lists every
annotation used, so a reviewer sees each assumption, and nothing is silently dropped (the W46
pass criterion).

### D4. Margin rule: worst case ≤ 85% of the stack, or the gate fails

`worst_path + 204 B ≤ 0.85 × STACK_WORDS × 4`. The 15% covers what the tool can't prove:
annotated frames that are measured rather than compiler-given, an indirect target missed in
review, and the next small change to a failure path (F-018's 160 B logging branch under `crypto_mac` is the example). A fixed
byte margin was rejected: 64 B is generous on a 512 B stack and nothing on a 1.5 KB one.

**First result, against this rule:** Node B's `can_task` is `1256 + 204 = 1460 B` against
`384 × 4 = 1536 B`, only **5%** of margin, so it fails D4. That is a finding (F-021), not a
reason to loosen the rule. `heartbeat_task` is `168 + 204 = 372 B` against 512 B: 27%, so it
passes.

### D5. The bench cross-check needs one new contract event: per-task stack high-water

The static number is an **upper bound**, so on silicon `static ≥ measured` must hold for every
task, or the tool is wrong. The measured side has to be visible from the normal image (CLAUDE.md
rule 7), and today it isn't for the `can` tasks.

**Decision:** a contract event `LOG_EVT_TASK_STACK(task_id, free_words)`, emitted for **every**
task by the existing health task (Node A) and heartbeat task (Node B) on the same 5 s tick as
`LOG_EVT_APP_ALIVE`. `task_id` is a small per-node enum listed in `events.csv`. `free_words` is
`uxTaskGetStackHighWaterMark()`. It's a contract event because a test needs it: the W46 bench
check asserts `static_worst ≥ STACK_WORDS − free_words` for each task. Cost: one record per task
every 5 s (3 on Node A, 2 on Node B).

*Alternative rejected: read `g_hw_*` with the debugger.* That works on Node A only, it needs a
debugger session the bench rule doesn't cover, and it's evidence nobody can rerun from a capture.

### D6. Where it runs: a local gate in the build scripts, its own tests in CI

- `pytest host_tools/tests/test_stack_drill.py` runs in CI against **fixture** `.su`/`.ci` files.
  These include the W40 case (856 B on a 768 B stack must fail) and the spike's unannotated
  oracle edge (must fail, not report 552 B).
- The real check needs a firmware build, which CI can't do. So `zz_build_gateway_app.sh` and
  `zz_build_actuator_app.sh` run `stack_drill.py` after a successful build and **stop before
  flashing** when it fails. There is no override switch: a failing stack is fixed or raised,
  not bypassed.
- The FBL is out of scope: a super-loop on MSP, no tasks (ADR-0004).

## Consequences

- **(+)** Stack sizes become checked numbers with a written-down path, not a high-water plus a
  comment. The W40 class of overflow fails before flashing.
- **(+)** Failure paths, the ones the bench rarely runs, are covered by construction.
- **(+)** No change to the shipped code: the flags leave the code bytes identical (spike).
- **(+)** Every assumption (indirect targets, library frames) is in one reviewed file.
- **(−)** The annotation file must track the code. A new function-pointer call site fails the
  run until someone annotates it. That's the intended cost, but it is a cost.
- **(−)** It's an upper bound along call-graph paths, not along *feasible* paths. It can report
  a path that never runs together (here: the failure branch of `crypto_mac` under the RX verify,
  which *is* feasible: the Seam 6 GARBAGE run on 11-06 drove it 6,843 times on Node A).
- **(−)** One more contract event (D5). Contract IDs are API.
- **(−)** Library frames (`memcpy`, `memset`) are measured, not compiler-given.

## Alternatives considered

| Option | Why not (as the primary method) |
|---|---|
| **Disassembly only** (`objdump`: `push`/`vpush`/`sub sp` per function, `bl` edges): the W40 prototype | No build change, but frame inference is fragile for anything unusual, and tail calls (`b`) and calls through registers (`blx rN`) are missed or guessed. Kept as the fallback in D1 |
| **Runtime only** (high-water, `configCHECK_FOR_STACK_OVERFLOW 2`, already on) | Free and already there, but it only sees paths that ran. Kept as the cross-check (D5) and the last line of defence, not as the sizing method |
| **MPU stack guard regions** | Turns an overflow into a clean fault instead of corruption. Worth having, but it detects at runtime, it doesn't size. A future idea, not this |
| **Commercial analysers** (e.g. AbsInt StackAnalyzer) | Abstract interpretation of the binary, strong on feasibility. Out of scope and licence for a portfolio project; named in the post as the industrial answer |
| **Open-source scripts** (`avstack.pl`, puncover) | Same data sources (`.su` or objdump), but they drop indirect calls silently, which is exactly the failure the spike showed |

## Open items

- **Node A not spiked yet.** The app build needs `NOINIT_LD_DIR` and `CY_IGNORE` from this
  shell. Do it Sat 14 with the tool; confirm identical code bytes there too.
- **`memcpy` / `memset` frames:** measure from `objdump -d` of the linked newlib-nano objects.
- **MSP budget** (D2): ISRs and nested interrupts, entry = the vector table. Its own item, after W46.
- **F-021:** Node B `can_task` at 5% margin. Options: raise `CAN_STACK_WORDS` (Node B has 1 MB
  SRAM), or take `crypto_mac()`'s ~500 B of buffers off the stack (they're per call and
  `crypto_mac` isn't reentrant across tasks today; check that before relying on it).
