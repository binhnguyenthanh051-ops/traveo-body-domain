# ADR-0020: TCB isolation — enforcing the CM0+ crypto boundary with protection units (M4 Seam 5)

**Status:** accepted · **Date:** 2026-07-10 · **Scope revised on the bench**

## Scope as built (M4)

The TCB-isolation claim is carried by **Boundary A alone**, and it is **proven on silicon**:
an SMPU denies the untrusted CM4 (moved to an ordinary protection context) access to the CM0+
image flash that holds the public key; a CM4-side read of that region takes a bus fault, verified
against a walls-off negative control where the same read succeeds. That is the mechanism ADR-0017
reason #2 asserted, now demonstrated.

**Boundary B (PPU over the CRYPTO block) and D5 (locking the walls against CM4) are kept
design-only** — implemented behind default-off flags (`PROT_WALL_CRYPTO`, `PROT_LOCK_MASTER` in
`proj_cm0p/src/prot_config_cm0p.c`) but **not** part of the demonstrated result. Rationale: the CM4
never touches the CRYPTO block directly — all crypto crosses the IPC mailbox to the CM0+ — and the
only persistent secret (the key) is already walled by Boundary A, so a CRYPTO wall is redundant
defense-in-depth for M4. It earns its place at **M5**, whose runtime MAC key would live in the
engine. The design detail in D2–D6 below is retained as the reasoning for that future work; where
it describes the CM4 core-MPU rejection (D1) and the SMPU (D3), it is what was built.

*(Not chased further on the bench: the fixed-PPU per-PC att model and the master-struct lock both
have silicon quirks that were disproportionate to walling a resource the CM4 cannot reach anyway.)*

## Context

ADR-0017 offloaded crypto to the CM0+ for three named reasons; reason #2 was **TCB
isolation** — "the crypto engine + key storage stay unreachable from the FBL's large attack
surface (the CAN/UDS parser), so a memory bug there can't reach crypto state or key material."
Through Seam 4 that was an **assertion**: the code merely *doesn't* have the CM4 touch the
Crypto block or the key. "No code path reaches it" is not a boundary — a memory-safety bug in
the CM4 FBL's attacker-reachable CAN/UDS parser (buffer overrun, wild pointer) could scribble
anywhere the *bus* lets CM4 go. Seam 5 makes the boundary real and demonstrated.

**The claim, stated precisely for M4's honest threat** (not an imagined runtime-compromised
CM4 — see ADR-0017's own red-team):

> A wild write or read from buggy CM4 firmware **cannot** reach the public key or the CRYPTO
> registers, because the **bus fabric** refuses the transaction — independently of anything
> CM4's own code does. The wall holds even when the CM4 program is the adversary's.

"Independently of CM4's own code" is the whole design driver, and it is exactly what the lazy
answer (the CM4 core MPU) cannot provide.

**Honesty carried into the write-up (continuing ADR-0016 D4 / ADR-0017's candour).** The public
key is **not secret** (ADR-0017 Context). So Boundary A's security *substance* is **integrity**
— deny CM4 *write* so a parser bug can't corrupt the key that gates the boot decision. The
*read*-deny is the easily-observable bench demo and the capability M5 genuinely needs for its
**secret** MAC key. We say that rather than dress a public key up as a vault.

**Scope reality (consistent with the standing lifecycle decision).** We run in NORMAL
lifecycle, no eFuse. Seam 5 proves the **mechanism works at runtime**, not a fused production
lockdown. Two consequences are named up front, not discovered: (1) **PC0 is bypass** — a master
in protection context 0 is not subject to SMPU/PPU checks, so the setup is as much about
*assigning contexts* as writing region rules; (2) **the debugger (DAP) is its own bus master,
PC0 in NORMAL lifecycle** — which is *why* our `mdw` dumps have always worked. The Seam-5 demo
therefore **cannot** be an OpenOCD read of the key (that bypasses the wall); the denial must be
triggered by **CM4 code** and observed via CM4's fault handler.

## Decisions

### D1. Enforce at the bus fabric (SMPU/PPU), never the CM4 core MPU

The boundary is built from **SMPU** (Shared Memory Protection Unit, for the key in flash) and
**PPU** (Peripheral Protection Unit, for the CRYPTO MMIO) — units configured by the trusted
CM0+ and checked by the bus on every access regardless of origin.

*(Family note, resolved: the build targets `COMPONENT_CAT1A` / device `CYT2B75CAS`, so despite
being TRAVEO T2G this part uses the PSoC6-style **CAT1A** `cy_prot` API — `PROT_SMPU_SMPU_STRUCTn`
+ `cy_stc_smpu_cfg_t`, and the `PERI_MS` att-based fixed/programmable PPUs — not the CAT1C
`PERI_MS_v3/v4` variant. The scaffold in `proj_cm0p/src/prot_config_cm0p.c` is written against
that API.)*

- **Alternative — program CM4's ARMv7-M core MPU to forbid the regions.** Rejected as the
  *security* boundary: CM4 administers its own MPU, so buggy/hostile CM4 code just reprograms
  it. It is a **safety** net (catches accidental bugs early), not a boundary against a hostile
  transaction. It may be added later as defense-in-depth; it is **not** the mechanism.

### D2. Protection contexts — CM0+ in PC0 (secure), CM4 in ordinary PC2 — set before the CM4 runs

*(Revised after reading the TRM protection-units chapter §6.3 — the original "give each core its
own PC, reserve PC0" plan didn't survive contact with the silicon.)* The TRM shows **PC0 and PC1
are hardware-special, CM0+-only contexts**: PC1 is the trusted-ROM syscall context, entered *only*
on a trusted interrupt and attainable *only* by the CM0+ (§14.3); the `PC_SAVED`/PC0 machinery
"is only present for the CM0+ master" (§6.3.1). So a plain `SetActivePC` cannot put the CM0+ crypto
service into PC1, and **CM4 has no hardware path to PC0 or PC1 at all.**

The arrangement is therefore:

- **CM0+ stays in its boot PC0** — the unrestricted secure/manager context. It reads the key and
  drives CRYPTO by the PC0 bypass (the TRM grants PC0 unrestricted access to every SMPU/PPU), so it
  needs no allow-rule and cannot self-brick. We deliberately do **not** rewrite CM0+'s `MSx_CTL`
  (that would risk clearing its own PC0 permission).
- **CM4 is moved to ordinary PC2**, mask = PC2-only. The Door-1 clamp then makes any CM4 attempt to
  program PC0/PC1 a no-op, and those contexts are CM0+-only regardless.

The walls thus only ever need to **deny CM4's PC**, not allow CM0+'s. This is set in `main_cm0p.c`
**before `Cy_SysEnableCM4()`** — CM4 is in reset the whole time, so no window exists where its PC
is live but a wall is down.

Rejected alternative (**both cores in ordinary PCs**, CM0+→PC2 / CM4→PC3): preserves a "no core
runs in PC0" purity, but the TRM makes PC0 the natural home of the secure CPU anyway, and moving
CM0+ out reintroduces a self-brick risk (losing execute on its own flash if the allow-struct
priority is wrong) and forces re-validating the SROM flash-syscall servicing (the S0-4 path) from a
non-PC0 context. Not worth it — the CM4-denial claim is identical either way.

The same boundary serves M5 for free: the FBL and the app both run as the **CM4** master (the
FBL jumps to the app in-core), so the app — the attacker-reachable code that must not hold the
runtime MAC key — inherits the identical wall and will have to request MAC ops over IPC. One PC
split, two milestones.

### D3. Boundary A — SMPU over the CM0+ image flash (the public key)

`crypto_pubkey_dev[]` is compiled into the CM0+ image, so it lives in the CM0+ code-flash region
(`0x1000_0000 .. 0x1002_0000`, `FLASH_CM0P_SIZE = 0x2_0000`; overview §6 / `fbl_cm4.ld`). With CM0+
in the PC0 bypass (D2), a **single** SMPU slave struct suffices: it **denies the whole 128 KB to
every non-zero PC** (so CM4's PC2 is refused), while CM0+'s PC0 accesses skip SMPU evaluation and
keep read/execute. No separate allow-struct and no deny-background/allow priority to get right —
the earlier two-struct scheme was only needed when CM0+ ran in a non-zero PC. CM4 never reads the
CM0+ image, so this costs the FBL/app nothing.

Substance = **integrity (write-deny)**; **read-deny** is the demo + the M5-relevant capability
(D-honesty above). Region granularity: SMPU regions are power-of-2 sized and naturally aligned
with an 8-way subregion-disable mask — the region descriptor is computed from the flash-map
constants by a **host-tested pure helper** (see Host/target split), so the alignment/size
encoding is verified off-target.

**Known limitation (noted, not yet addressed) — key lives in CM0+ *code* flash.** Compiling the
key into the CM0+ image is fine for a *public* key at this stage, but it is inflexible: rotating
the key means rebuilding and re-flashing the whole CM0+ image. The better long-term home is a
**dedicated work-flash region** owned and protected by CM0+ only (SMPU'd the same way), so the
key is data that can be re-provisioned without a code rebuild — and it becomes the natural place
for the M5 *secret* MAC key. The wall mechanism here is identical either way; only the region's
`{base,len}` changes, which is exactly what the host-tested helper already parametrises. Deferred
as out-of-scope for Seam 5 (would pull in a key-provisioning path); revisit at M5.

### D4. Boundary B — PPU over the CRYPTO peripheral MMIO *(DEFERRED — design-only, see Scope)*

A PPU over the CRYPTO register block allows only the CM0+ PC; a CM4 register access faults. CM4
never touches CRYPTO directly (all crypto crosses the IPC mailbox to CM0+, ADR-0017/0018), so
this is free to the FBL/app.

- **Fixed vs programmable PPU** *(verify in TRM: whether CYT2B7 exposes a dedicated fixed PPU
  region for the CRYPTO group).* Prefer the **fixed** PPU (purpose-built, one struct); fall back
  to a **programmable** PPU over the CRYPTO base/size if no fixed region covers it exactly.
  **RESOLVED from the local PDL device header (`cyt2b75cas.h`, mtb-pdl-cat1 v3.22.1):** fixed
  PPUs exist for the whole CRYPTO block, in fact split into six sub-region structs —
  `PERI_MS_PPU_FX_CRYPTO_{MAIN, CRYPTO, BOOT, KEY0, KEY1, BUF}`. We wall all six (CM4 needs none),
  so no programmable-PPU address math is required for Boundary B. *(Still bench-verify that a fixed
  slave att with `pcMask = CM0+ only` denies a CM4-PC access.)*

### D5. Lock the units against the untrusted core *(DEFERRED — design-only, see Scope)*

Each SMPU/PPU pair has a *master* sub-struct guarding its own configuration. These are locked to
the **CM0+ PC** so CM4 cannot disable or rewrite the walls, and CM4 must be unable to re-enter
PC0. Without D5, D3/D4 are theatre — CM4 would just turn them off. This decision is the
difference between "configured" and "enforced," and it is the headline of the seam.

**TRM detail that shapes the lock (confirmed from the owner's TRM excerpts).** A protection
struct's *master* structure has its read attributes (`UR`/`PR`) hard-wired to `1` — reading a
struct's own config is *always* allowed. So the lock cannot be a read-deny; it is strictly a
**write-deny** keyed on the master struct's `pcMask`. More importantly, **PC0 is defined to have
*unrestricted* access to every SMPU/PPU struct**. The entire boundary therefore collapses to a
single invariant: **CM4 must never be able to make its active PC 0.** That resolves into two
"doors," and the TRM's protection-context section settles both:

- **Door 1 — the active-PC field `PROT_MPUx_MS_CTL.PC[]`** (which a master writes to change its
  own PC). *Sealed by hardware:* the TRM states that programming `PC[]` to a value whose
  `PROT_SMPU_MSx_CTL.PC_MASK` bit is `0` leaves `PC[]` **unchanged**. So once CM0+ clears PC0
  (and all but PC2) from CM4's mask, CM4's own `SetActivePC(0)` is a silent no-op. No extra lock —
  the mask *is* the lock.
- **Door 2 — the mask register `PROT_SMPU_MSx_CTL.PC_MASK`** itself. The TRM: it "is controlled by
  the **secure CPU** and has the same access restrictions as the SMPU registers." So the seal is
  the secure/non-secure split: **CM0+ must be the secure master (`NS=0`), CM4 non-secure (`NS=1`)**,
  configured via `Cy_Prot_ConfigBusMaster`. A CM4 write to `MS_CTL` to re-add PC0 is then a
  non-secure write to a secure-CPU register → denied. The one remaining silicon check is to
  *confirm* that CM4 write is actually refused.

**Ordering (implementation finding).** All of this is written by CM0+ while it is in its reset
**PC0** — the unrestricted context is the only reason it can touch the SMPU/PPU/`MSx_CTL`
registers. Because CM0+ **stays** in PC0 (D2), the self-brick that would have come from moving the
trusted core into its own walled PC does not arise — a direct benefit of the revised PC plan. The
only ordering rule left is the obvious one: put the walls and locks up, then move CM4 to PC2, all
before `Cy_SysEnableCM4` (CM4 is in reset throughout). CM0+'s own `MSx_CTL` is deliberately left
untouched so its PC0 permission is never disturbed.

### D6. Enforcement is bench-verified; only the descriptor math is host-tested

There is no host fake for the bus fabric, so unlike every prior seam the *enforcement* proof is
**on-silicon**, not Unity:

- **Negative control:** CM0+ reads the key / drives Crypto → still works (every prior seam).
- **The demo:** a **debug-build-only** CM4 routine deliberately reads a key byte and pokes a
  CRYPTO register → **BusFault/HardFault on CM4**, caught by CM4's fault handler (which we *can*
  observe over OpenOCD). Before the walls the access succeeds; after, it faults. That delta is
  the proof. The access is **CM4-initiated**, never an `mdw` (the DAP bypasses, D-scope).

The one host-testable slice is a pure function computing the protection-region descriptor
(base / size-code / subregion mask / attributes / PC-mask) from the flash-map constants — its
Unity test asserts the region fully covers the key extent, is correctly aligned, and encodes
CM0+-only. We build that to honour the rhythm for the part that *can* be tested, and state
plainly that enforcement itself is bench-only.

## Host/target split (ADR-0001)

| Host-testable core | Target-only (bench-verified) |
|---|---|
| Region-descriptor math: `{base,len} → {addr, size-code, subregion mask}` | SMPU/PPU/PC configuration (`Cy_Prot_*`, mtb-pdl-cat1) |
| Attribute/PC-mask encoding = "CM0+ only" | The bus-fabric denial itself (no host model) |
| Coverage + alignment assertions over the real flash-map constants | The CM4 fault-demo + fault-handler capture |

## Consequences

- (+) ADR-0017 reason #2 stops being an assertion — a buggy CM4 parser provably can't reach the
  key or the engine; the TCB is a boundary, not a convention.
- (+) The PC split is M5-ready: the app inherits the wall, forcing it through the IPC service for
  MAC ops — the runtime-secret-key case ADR-0017 offloaded *for*.
- (+) Zero cost to the FBL/app: neither ever legitimately touches the walled regions.
- (−) The CM0+ boot path gains protection-unit setup that must run **before** `Cy_SysEnableCM4`
  and be **locked** — a new ordering/locking constraint to get right (D2/D5).
- (−) The seam's core proof is bench-only (D6) — less regression-proof than a host suite; the
  descriptor helper recovers only the encoding math.
- (−) NORMAL-lifecycle: the DAP still bypasses (D-scope). We demonstrate the mechanism, not a
  fused lockdown; production SECURE-lifecycle fusing is design-essay, not built.

## Alternatives considered

- **CM4 core MPU for both regions** — rejected (D1): self-administered, a safety net not a
  boundary; optional defense-in-depth only.
- **Configure regions but skip PC separation/locking (demo-grade)** — rejected: the wall would
  be bypassable by CM4 itself, so the blog claim would be false; we'd have to disclaim it.
- **Wall only one boundary (engine *or* key)** — rejected: the TCB claim is about both crypto
  state *and* key material; half a wall is a half-truth.
- **Move the key out of flash into an eFuse/SFlash secret** — out of scope (standing
  no-eFuse decision) and unnecessary for a *public* key; the integrity wall is the right tool.

## To verify in the TRM / on silicon

*Resolved from the TRM/PDL this session (no longer open):* PC plan (D2 — CM0+ PC0, CM4 PC2; PC0/PC1
are CM0+-only special contexts); fixed PPUs cover CRYPTO exactly (D4 — six `PPU_FX_CRYPTO_*`);
Door 1 clamp (a masked PC write is a no-op); single-deny-struct suffices (D3, no priority ordering
to settle). Remaining bench checks:

- **(headline)** Door 2: with CM4 non-secure and CM0+ the secure CPU, confirm a CM4 raw write to
  `PROT_SMPU_MS14_CTL` (to re-add PC0) is refused / leaves `PC_MASK` unchanged. If PC0 ever becomes
  reachable by CM4, every wall and lock is void (PC0 is unrestricted by design).
- That CM0+ is already the **secure** master by default in NORMAL lifecycle (so Door 2 holds
  without us rewriting CM0+'s `MSx_CTL` and risking its PC0 permission). If not, find a way to mark
  CM0+ secure that leaves PC0 intact.
- That the fixed-PPU slave att with `pcMask = NONE` denies a CM4 (PC2) CRYPTO access while PC0
  (CM0+) still drives it — i.e. the PC0-unrestricted rule behaves as the TRM states.
- That the CM4 fault-demo faults *after* setup and succeeds *before* it (D6), observed via CM4's
  fault handler, not the DAP.

## Review history

Design-reviewed with the project owner (this session): scope = **both** boundaries (crypto PPU +
key SMPU); rigor = **full** (separate PCs + locked master structs). Recommendations D1 (bus
fabric over core MPU), D2 (two PCs before CM4 start), D5 (lock or it's theatre), and D6 (bench
proof + one host-tested helper) accepted. Descriptor helper + failing Unity test next, then the
CM0+ config and the CM4 fault-demo bring-up. Silicon findings appended here.

**TRM review (same session, after the CM0+ config scaffold).** Read the body-controller TRM
protection-units chapter (§6.3–6.4, §14.3) directly. Two decisions changed: (1) **D2 revised** —
PC0/PC1 are hardware-special CM0+-only contexts (PC1 = trusted-ROM, CM0+-only; PC0 machinery CM0+-
only), so the "each core its own PC" plan is replaced by **CM0+ stays in PC0, CM4 → ordinary PC2**;
owner chose this (recommended) over "both cores in ordinary PCs." (2) **D3 simplified** to a single
deny struct (CM0+'s PC0 bypass removes the need for a deny-background + allow pair). Door 1 (active-
PC clamp) confirmed sealed by hardware; Door 2 (mask register) sealed by the secure/non-secure
split — one bench check left. `prot_config_cm0p.c` scaffold updated to match.

**Bench outcome + scope decision (same session).** On silicon both CPUs actually run in **PC2**
(the DAP is the PC0 our dumps used), so the final arrangement is **CM0+ left in PC2, CM4 moved to
PC3**, and Boundary A is an **SMPU in match mode** that denies only the CM4's PC (the CM0+ falls
through). That wall is **proven** — a CM4 read of the key region faults, with a clean walls-off
negative control. The CRYPTO PPU (D4) and the master-struct lock (D5) hit device-specific quirks
and were **descoped to design-only** (default-off flags) as redundant defense-in-depth — see
"Scope as built" at the top. Seam 5 closed on Boundary A.
