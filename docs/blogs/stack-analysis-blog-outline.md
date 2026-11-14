# Outline — the stack-analysis series (2 posts)

*2026-11-14 · outline only, no prose. Style guide v2 (`docs/blogs/blog-style-guide.md`); M5 is the template.
Every fact names its source. `[TODO: measure]` = not in the repo yet; `[TODO: Ben]` = the owner decides.
Drafts: `docs/blogs/stack-1-blog-draft.md`, `docs/blogs/stack-2-blog-draft.md`.*

**Evidence used throughout:**
- **A** = `docs/bench/2026-11-14/stack_bound_asm_examples.md`: objdump of Node B `actuator_cm7.elf`, built 2026-11-12, `-Og`.
- **D** = `docs/bench/2026-11-14/stack_bound_failure_demo.md`: the tool's two refusals and its run time.
- **X** = `docs/bench/2026-11-11/stack_crosscheck.md` + `summary.txt`: static vs measured on silicon.

---

## Post 1 — "What a function leaves on the stack"

*Explainer, built on real Traveo code. Lens: design essay (style guide §13). ~6 min.*

### 1. Hook: a high-water mark only covers paths that ran
- M2 set the task stacks for the CM4F context frame, then planned to "trim later from
  `uxTaskGetStackHighWaterMark`" (`docs/briefs/M2-bringup_log.md` §3).
- By M5 the `can` stack was 192 words (768 B), sized before SecOC existed. The first `crypto_mac()`
  overflowed it (`docs/briefs/M5-bringup-plan.md` row 14; ADR-0025 Table 1).
- The question for this post: what does a function actually put on the stack, byte by byte?

### 2. The registers a call uses (AAPCS)
- `r0–r3`: arguments and the return value. `r4–r11`: callee-saved. `r12`: scratch. `sp`, `lr`, `pc`.
- Source: Arm's public *Procedure Call Standard for the Arm Architecture* (AAPCS32). Clean-room: only
  public Arm docs and our own objdump output.
- Real argument 7 and 8: `verify_core` reads them from the caller's frame at `[sp, #144]` and
  `[sp, #148]` (A).

### 3. One real function, line by line: `verify_core`
- Prologue: `stmdb sp!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}` = 36 B, then `sub sp, #100` → **136 B**,
  equal to GCC's `.su` (A).
- Epilogue: `add sp, #100`, then `ldmia.w sp!, {…, pc}`: the saved `lr` becomes `pc` and the function returns (A).
- Leaf function `get_u16le`: no push, no `sub sp`; it returns with `bx lr`, frame 0 B (A, objdump 2026-11-14).
- `vpush`: no app function on either node uses it (0 hits on Node B). The only one is FreeRTOS's switch,
  `vstmdbeq r0!, {s16-s31}` on Node A (objdump 2026-11-14), so it's shown in §6.

### 4. The frame, as a block diagram
- From `sp` upward: outgoing argument 5 for `build_mac_input` at `[sp]` (A), the MAC tag (16 B) and
  the MAC input (70 B = 2 + 4 + 64, `shared/secoc` defines), then 36 B of saved registers including `lr`.
- Above them, the caller's frame with `verify_core`'s arguments 5–8 (A).
- Stack alignment: 8 B at a public interface (AAPCS). Our profile uses 8 B
  (`tools/stack_bound/src/stack_bound/profiles/cortex-m-freertos.toml`).

### 5. `bl` sets `lr`, so a caller must save it
- `bl 100867c4 <build_mac_input>`: the target is in the instruction, and `lr` = the return address (A).
- `verify_core` calls four functions, so it pushes `lr`. `get_u16le` calls none, so it doesn't (A).
- Return paths: `bx lr` (leaf) and `ldmia … {…, pc}` (non-leaf), both real (A).
- `blx fp` is shown once and left as the teaser for Post 2.

### 6. Exceptions: what the hardware pushes
- Hardware frame: 8 words, or 26 with an FP context, + 1 alignment word (ADR-0025 D2; M2 log §3).
- `EXC_RETURN` in `lr` tells the core which stack and frame to restore. Tasks run on PSP; interrupts and
  `main()` run on MSP. [TODO: measure] if any number is quoted for MSP: not analysed until W47.
- FreeRTOS then pushes `s16–s31` (64 B, the real `vstmdbeq` from §3) and `r4–r11, lr` (36 B):
  **208 B** per task in total (ADR-0025 D2).
- How FreeRTOS spots an overflow: new stacks are filled with `0xA5`, and at each switch-out it checks
  that the lowest 16 B are still `0xA5` (method 2, both nodes; A, last section).

### 7. The whole call, as pseudo code
- **Illustration, not evidence** (owner, 2026-11-14): no memory dump. Pseudo code replays §3–§6 one step
  at a time, with `sp` tracked. The numbers are `verify_core`'s real ones (A).
- Block 1, one call and its return:

  ```
  # caller (secoc_verify_len)
  r0..r3 = cy, can_id, frame, plen        # arguments 1-4 travel in registers
  sp -= 16; [sp..] = epoch, counter, pdu_out, pdu_len   # 5-8 go on the stack
  bl verify_core                          # lr = return address, pc = verify_core

  # verify_core
  push {r4..r11, lr}                      # sp -= 36   saved registers + return address
  sp -= 100                               # locals: MAC input, tag, outgoing arg
  r6 = [sp + 144]                         # argument 7, read from the caller's frame
  bl build_mac_input                      # overwrites lr: that's why it was saved
  blx oracle->mac                         # target from a register (Post 2)
  sp += 100                               # locals gone
  pop {r4..r11, pc}                       # sp += 36   saved lr -> pc = return
  ```
  (Shown as 16 B for arguments 5–8; in the real code the caller reserves that space in its own frame.)

- Block 2, how FreeRTOS sees it (method 2 + the high-water mark, as configured on both nodes, A):

  ```
  create(task):
      fill task.stack with 0xA5               # every byte, once
  switch_out(task):
      push the context (208 B) onto task.stack
      if any of task.stack[0..15] != 0xA5:    # lowest 16 bytes
          vApplicationStackOverflowHook(task) # after the damage, not before
  high_water(task):
      return count of 0xA5 bytes from the bottom up   # only paths that ran
  ```
- Block 3, the overflow in one line of arithmetic: `sp_at_entry − (deepest path + 208) < stack_bottom`
  → a write below the array, into whatever the linker placed there.
- Honest — every example is a `-Og` debug build; `-O2` frames differ.

### 8. Teaser → Post 2
- One line: a 568 B frame (`push {r4, r5, lr}` + `sub.w sp, sp, #556`, A) behind a `blx` is exactly
  what blew up my CAN task. Post 2 is how I made sure it can't happen quietly again.

### MEDIA — Post 1
- **Hero:** `img/stack-p1/hero.svg` from the M5 template; caption `TRAVEO T2G · STACK SERIES 1/2`
  [TODO: Ben — caption wording].
- **Fig 1** (flowchart LR): paths that ran vs the `crypto_mac` path that hadn't been written yet; 768 B stack vs
  856 B worst case (M5 bring-up row 14).
- **Fig 2** (register strip): `r0–r3` · `r4–r11` · `r12` · `sp` · `lr` · `pc`, colour-coded by who saves them.
- **Code block:** `verify_core` prologue/epilogue, 6 lines, tagged with `←` notes (A).
- **Code block:** `get_u16le`, 4 lines, the leaf (A).
- **Fig 3** (stack column, flowchart TB): caller's args 5–8 · saved regs + `lr` 36 B · locals 100 B · `sp`;
  "grows down" arrow; free stack in grey.
- **Fig 4** (sequence diagram): `bl` → `lr` = return address → callee pushes `lr` → `pop {…, pc}`.
- **Fig 5** (stack column): exception entry 104 B + 4 alignment + FreeRTOS 64 + 36 = 208 B.
- **Fig 6** (before/after): healthy stack with its `0xA5` floor intact vs overflow into the neighbour,
  caught only at switch-out.
- **Code blocks (§7):** the three pseudo-code blocks. Optional **Fig 7** (sequence diagram): caller →
  `verify_core` → `build_mac_input`, with `sp` and `lr` noted on each arrow.
- **Metadata + Follow along**, per the template.

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
