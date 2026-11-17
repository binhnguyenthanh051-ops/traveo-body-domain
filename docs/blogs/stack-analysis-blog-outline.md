# Outline — the stack-analysis series (2 posts)

*2026-11-14 · outline only, no prose. Style guide v2 (`docs/blogs/blog-style-guide.md`); M5 is the template.
Every fact names its source. `[TODO: measure]` = not in the repo yet; `[TODO: Ben]` = the owner decides.
Drafts: `docs/blogs/stack-1-blog-draft.md`, `docs/blogs/stack-2-blog-draft.md`.*

**Evidence used throughout:**
- **A** = `docs/bench/2026-11-14/stack_bound_asm_examples.md`: objdump of Node B `actuator_cm7.elf`, built 2026-11-12, `-Og`.
- **D** = `docs/bench/2026-11-14/stack_bound_failure_demo.md`: the tool's two refusals and its run time.
- **X** = `docs/bench/2026-11-11/stack_crosscheck.md` + `summary.txt`: static vs measured on silicon.

---

## Post 1 — "What happens on the stack when a function calls another" (reworked 2026-11-14)

*For a **fresher**: knows what RAM, a pointer and a function call are; has never looked inside a stack.
Owner, 2026-11-14: the first draft read like it was written for a senior. ~6 min.*

**One small example, not the CAN code.** A demo task built only with `STACK_DEMO=1`:

```c
task  -> func_a(0x11): uint32_t arr_a[2] = {0xAAAA0001, 0xAAAA0002}
      -> func_b(0x22): uint32_t arr_b[2] = {0xBBBB0001, 0xBBBB0002}
      -> func_c(0x33): uint32_t arr_c[4] = {0xCCCC0001 … 0xCCCC0004}   <- breakpoint
```

Values chosen to be easy to spot in a hex dump. **Every word shown in the post comes from the bench
capture** (Node B, GDB `x/…wx`), planned for 2026-11-15. Until then: `[TODO: measure]`.
The CAN chain, `verify_core`, `blx`, PendSV, EXC_RETURN, AAPCS details and the float ABI move to Post 2
or out.

### 1. Hook (keep)
M5's confession: the stack doubled from 768 B to 1.5 KB. "I'd never looked inside one."

### 2. What is a stack?
- A block of RAM set aside for one task, plus one register, `sp`, that marks how full it is.
- Last in, first out: a pile of plates.
- In FreeRTOS it's literally an array: `static StackType_t stack[128];` (512 B). Its real address from the
  capture `[TODO: measure]`.
- **Fig:** RAM as a strip of addresses, the demo task's array highlighted, `sp` pointing into it.

### 3. Why does a function need the stack?
Three things it can't keep in registers, each with a one-line reason:
- **the way home** (return address): the CPU must know where to continue after the call;
- **local variables**, especially arrays: too big for registers;
- **saved registers**: the caller's values it borrows and must give back.
- **Fig:** one frame = way home + saved registers + locals.

### 4. The example
The C code above, 15 lines. "Watch the stack while it runs."

### 5. Watch it fill up (the core of the post, 4 real snapshots)
Water-tank figures, **lowest address on top** (like a debugger's memory window), so the stack rises like water:
1. Task just started: everything still `0xA5` (FreeRTOS's paint).
2. Inside `func_a`: `arr_a` + the way home.
3. Inside `func_b`.
4. Inside `func_c`, at the breakpoint: **the whole stack, every word**, coloured by function:
   `address · value · whose · what` (e.g. `0xCCCC0001 · func_c · arr_c[0]`, `0x1008…  · func_c · way back into func_b`).
- **Fig:** the 4 tanks side by side. **Fig:** the full word-by-word dump at the breakpoint.

### 6. Push and pop, in 4 instructions
`func_b`'s disassembly: `push {…, lr}` / `sub sp, #…` … `add sp, #…` / `pop {…, pc}`.
Why `lr` must be saved: `bl` overwrites it, there's only one. **Fig:** sequence a → b → c → back.

### 7. Coming back: the stain = the high-water mark
- After the calls return, `sp` is back at the top, but the old values are still there (capture #5).
- FreeRTOS counts the bytes still `0xA5`: that's the high-water mark. **Fig:** the tank with the stain line.
- The catch: it only knows the paths that ran. One line → Post 2.

### 8. What happens when it doesn't fit?
- `STACK_DEMO=2`: `func_c` gets an array bigger than the task's stack. The water goes over the rim into the
  next variable in RAM, and FreeRTOS's overflow hook fires at the next task switch (capture: breakpoint in
  the hook, the stack's lowest 16 B no longer `0xA5`) `[TODO: measure]`.
- One line: an interrupt also needs room on the task's stack at any moment.

### 9. User manual
- GDB: `break func_c`, `bt`, `x/128wx <stack base>`; what each shows.
- Build switch: `STACK_DEMO=1` / `=2` on the make line (off by default).
- Failure demo = §8.
- Forward link: "Now imagine a call where nobody can tell who gets called." → Post 2.

### MEDIA — Post 1
- Hero: redo to match (the tank).
- Figs: RAM strip · one frame · 4 tanks · full dump · sequence · stain · overflow. Every value from the capture.

---

## Post 2 — "The CAN task that outgrew its stack, and the tool I built so it can't happen quietly again"

*The incident + `stack_bound`. Lens: bring-up findings inside the design frame (§13). Function
pointers are the headline. ~8 min.*

### 1. The incident, bring-up
- W40, M5 Stage 3: Node B `can` overflowed on its first `crypto_mac()`. `vApplicationStackOverflowHook`
  fired, then the hook's endless loop tripped the hardware watchdog (M5 bring-up row 14).
- The fix then: `can` + Node A `can` / `app` went from 192 to 384 words (commit `0cab23f`).
- A 30-line prototype found 856 B worst case vs 768 B (`docs/future-ideas.md` "Stack drill").
- Twist, set up here: the doubled stack still had only 48 B left on its worst path (§7).

### 2. Why the obvious tools missed it
- The high-water mark only sees paths that ran (Post 1 recap, one line).
- GCC's own call graph gave **552 B** for a **1256 B** path: it stops at the function pointer
  (ADR-0025 Table 5, spike 2026-11-06).
- Why the pointer is there on purpose: SecOC reaches the MAC through `secoc_mac_if_t`, so the host tests
  plug in a fake (`shared/secoc/tests/secoc_mac_fake.c:35`).

### 3. Function pointers: the call nobody can see (headline)
- C: `cy->oracle->mac(cy->key_id, macin, macin_len, tag)` (`shared/secoc/src/secoc.c:139`).
- Binary: `ldr r3, [r7, #0]` → `ldr.w fp, [r3]` → `blx fp`. The target is in a register, not the instruction (A).
- The value: `g_oracle` in flash holds `0x10086129` = `crypto_mac` + the Thumb bit (A). That bit is how the
  tool spots stored function addresses (address-taken).
- Honest — today the bindings are a list I write, and the tool checks each one against the binary. Following
  stored values automatically is the next slice (`tools/stack_bound/src/stack_bound/analyze.py` docstring).

### 4. The concept: walk every path, refuse to guess
- Pseudo code: `depth(f) = frame(f) + max(depth(callee))`. A direct `bl` gives its target; a `blx` needs a
  binding or it's an error. Recursion, or a function with no frame, is also an error (`analyze.py` `_walk`).
- Worked example, today's build: log branch 152 B; `crypto_mac` 568 + 8 + 152 = 728 B; `verify_core`
  136 + 728 = **864 B** (A).
- `required = depth + 208 B`, then pass if ≤ 80% of the stack (ADR-0025 D2, D4).
- Two real refusals: no binding → exit 2; wrong target → "never address-taken" (D).

### 5. The design, and why
- The ELF is the referee (which functions exist), GCC's `.su`/`.ci` is the witness (frames, calls), and the
  runtime high-water is the cross-check (stack_bound ADR-0001 D1, D2; ADR-0025 D5).
- One alternative each, inline: compiler data only misses prebuilt `memcpy` (8 B, project file
  `[frames]`); runtime only sees what ran.
- Why 80% and not a fixed number of bytes, and why the tool lives in its own repo (ADR-0025 D4; ADR-0001 D15).

### 6. The implementation
- Five steps: read the ELF → read GCC's output → join + bind → walk + check → one report as text/JSON/HTML
  (`tools/stack_bound/src`, ~1,050 lines incl. profile; `wc -l` 2026-11-14).
- A real `.su` line and `.ci` edge (`secoc.c:126:30:verify_core 136 static`; `__indirect_call` at
  `secoc.c:139:10`), from the build in `node_b_actuator/proj_cm7/build`.
- The gate in the build scripts, with no override (`tools/stack_gate.sh`), plus the
  `LOG_EVT_TASK_STACK` event every 5 s (ADR-0025 D5, D6).
- Spec block: Python 3.11+, pyelftools, exit codes 0–3, 47 tests (pytest, v0.2.0), 1.6–2.5 s per image (D).

### 7. The test
- The first real image broke the tool: three `s_stack` arrays and 0.1.1 silently took the last one; now an
  error (ADR-0001 §8 Table 12). Also 2,006 linker-removed entries (Node B report, 2026-11-11).
- Three stacks failed, not one: A `can` 97% (48 B left), B `can` 96% (64 B left), A `app` 82%. Now
  73 / 72 / 61% at 512 words (`docs/project/findings.md` F-021; commit `e322a6f`).
- Silicon: measured ≤ static on all 7 tasks, e.g. A `can` 1200 ≤ 1488 B, B `can` 1144 ≤ 1472 B (X).
- The bench surprise: the gate stopped two good builds (`/cygdrive` path, `pass\r`) and failed closed
  (X Check 3; commit `94ce2f5`).
- Cost spec block: identical code bytes, +512 B per raised stack, 6/5 log records per 5 s, +1.6–2.5 s
  per build (ADR-0025 D7, D5; D).
- Honest — it counts paths that may never run together; here the error branch is real (6,843 runs, ADR-0025 Table 6).

### 8. User manual
- Run: `python -m stack_bound <elf> --su build --project stack_bound.toml --html report.html`.
- Success: Node B's gate output, 5 tagged lines (`docs/bench/2026-11-11/flash_nodeB.log` lines 81–97).
- Failure demo: delete the `verify_core` binding → exit 2, nothing flashed (D).

### 9. Forward link + prediction
- Honest — the interrupt stack (MSP) isn't analysed yet; every report says so. That's exactly what W47 adds.
- Prediction: [TODO: Ben] — what you expect the MSP worst case to show, labelled as a prediction and
  checked in the W47 post.

### MEDIA — Post 2
- **Hero:** `img/stack-p2/hero.svg`; caption `TRAVEO T2G · STACK SERIES 2/2` [TODO: Ben — wording].
- **Fig 1** (before/after): GCC's graph stops at `?` (552 B) vs following the pointer (1256 B). Draft Fig 2.
- **Code block:** the C call, then the 3 asm lines, then the `g_oracle` flash word (A).
- **Fig 2** (trace, flowchart TB): `blx fp` ← `fp = [r3]` ← `r3 = [r7]` ← `r7 = cy` → `g_oracle` →
  `crypto_mac` 568 B.
- **Fig 3** (funnel): all functions → address-taken → type matches → bound by value (stack_bound idea Fig 5).
- **Code block:** the walk's pseudo code; the worked 152 / 728 / 864 B sums.
- **Fig 4** (call tree): `verify_core` and its four callees, the deepest branch highlighted.
- **Fig 5** (decision): binding? → address-taken? → edge, or one of two errors. Draft "Step 3" figure.
- **Fig 6** (three sources → referee / witness / cross-check). Draft §3 figure.
- **Spec block:** the 208 B switch frame, 4 lines.
- **Fig 7** (pipeline, 5 steps). **Fig 8** (build → gate → flash → bench). Both from the draft.
- **Fig 9** (before/after): silent `s_stack` pick vs error. **Fig 10:** three stacks before/after.
  **Fig 11:** measured ≤ static, 7 tasks.
- **User manual blocks:** the gate table, one `LOG_EVT_TASK_STACK` line, the refusal line.
- **Metadata + Follow along**, with the stack_bound repo at tag `v0.2.0` (public, checked with `gh` on 2026-11-14).

---

## Open for the owner
- Post 2 §9: your prediction for the interrupt stack.
- Hero captions; "Forward Link + Prediction" isn't a rule in the guide yet: add it there if it should
  apply to every post.
