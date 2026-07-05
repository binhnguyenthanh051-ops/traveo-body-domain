# ADR-0015: Resource & lifecycle manager (`sysmgr`) and the fail-safe catalog

**Status:** accepted · **Date:** 2026-07-02

## Context

M3 gives the FBL more than one thing to bring up before the diagnostic stack (ADR-0012) can
run — CAN, flash, optionally crypto — with a real ordering dependency ("UDS is only ready
when CAN is ready"). There was no existing mechanism for this; module bring-up was implicit.
This also needs to be the foundation a future **Ignition Manager** builds on (App-side,
later) without a rewrite — the reason to design it now rather than improvise it inside the
FBL's programming-mode loop.

## Decisions

### D1. One module, not AUTOSAR's EcuM/BswM/WdgM split

AUTOSAR separates state-sequencing (EcuM), mode arbitration (BswM), and continuous health
supervision (WdgM) because runtime health monitoring is a genuinely different concern from
one-time boot sequencing. For M3/FBL scope (bring up once, no mid-session runtime health
checks yet) that split is unneeded complexity — one module, `sysmgr`, covers both. **Extension
point, not built now:** if the App later needs continuous runtime health monitoring with
different timing/lifetime than boot sequencing, split it out then.

### D2. Resource descriptor — a plain `_t`, not an `_if_t`

```c
typedef struct {
    sysmgr_res_id_t       id;
    sysmgr_res_id_t       deps[SYSMGR_MAX_DEPS];
    uint8_t                dep_count;
    int                  (*init)(void);          /* non-blocking; 0 = started ok */
    int                  (*poll_ready)(void);     /* NULL if init() is synchronous */
    void                 (*deinit)(void);
    uint32_t               init_timeout_ms;       /* 0 = no timeout */
    uint8_t                max_retries;
    sysmgr_fail_policy_t   on_exhausted;           /* required, no implicit default */
} sysmgr_resource_t;
```

This mixes data (deps, timeout, retry count) with embedded callbacks, so per the naming
convention (`docs/coding-standard.md`) it stays a plain `_t` — `_if_t` is reserved for structs
that are *only* function pointers (a pure vtable, e.g. `can_hal_if_t`).

### D3. Author-ordered table + assertion, not a runtime topological sort

The resource table is written in dependency order by whoever wires it up (composition root,
same pattern as ADR-0004's per-variant config). `sysmgr` asserts each resource's declared
`deps` are already `READY` before starting it — fails loud at init if authored wrong. A real
topological sort (Kahn's algorithm) is graph-algorithm code in a MISRA/no-heap context for a
graph of ~5 nodes; not worth it at this scale. Revisit if the graph grows materially.

### D4. Tick-based sequencer — non-blocking

`sysmgr_tick(uint32_t now_ms)` runs once per FBL super-loop iteration, advancing whichever
resource is currently `INITIALIZING` (checking `poll_ready()`/timeout) before starting the
next one in order. Same idiom as the existing `fbl_dwell_for_tool` (non-blocking dwell,
host-tested with a fake clock) — reused rather than inventing a second style.

### D5. Dependency enforcement is structural, not a consumer-side poll

A resource's `init()` is never called until every resource in its `deps` list reports
`READY`. Consumers (e.g. the diagnostic stack) never manually check "is CAN ready" — by the
time the diag resource's own `init()` runs, CAN is guaranteed ready by construction. This is
the direct answer to "UDS is only ready when CAN is ready."

### D6. Fail policy is per-resource config, not a sysmgr-wide default

```c
typedef enum {
    SYSMGR_FAIL_RETRY_INPLACE,   /* re-call init(), no reset, bounded count */
    SYSMGR_FAIL_SYSTEM_RESET,    /* escalate to fbl_port_system_reset */
    SYSMGR_FAIL_DEGRADE          /* mark UNAVAILABLE, dependents run reduced, no reset */
} sysmgr_fail_policy_t;
```

Every resource entry must declare `on_exhausted` explicitly — there is no implicit default,
so a new resource can't silently inherit the wrong behaviour.

**The FBL's CAN resource is `SYSMGR_FAIL_DEGRADE`, not `SYSTEM_RESET`.** CAN controller init
on this chip (message-RAM partition, bit-timing config, the `CCCR.INIT` handshake — ADR-0011
D1/D2) is a deterministic local register sequence, not something waiting on an external
condition. If it fails, it is a code/config bug or a genuinely dead transceiver — a reset
re-runs the identical sequence and fixes nothing, while burning the shared BREG counter for
no benefit (D8). It is also unnecessary during the knock window specifically: a CAN failure
there is indistinguishable from "nobody knocked" to the existing decision tree (ADR-0008 D2
timeout → continue), so a valid app still boots regardless. `SYSTEM_RESET` remains an
available policy value — it may be the right choice for a future App-side resource with a
different risk/benefit profile — but it is a per-table decision, not hardcoded into `sysmgr`
itself.

### D7. Fail-safe is a named catalog, not a new module

Every module that manages a resource or state transition declares its fail-safe behaviour;
the existing behaviours plus this milestone's addition are recorded as one list, and future
ADRs are held to adding a row here rather than inventing ad hoc recovery:

| # | Trigger | Safe state | Source |
|---|---|---|---|
| 1 | app digest/vector invalid | stay in FBL, never jump | ADR-0008 D1 step 5 |
| 2 | `.noinit` corrupt on a read-cause | reinit to default, continue | ADR-0007 D5 |
| 3 | boot-loop counter > N | forced programming mode | ADR-0007 D4 / ADR-0008 D1 step 2 |
| 4 | critical resource exhausts in-place retries | `SYSTEM_RESET` per D6 (rides #3's counter, accepted conflation) | this ADR |
| 5 | non-critical resource exhausts retries | `DEGRADE`, dependents run reduced | this ADR |

### D8. Accepted scope limit — not solved here

Nothing bounds an infinite reset loop if a `SYSTEM_RESET`-policy resource is permanently
dead — the BREG counter (row 3) only bounds "should we jump to the app," which is moot once
already resident in programming mode. A terminal "give up, sit inert" state would close this
gap but needs its own persisted reason code, which is more machinery than this milestone
warrants. **Deliberately left open** as a portfolio-scope limit, not silently handled — revisit
only if a real need for it appears.

## Host/target split (ADR-0001)

| Host-testable core | Target-only |
|---|---|
| Dependency assertion, state machine per resource, timeout/retry counting | each resource's own `init`/`poll_ready`/`deinit` (e.g. `can_hal_if_t.init`, flash unlock) |
| Fail-policy dispatch (D6) | `fbl_port_system_reset` (existing, reused) |

`sysmgr` itself needs only a fake clock and stub resource callbacks (success/fail/pending on
cue) to host-test — no new port surface of its own.

## Consequences

- (+) One mechanism answers both "sequencing" and "resource readiness" — they were the same
  problem viewed from two angles.
- (+) Forward-compatible with a future Ignition Manager: resources already have symmetric
  bring-up/tear-down (`init`/`deinit`); a group-based `request_up`/`request_down` API is an
  additive extension, not a rewrite.
- (−) Config-authored ordering (D3) pushes correctness onto whoever writes the table — judged
  acceptable given the small, fixed resource count in this project.

## Alternatives considered

- Split EcuM/BswM/WdgM-style modules — rejected for now (D1); the extension point is noted,
  not built.
- Runtime topological sort — rejected (D3); revisit only if the graph grows.
- A dedicated resource-retry counter separate from BREG — rejected (D6/D8 accept the
  conflation); a second counter is more state than the failure mode justifies.
- CAN = `SYSTEM_RESET` in the FBL — rejected on reflection (D6); not a plausible fix for a
  deterministic local init failure.
