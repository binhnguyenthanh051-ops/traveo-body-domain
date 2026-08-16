# ADR-0023: Target logging channel — structured events over UART, assertable by the BVT

**Status:** proposed · **Date:** 2026-08-01

> Owns the **observability channel** out of both nodes: the record format, the ring-buffer
> seam, the cross-core routing, the failure policy, and — the part that makes this more than a
> `printf` — **the rule for when a log line may be used as a test assertion**. Consumed by the
> hardware BVT bench (`docs/briefs/BVT-bench-brief.md`, ADR-0022 pending). Reuses the
> non-cacheable shared-RAM placement established by ADR-0018 D6; it does **not** reuse the IPC
> mailbox itself (D4). MISRA constraints per ADR-0003.
>
> The requirements it must satisfy are `REQ-LOG-001..014` in `docs/requirements/logging.md`;
> `@impl`/`@test` tags link code and tests back to the `Dn` here. `REQ-LOG-009` is the
> observable form of the per-reason drop counters already required by `REQ-SECOC-001`.

## Context

Neither node has a console. There is no `retarget-io` anywhere in `node_a_gateway/` or
`node_b_actuator/`, so the only evidence a running target emits today is CAN traffic. That was
survivable while every bring-up happened with a debugger attached and a human watching. It
stops being survivable the moment a BVT runs unattended and reports "red" with no explanation.

But "add a console" understates the requirement, because of what M5 asks the bench to prove.
The SecOC negative tests — replay and forgery — assert that the actuator **holds its
last-known-good state**. That is a *negative observation*, and it passes identically when:

- the node correctly rejected the frame *(the property we want)*,
- the node never received the frame,
- the node is wedged, dead, or was never listening.

A test that cannot separate those is not testing the security property. For a milestone whose
entire point *is* the security property, that is the difference between a test and a placebo.

So the logging channel is not a debugging convenience bolted on beside the tests — for the
rejection paths it is **the only positive evidence that exists**, and it has to be designed
with the same care as a wire protocol: a stable format, a stable event vocabulary, defined
overflow semantics, and an explicit contract about what a consumer may depend on.

Two constraints bound the design:

1. **No `<stdio.h>` in production C** (ADR-0003, `docs/coding-standard.md`). `cy_retarget_io`
   is a `printf` shim; adopting it would deviate from our own standard on line one, for
   convenience — the least defensible kind of deviation.
2. **Some log sites are on hot paths.** A SecOC verify runs per received frame. Whatever
   logging costs must be payable inside a per-frame path and inside an ISR, without perturbing
   the timing the milestone is measuring.

## Decisions

### D1. Fixed-size **binary event records**, not formatted text

The producer never formats. `log_evt(evt_id, arg0, arg1)` writes a fixed 16-byte record; all
rendering happens on the host.

- **Why not text:** formatting means writing our own integer/hex conversion anyway (no
  `printf`), so text is *more* target code, not less — and it is variable-cost code on a
  per-frame path. Decimal conversion in a SecOC verify or an ISR is exactly the jitter we
  don't want while measuring that path.
- **Why it matters beyond cost:** a formatted string is not a stable interface. Asserting on
  one couples the test suite to prose, and prose gets reworded. An event ID is a contract (D6).
- **Tradeoff (real, accepted):** attaching a plain terminal shows bytes, not sentences. A host
  decoder is now mandatory tooling rather than optional. D3 buys most of that back.
- **Alternative rejected:** `cy_retarget_io` + `printf` with a documented MISRA deviation.
  Fastest to stand up, and genuinely fine for a project that only ever reads logs with human
  eyes. Rejected because it fails the per-frame-path constraint and gives the BVT nothing
  stable to assert on — it optimises for the bring-up week and taxes every week after.

### D2. Record layout — explicit byte offsets, little-endian, CRC-16, 16 bytes

```
off  size  field
 0    1    sync      0xA5
 1    1    core_seq  (core_id << 6) | (seq & 0x3F)
 2    2    evt       u16 LE   — event ID (D6)
 4    4    ts_ms     u32 LE   — producing core's monotonic ms
 8    4    arg0      u32 LE   — event-specific
12    2    arg1      u16 LE   — event-specific
14    2    crc16     CRC-16/CCITT-FALSE over bytes 0..13
                                                        = 16 bytes
```

- **Encoded byte-by-byte**, never by overlaying a `struct` on a buffer — same discipline as
  `crypto_msg.c` and `shared/secoc`. No packing pragmas, no padding surprises, no
  implementation-defined bitfield layout.
- **16 bytes is deliberate:** a power-of-two record in a power-of-two ring makes every index
  computation a mask instead of a division (MISRA-friendly, and cheap in the producer's
  critical section).
- **`seq` (6 bits, per core) is not decoration** — see D5. It is how a consumer learns that
  records were *dropped* rather than never emitted, which is precisely the ambiguity that
  would otherwise let a dropped rejection event masquerade as "the node never rejected".
- **`crc16` guards resync, not the wire.** A UART on a bench desk rarely corrupts bytes, but a
  decoder attaching mid-stream is guaranteed to land mid-record. It locks onto a candidate
  `0xA5`, validates the CRC, then steps 16 bytes at a time, re-syncing only on CRC failure.
- **Tradeoff:** 16 B/record is fat next to a 4-byte trace word. We are not bandwidth-starved on
  a dedicated debug UART, and the headroom buys a real timestamp and two arguments.

### D3. `0xA5` sync is outside ASCII — so free text passes through the same stream

Because every record begins with a byte ≥ 0x80 and ASCII is < 0x80, **raw text and binary
records can share one UART** with no escaping and no mode switch. The decoder emits any
non-record byte run verbatim.

This resolves D1's tradeoff honestly rather than by assertion:

- A fixed **ASCII boot banner** proves TX works from a plain terminal (PuTTY) before any
  decoder exists — the first thing you need when bringing the logger itself up.
- Genuinely one-off human notes stay possible via `log_text()` without inventing a second
  channel.
- **But text is never assertable** (D6). It is for humans; the events are for the gate.

### D4. Cross-core transport: a **separate SPSC ring in shared RAM** — *not* the IPC mailbox

Each producing core owns one lock-free single-producer/single-consumer **byte** ring in shared
RAM. The UART-owning core (CM4 on Node A, CM7 on Node B) drains every ring.

**The mailbox is the wrong carrier, and this corrects an earlier instinct to reuse it.** The
IPC mailbox (ADR-0018 D3) is *synchronous and single-outstanding* by design: acquire the
semaphore, send, block until the answer. Logging is asynchronous, one-way, fire-and-forget, and
far more frequent. Putting log traffic through it would:

- serialise log writes behind in-flight crypto RPCs, and vice versa — coupling logging latency
  directly into **SecOC verify latency**, the number M5 exists to measure;
- make the M0+ MAC path contend for the same semaphore it needs for its actual work;
- give logging the mailbox's blocking failure modes, when D7 requires it never to block.

What *is* reused is the thing worth reusing: **the memory placement.** The rings live in the
same **MPU non-cacheable region** as the mailbox (ADR-0018 D6). On Node B the CM7's L1 D-cache
makes a naively-placed shared buffer silently incoherent — that problem is already solved, and
the log rings inherit the solution rather than rediscovering the bug.

- **Byte-oriented, not record-oriented**, so an ASCII run (D3) and a 16-byte record share one
  ring. A producer reserves its full length atomically, fills, then publishes.
- **Tradeoff:** two independent time bases. Each core stamps `ts_ms` from its own tick, so
  cross-core ordering is approximate and skew is uncorrected in M5. The host decoder sorts
  per-core and interleaves by timestamp, which is good enough to read a boot sequence but is
  **not** a basis for cross-core timing claims. *(Follow-up: check the TRM for a free-running
  counter readable from both cores; if one exists, stamping from it removes the caveat.)*
- **Alternative rejected:** one shared ring with a multi-core lock. Needs a hardware semaphore
  on the hot path, and a stalled core could then block the other's logging. Per-core SPSC rings
  need no cross-core lock at all.

### D5. Overflow is **drop-newest, counted, and announced**

The ring is finite and the UART is slow; a burst *will* overrun it. Silent loss is
unacceptable, because "event absent" is exactly what several BVT tests assert on.

- Producer drops the record when the ring lacks room. **Never blocks, never spins, never
  disables interrupts waiting for space.**
- Each drop increments a per-core counter. When space frees, the producer emits
  `LOG_EVT_OVERFLOW` with the number lost.
- `seq` (D2) gives the consumer a second, independent detector: a gap in the per-core sequence
  proves loss even if the overflow record itself was lost.
- **The BVT must treat any overflow in a test window as a failed run, not a passed one.** A
  suite that reports "green" from a window it knows was lossy is worse than one that reports an
  error.
- **Why drop-newest, not drop-oldest:** drop-oldest requires the *producer* to advance the
  consumer's tail, which breaks the lock-free SPSC invariant and would need a lock. It also
  preserves the boot banner and the early records, which is usually what explains a failure.
  The cost is that a burst hides its own tail — accepted, and made visible by the counter.

### D6. Two ID ranges: **contract events** (assertable) and **diagnostic events** (never)

| Range | Class | Rule |
|---|---|---|
| `0x0000–0x0FFF` | **Contract** | Part of the tested interface. A BVT/regression test may assert on these. Changing or removing one is a breaking change and must update the tests in the same commit. |
| `0x1000–0xFFFF` | **Diagnostic** | Free-flowing developer telemetry. **No test may assert on these**, so they can be added, reworded, or deleted at will. |
| *(any ASCII run)* | **Text** | Human-only. Never assertable, by construction. |

Within the contract range the high byte is the module (`0x00xx` boot, `0x01xx` SecOC,
`0x02xx` crypto, `0x03xx` diag/UDS, `0x04xx` app).

This turns "may I assert on this?" into a **mechanical question with one right answer** rather
than a judgment call — the same principle as `docs/checks-pipeline-brief.md`: a grep that is
right 100% of the time beats a reviewer who is right most of the time. It also means the
diagnostic tier stays genuinely free: no one has to wonder whether deleting a log line will
break CI.

The M5 events that matter are contract events by definition, because they are the positive
evidence named in Context — e.g. `SECOC_REJECT_FRESHNESS`, `SECOC_REJECT_MAC`,
`SECOC_ACCEPT`, each carrying the CAN ID and reason in `arg0`/`arg1`.

### D7. Logging never changes system behaviour — including when it fails

- `log_evt()` is safe from task or ISR context, on any core, and is **bounded and
  non-blocking**. Its critical section spans **reserve + copy + publish** as one indivisible
  step, with interrupts masked (`BASEPRI`). Never the UART.

  > **Corrected 2026-08-02.** This decision originally said the masked region covered "the
  > reservation only — not the copy". That is wrong, and review of the first implementation
  > found it. With two producers on one core (a task and an ISR), releasing the lock after
  > reserving allows: task reserves `[0,16)` → ISR reserves `[16,32)`, copies, sets `head=32`
  > → the drain ships 16 bytes the task has not written yet → task then sets `head=16`,
  > moving head *backwards*, so `head - tail` underflows and the drain treats the whole ring
  > as pending and floods the sink. The narrow critical section was a false economy: copying
  > 16 bytes under mask costs well under a microsecond, and buying it back would require a
  > separate commit-ordering index. `LOG_TEXT_MAX` (64 B) bounds the longest single enqueue
  > so the masked duration stays bounded for text too.
- The sink is drained by a **low-priority task** on the UART-owning core. If that task never
  runs, producers degrade to dropping (D5); nothing stalls.
- A full ring, a dead sink, or an unconfigured port are all **non-errors** at the call site.
  `log_evt()` returns `void` on purpose: there is no failure a caller could sensibly handle,
  and offering one would invite callers to add error paths on a diagnostic facility.
- **`log_panic()` is the deliberate exception.** From a fault handler the ring is worthless —
  no task will ever drain it — so `log_panic()` bypasses the ring and writes the record with a
  blocking, polled, interrupt-free UART write. It is the one place logging is allowed to spin,
  because at that point nothing else is going to run anyway.

### D8. Host-testable core, target-only port (ADR-0001)

- `shared/log/src/log.c` — ring reservation, record encoding, CRC, sequence/overflow
  accounting, drain. **Pure logic, no vendor headers, host-tested with plain GCC.**
- `log_port_*()` — UART TX, monotonic ms, interrupt mask/restore, and the ring storage
  (which is target-specific: linker section + MPU non-cacheable region, D4). One link-time
  implementation per image, faked in `tests/` — the **port-singleton** shape from
  `docs/coding-standard.md`, matching `fbl_port_*` / `app_port_*`. There is exactly one logger
  per image, so this is not an `_if_t` vtable.

The decoder is testable from the same fixtures: encode records with the host build, decode with
the Python tool, assert round-trip. That closes the "the tester can lie" gap from the BVT brief
— a format drift between C and Python fails a host test, not a bench run at midnight.

### D9. The event registry is **generated from one source**

`shared/log/events.csv` (id, name, class, arg0 meaning, arg1 meaning) generates both
`log_events.h` (C enum) and `host_tools/lib/log_events.py` (decoder table). A `checks/` script
fails CI if either generated file is stale.

Hand-maintaining the ID list in two languages guarantees eventual drift, and a drifted decoder
produces **confidently wrong test results** — the worst failure mode available to a merge gate.
One source, mechanically enforced.

### D10. Output is **cyclic with a published latency bound**, and the sink starts **polled**, not DMA

Two questions that look separate — *"does a log print immediately?"* and *"should the UART use
DMA?"* — are the same question about where the cost of getting bytes out is paid.

**Output is cyclic, and that is the design, not a compromise.** `log_evt()` enqueues and
returns; a low-priority task drains. Immediate output would mean the producer waits on a
~10 µs/byte UART, on a per-frame path, at any priority including inside an ISR — the exact
inversion D7 forbids. A logging call that can stall the SecOC verify path has changed the
behaviour it was added to observe.

Cyclic output would normally cost timeline fidelity. Here it does not, because **`ts_ms` is
sampled inside `log_evt()`, at production time, not at drain time**. Drain order and drain delay
never distort the recorded sequence; the host reconstructs the true timeline regardless of how
lumpy the output was. Buffering costs you *freshness*, not *truth*.

What it does cost is **latency**, and latency has to be published rather than left implicit —
because the BVT asserts on the **absence** of events (replay/forgery), and "absent" is
indistinguishable from "not drained yet". Hence `LOG_DRAIN_LATENCY_MS` (REQ-LOG-014): a stated
worst case from `log_evt()` returning to the byte leaving the sink, which a test waits out before
concluding absence. Without a published bound, every absence assertion is a race.

Two consequences follow from buffering, both already handled: records queued when the system
dies are lost (hence `log_panic()`'s ring bypass, D7), and a burst can overrun (hence
drop-newest + counting, D5).

**On DMA — the right answer is "later, and behind the port seam".**

| | Polled / interrupt-driven | DMA |
|---|---|---|
| CPU cost at 1 Mbps | Refill the SCB TX FIFO per drain tick; small, but proportional to bytes | ~one interrupt per burst; near-constant |
| Complexity | Trivial; works on day one | Descriptor setup, and a ring wrap must be split into ≤2 linear transfers |
| Failure mode | Visibly stops | Silently stalls mid-descriptor — harder to diagnose, on the very channel you diagnose *with* |
| Resource | None | Consumes a DataWire/DMAC channel |
| Panic path | Same code path | **Unusable** — a fault handler cannot wait on DMA, so a polled path must exist anyway |

That last row is decisive: `log_panic()` needs a blocking polled write regardless, so choosing
DMA means maintaining *two* TX paths rather than one. Combined with 1 Mbps (see baud, below)
being comfortable for polled output at our record rates, DMA buys little now.

Cache-coherency, the usual DMA trap, is already neutralised — the rings live in the
non-cacheable region (D4), so a DMA engine cannot read stale cached data. That placement
decision pays twice.

**`log_port_tx()`'s contract is already DMA-ready**, which is why this can be deferred safely: it
is non-blocking and returns a *partial* accepted count, so a DMA implementation swaps in without
touching `log.c`. One caveat to record now rather than discover later: "accepted" must mean
"safely consumed", so a **zero-copy** DMA straight from the ring would need the tail pinned until
transfer completion — an extra `log_port_tx_done()` call. Prefer a small port-owned bounce buffer
(~256 B) instead: it keeps the interface as written, at the price of one copy.

**Revisit DMA only on measurement** — if drain CPU time shows up in an M5 timing measurement, or
the overflow counter (D5) says the sink cannot keep up.

## Consequences

- **(+)** The SecOC rejection tests become positive assertions instead of "nothing happened"
  (Context). This is the decision's main return.
- **(+)** Bring-up debugging stops requiring a debugger and a human at the desk.
- **(+)** No MISRA deviation is needed; no `<stdio.h>` enters production code.
- **(+)** The format is cheap enough for a per-frame path, so the M5 timing work is not
  distorted by its own instrumentation.
- **(−)** A host decoder is now required tooling. Mitigated by D3 (banner + text passthrough
  readable in any terminal).
- **(−)** Two generated files and a staleness check — new build machinery, however small.
- **(−)** Cross-core timestamps carry uncorrected skew (D4). Fine for reading a sequence; **not**
  a basis for cross-core latency claims until the shared-counter follow-up lands.
- **(−)** `log_evt()` on a hot path still costs a short interrupt-masked region. Bounded and
  measured at bring-up, but it is not free, and a log site added inside a tight loop can still
  hurt. Reviewer's job, not the format's.

## Resolved open items

### Baud rate — **1 Mbps default**, not the 4 Mbps the bridge allows

The KitProg3 USB-UART bridge tops out at **4,000,000 bps**. We deliberately do not use it.

At 16 B/record, **1 Mbps ≈ 100 KB/s ≈ 6,250 records/s** — one to two orders of magnitude more
than any path here produces, including a per-frame SecOC log at full CAN FD load. 4 Mbps buys
headroom we have no use for and costs real things:

- **CPU.** 400 KB/s must be fed by something. Without DMA that is a per-byte or
  small-FIFO interrupt load on the *UART-owning core* — the CM4/CM7 running the application —
  which is precisely the perturbation D7 exists to prevent. The instrumentation would start
  distorting the M5 timing it exists to observe.
- **Baud accuracy.** The SCB derives its rate by dividing CLK_PERI against an oversample
  factor; the result must land within roughly 2% or framing errors appear. Exact divisors get
  scarce at the top of the range — *check the divider math against each node's clock tree for
  whichever rate is chosen.*
- **Bridge behaviour.** USB-CDC buffering and latency are least predictable at maximum rate,
  and a flaky console is worse than a slower one.

Revisit only if the overflow counter (D5) says so — measurement, not argument. The 4 Mbps
ceiling is documented headroom, not a target.

*(Still to confirm: which SCB instance and pins each kit routes to the KitProg3 bridge.)*

### Cross-core time base — **a TCPWM 32-bit counter**, and the per-core options rejected

The TRM (`docs/references/TraveoT2G_BodyControlEntry_TRM.pdf`, §25.3.1) settles D4's
follow-up. Candidates, and why most fail:

| Candidate | Verdict |
|---|---|
| **SysTick** | **Per-core** (§11278: both CM0+ and CM4 have their own, part of the core's internal peripherals). This is exactly the skew source D4 describes — the problem, not the fix. |
| **MCWDT** | Free-running 32-bit subcounter (`MCWDTx_CTR2_CNT`, §20.4.2.2), but §1.2.2.12: *"An MCWDT is available for each of the CPU cores… it is recommended to assign one MCWDT per processor."* Per-core by design, and entangled with watchdog configuration. Clocked from ILO0/WCO (~32 kHz → ~30 µs). Rejected on ownership, not resolution. |
| **Basic WDT** | Genuinely device-level and free-running (§1.2.2.12), but it is *the watchdog* — repurposing it as a timebase couples logging to the reset supervisor. No. |
| **EVTGEN counter** | Auto-reloads (§28.2.3) — periodic, not free-running. Wrong shape. |
| **CAN FD global timestamp** | Device-wide and shared across M_TTCAN groups (§23.3.2), but 16-bit and captured *per frame* into RX/TX elements rather than freely readable. Interesting for correlating a log record to a specific frame later; not a general timebase. |
| **TCPWM 32-bit counter, timer mode, continuous** | **Chosen.** |

**Decision:** dedicate one TCPWM 32-bit counter in **timer mode, continuous** (§25.3.1,
`ONE_SHOT = 0`), clocked from a peripheral divider at **1 MHz**. It is a peripheral, so it sits
equidistant from both cores on the AHB-Lite bus; 32 bits at 1 µs wraps in ~71.6 minutes, and a
wrap is detectable host-side from the record sequence.

Consequences to design for:

- **Both cores need read access**, which is a PPU configuration question and therefore
  **ADR-0020's territory** — read-only for the app core is the right default; the counter
  should be configured once by the security core.
- Reading costs a peripheral-bus access rather than a core-local register. Cheap, but not
  free, and subject to bus contention — acceptable inside the already-bounded `log_evt()`.
- Costs one counter from the TCPWM pool.
- Exact register names for this part live in the separate **Registers** TRM (§25.2 note); the
  PDL (`Cy_TCPWM_Counter_*`) abstracts them and is what the port should use.

Until this lands, `log_port_now_ms()` stays per-core and the skew caveat in D4 stands.

## Open items

- **Ring size** per core: 2 KB (128 records) is the starting guess. Tune from the observed
  overflow counter (D5) rather than by argument.
- **SCB instance + pin routing** to the KitProg3 bridge on each kit.
