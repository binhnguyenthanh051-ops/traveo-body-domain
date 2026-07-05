<!--
IDEA / OUTLINE — M2 application (FreeRTOS + CANFD + .noinit reprogram) bring-up chapter.
This is a skeleton, not a finished draft (unlike the M1 file). Flesh each beat into prose in
YOUR voice — the authenticity is the point. Same rules as M1:
- [MEDIA: ...] markers show where photos/GIFs/scope shots go (see the shot list at the end).
- Copyright: paraphrase the TRM / vendor docs; don't paste vendor driver source or TRM tables.
  Keep your own snippets short.
- Set the episode number to match your publishing order; link the code at the M2 tag.
- Source material to mine: docs/briefs/M2-bringup_log.md, docs/briefs/M2-app-can-bringup_log.md,
  docs/review/ADR-0010-0011-review.md, ADR-0010/0011/0007. The commit history on
  feat/m2-app-mvp reads as the timeline.
-->

# M2 blog — working titles (pick one)

- **"Init OK, no RX: debugging a CAN peripheral you can't see"** ← my favourite; the NVIC-mux
  saga is the strongest single story.
- "It worked, but for the wrong reason — three bugs the .map file caught"
- "Bring-up by instrument, not by guess"
- "The handshake that worked by accident"

## The throughline (the one idea the whole post argues)

**Cheap instrumentation beats guessing.** Every bug in M2 was localized by something small and
already in place — an assert that recorded `file:line`, four stage counters, a read of the
`.map` file, a stack high-water mark — not by staring at a fault. The M1 theme was "every
surprise lives at the hardware seam." M2's is: *make the seam observable before you need to.*

Secondary theme worth weaving in: **"it works" is not "it's correct."** Two of the best beats
(the `.noinit` handshake, the "stack overflow") *appeared* to work or looked like one thing,
and only a cheap check revealed the truth.

## The hook (opening idea)

Open on the CAN bug because it's the most visceral: the peripheral said a frame arrived
(the RX-FIFO flag was set), the loopback was engaged, the clock was running — and yet the
interrupt handler never ran, not once. The frame fired everywhere inside the peripheral and
never reached the CPU. Then pivot: I only found it in one flash because I'd wired four counters
that told me *where* the path died, not *that* it died.

---

## Beat 1 — The bet (framing), reprised from M1

One seam at a time: FreeRTOS alone, then CANFD, then the cross-image `.noinit` reprogram. Same
host-test split as M1 — the RTOS-independent logic (message decode, the body-control state
machine, the reprogram/boot-loop rule) proven on x86 first, so every on-board bug is a *seam*
bug. Keep this short; the M1 post already sold it. New wrinkle for M2: the seams are inside
*one* running system now, so I leaned harder on making each one *observable*.

## Beat 2 — The "stack overflow" that was a lie (Seam 1)

The first trap said `vApplicationStackOverflowHook`. It wasn't. Two clues: the task-name it
captured was null, and it happened *during scheduler start* — before any context switch, when
the overflow check literally cannot run. What actually happened: my two trap functions had
byte-identical bodies, so the **linker folded them into one address** and the debugger slapped
the wrong label on it. The real fault was a `configASSERT` — and the thing it caught was a
genuine bug: I asserted `VTOR == app_base`, but Cypress's startup **relocates the vector table
to RAM** after the bootloader's hand-over, so by `main()` it isn't the flash base at all.
- **Lesson:** give diagnostic traps *distinct* bodies or the linker will disguise them; and
  "the bootloader set VTOR" doesn't mean it still points there after the BSP startup runs.
- The fix that made the next bugs findable: I made the assert hook record `file:line`.

## Beat 3 — Init OK, no RX: the interrupt that fired everywhere but the CPU (Seam 2, the centerpiece)

The full saga. Build it in the order it happened:
1. RX FIFO 0 configured with **zero elements** → every accepted frame silently dropped.
   ("Enable FIFO" and "size FIFO" are two settings.)
2. Internal loopback did nothing until I learned its control bits are **write-protected** unless
   you re-enter config mode. (A nice "protected register" aside.)
3. The real one: loopback on, clock on, FIFO sized, RX interrupt enabled by the driver — and the
   handler still never ran. My four counters (`tx` climbing, `isr` stuck at zero) said: the frame
   was transmitted and looped, the peripheral flagged it, but nothing reached the NVIC. Cause:
   on this part the CM4 reaches peripheral interrupts through an **8-channel interrupt mux**; the
   CAN "IRQ number" is a *system-interrupt index*, not a CPU interrupt line. The obvious wiring
   (the form that works on the vendor's other family) silently routed it to the wrong place.
- **The honest twist:** I'd *written the NVIC-mux caveat into my own design record*, then shipped
  the wrong form anyway. The counters — not my memory — caught it.
- **Lesson:** per-stage counters turn "it doesn't work" into "it dies *here*," which is the whole
  ballgame. And a peripheral IRQ enum is not always a CPU interrupt line — know your interrupt
  topology.

## Beat 4 — The handshake that worked by accident (Seam 3)

The App→bootloader `.noinit` reprogram request worked end to end — pressed the button, the
bootloader stayed in programming mode. Done, right? Then I read the `.map` out of habit: the
handshake variable wasn't at the address I'd "pinned" — it was ~1.4 KB higher, inside a region
the BSP reserves. It matched between the two images *only* because both happened to lay out
their shared `.noinit` identically; a library bump would have silently split them. The catch:
`.noinit` is a shared catch-all (the BSP and libraries put things there too), and they land
first. The fix was to give the handshake its **own** named section, pinned, so its address is
mine, not an accident.
- **Lesson:** "it works" hid a latent field bug; the `.map` file — not the running system —
  told the truth. To pin *your* data, pin *its own section*, not the catch-all it shares.

## Beat 5 — Proving CAN twice: loopback, then the wire

Why I brought CAN up in two phases. **Internal loopback** proved the entire software path with
zero external hardware — so by the time a real interface was on the bench, the only new
variables were physical (transceiver, termination, timing on the wire). **Phase B** used a
borrowed Vector VN1610, but driven from a ~90-line Python script via `python-can` — no
five-figure analyzer licence, just the free driver library. (Nice practical aside: the "driver"
you install for the GUI tool is *not* the API library the script needs — a 20-minute detour
worth one paragraph.) Ten FD frames out, ten echoes back, first try on the wire.
- **Lesson:** split bring-up so software faults can't hide behind bus faults; and you rarely
  need the expensive tool to prove the cheap thing.

## Beat 6 — The misdiagnosis I got to reclaim (stacks)

Short, satisfying coda. Because the "stack overflow" (Beat 2) was a lie, the stacks I'd bumped
"to fix it" were 2–7× over-provisioned. I wired the tasks to report their own high-water marks,
read them off the running board (11–88 words used against 256–512 allocated), and trimmed on
*measurement* — reclaiming ~4 KB.
- **Lesson:** size stacks from the high-water mark on silicon, never from a guess (especially a
  guess born of a misdiagnosis).

## Beat 7 — What it proved / where it leaves M2

Payoff of the throughline: the four bugs above were caught by, respectively, an assert that
recorded its location, four counters, a `.map` read, and a high-water mark — none by staring.
State the milestone plainly: the app runs on the RTOS, CAN echoes on a real bus, and the
App→bootloader reprogram closes the handshake thread I'd deferred in M1. Honesty notes to keep:
the watchdog is only *assumed* here (real integration is a later milestone), and still no secure
boot. Tease M3 (UDS reprogramming) as the natural next step.

## Beat 8 — AI-workflow note (reuse M1's, updated)

The design records + scaffolding were AI-drafted then reviewed; the review caught real issues
(the missing queue in the budget, the linker-regeneration trap) before the board. But the sharper
M2 story is that the *instrumentation* — the counters, the assert capture, the habit of reading
the `.map` — is what turned AI-drafted code I had to understand-and-defend into something I could
actually debug on silicon. Treat every generated line as a draft you must be able to explain.

---

## Shot list (MEDIA)

- [MEDIA: two LEDs — the app heartbeat (slow) vs the bootloader "in programming mode" blink
  (fast) — the visual proof the reprogram request crossed images.]
- [MEDIA: debugger watch window — the four CAN stage counters, `tx` climbing while `isr` stays 0
  (the moment the NVIC-mux bug is localized).]
- [MEDIA: a trimmed `.map` snippet — the handshake at the *wrong* offset (before) vs pinned in
  its own `.fbl_handshake` section at the agreed address (after).]
- [MEDIA: terminal — the Python probe printing "10/10 echoes received" over the VN1610.]
- [MEDIA: debugger — the high-water marks (free words per task) that justified the stack trim.]
- [MEDIA: optional — the bench: kit + VN1610 wired to the CAN connector, termination visible.]

## One-liners worth keeping (raw, for the edit pass)

- "The frame fired everywhere inside the peripheral and never reached the CPU."
- "I'd written the caveat into my own design doc, then shipped the bug it warned about."
- "It worked — which is exactly why I almost didn't check the map file."
- "The debugger you reach for to investigate is sometimes the thing hiding the bug." (M1 callback
  — the folded-assert mislabel is a cousin of M1's works-attached/faults-detached.)
