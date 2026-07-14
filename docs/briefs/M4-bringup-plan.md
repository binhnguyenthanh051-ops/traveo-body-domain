# M4 on-board bring-up plan

Where M4 stands going into this: the architecture is agreed (ADR-0016–0019), the host-testable
core is implemented and green (`shared/crypto`, 22 Unity tests + `host_tools/sign_image.py`),
and every host test already proves the fail-safe holds against a stub M0+ — timeout, malformed
reply, and busy mailbox all collapse to "never trust" before a single line of target code exists
(ADR-0016 D5). This is the step-4-equivalent of `M1-implementation-sequence.md`: what's left is
target-only, behind the ports the design deliberately left open, and it's sequenced one seam at a
time — same discipline M3's bring-up log used — so a fault stays answerable to "which seam?"
rather than "somewhere in the new dual-core thing."

## What bring-up found that the design session didn't know yet

Digging into the actual BSP before writing this plan surfaced two facts that change *how* M4
gets built, not *what* it verifies:

**The CM0+ image is not a separately-flashed binary — it's a section linked into the FBL.** Both
cores share one code flash; the CM4 linker
(`node_a_gateway/bootloader/bsps/.../COMPONENT_CM4/TOOLCHAIN_GCC_ARM/linker.ld`) reserves
`FLASH_CM0P_SIZE = 0x20000` (128 KB) at the flash origin (`0x1000_0000`) for a `.cy_m0p_image`
section, and CM4 code starts right after it. So "own a custom M0+ image" (ADR-0017 D4) means:
**replace what fills `.cy_m0p_image`**, and the crypto service ships as part of the FBL image,
flashed as one combined binary via the existing `zz_build_gateway_fbl.sh program` — no new flash
step. This also confirms ADR-0019 D2 is the right call: the public key anchors to the same image
the ROM starts first.

**The vendor already ships the exact offload this project designed.** `cat1cm0p` (the BSP's CM0+
asset library) has a `COMPONENT_CM0P_CRYPTO` prebuilt — a working M0+ crypto *server*, with its
own IPC protocol — sitting right next to the `COMPONENT_CM0P_SLEEP` prebuilt currently selected
in `bsp.mk`. That's a real fork to resolve before writing any target code:

| | What it is | Verdict |
|---|---|---|
| (a) Vendor `CM0P_CRYPTO` + PDL client calls | Infineon's own IPC protocol end to end | fastest, but bypasses `crypto_msg`/`ipc_mailbox`/`crypto_dispatch` entirely — none of ADR-0017/0018 gets demonstrated |
| (b) Fully custom CM0+ + hand-written SHA/ECDSA | our protocol, our crypto | rejected outright — ADR-0006 forbids hand-rolled crypto |
| **(c) Custom CM0+, our protocol, PDL `Cy_Crypto_Core_*` driver calls underneath** | our IPC design, vendor-vetted primitives | **selected** |

**Seam 0 below is where this decision becomes real** rather than assumed. (c) is the honest
middle: the architecture on display (the layered service, the mailbox, the fail-safe composition)
is ours and load-bearing; the actual SHA-256/ECDSA math is the vetted PDL driver, exactly as
ADR-0006 requires. (a) is worth naming in the blog as the "buy vs. build" alternative — it's a
slightly uncomfortable fact that the vendor already sells this offload, and saying so plainly is
better than pretending it doesn't exist.

## Sequencing principle

Same as M1/M3: **settle what doesn't need the board first, prove one new mechanism per seam, and
never let a seam depend on something the previous seam didn't already establish.** The seams below
are ordered by that dependency chain — mailbox mechanics before crypto content, crypto content
before the real trust decision, the trust decision before the isolation and fault-injection seams
that only make sense once trust actually works.

---

## Seam 0 — resolve the CM0+ project structure (no crypto yet)

**Goal:** replace `COMPONENT_CM0P_SLEEP` with *our* minimal CM0+ image and prove the FBL still
boots and starts the CM4 exactly as before — i.e. prove D4's non-negotiable ("preserve the
existing responsibility") before adding anything to it.

- Decide the MTB project shape: a two-project `APPLICATION` (`proj_cm0p` + `proj_cm4`, the BSP's
  existing `COMPONENT_CM0P/` scaffolding suggests this is the intended path) vs. injecting a
  prebuilt `.cy_m0p_image` blob into the current single-Makefile `COMBINED` FBL build. Prefer the
  two-project layout — it's the natural home for `shared/crypto`'s M0+-side dispatch loop and
  scales better once M5 adds an op.
- Minimal CM0+ content at this seam: `cybsp_init()` equivalent + whatever starts the CM4 (today
  done implicitly by the vendor prebuilt) — nothing else. No IPC, no crypto.
- **(verify in TRM/board)** `CY_USING_PREBUILT_CM0P_IMAGE` vs building CM0+ fresh each time —
  confirm which `cybsp.c` path this project should take instead of assuming.

**Exit:** `zz_build_gateway_fbl.sh program` flashes a combined image whose CM0+ half is *ours*
(not the vendor sleep prebuilt) and the board boots to the app exactly as it did at the end of M3
— no regression, no functional change yet. If this doesn't hold, nothing downstream is trustworthy.

**Scaffolded (not yet board-verified):** `node_a_gateway/bootloader/` is now an MTB
`APPLICATION` with two `PROJECT`s — `proj_cm4/` (the existing FBL, moved in place, paths
fixed) and `proj_cm0p/` (new: `Cy_SysEnableCM4()` + idle, using the BSP's own
`COMPONENT_CM0P` linker/startup unmodified). `DISABLE_COMPONENTS=CM0P_SLEEP` in both
projects' Makefiles, since the vendor prebuilt and `proj_cm0p` would otherwise both claim
the same flash region. No edit to `fbl_cm4.ld` was needed — its `.cy_m0p_image` output
section is simply empty in the CM4 ELF now, and CM4 `.text` still starts at
`ORIGIN(flash) + FLASH_CM0P_SIZE` exactly as before.

**Structure + build validated on MTB 3.8** (2026-07-06): `make get_app_info` reports the app
root as `MTB_TYPE=APPLICATION` / `MTB_PROJECTS=proj_cm0p proj_cm4`, `proj_cm4`→`CM4`,
`proj_cm0p`→`CM0P`, both with `MTB_DISABLED_COMPONENTS=CM0P_SLEEP` (vendor prebuilt excluded,
so `proj_cm0p` fills the CM0+ region) — and **both projects then `make build` clean** (compile
+ link), including `proj_cm0p/src/main_cm0p.c`'s `cybsp_init()` / `Cy_SysEnableCM4()`. Two
break-and-fix notes for the record: (1) the initial "older build flow (2.1 or earlier)" error
was a missing `CY_GETLIBS_SHARED_PATH` at the app root — `start.mk` errors when it's empty —
plus a stale generated `libs/app.mk` carried from the COMBINED layout; (2) `getlibs` can't
reach the Infineon super-manifest through the site firewall (`raw.githubusercontent.com`
blocked), but it still regenerates each project's `libs/mtb.mk` offline from the already-locked
`mtb_shared` assets before failing on the online refresh — so `make build` works without a
successful `getlibs`. **Still open — the true Seam-0 exit:** on-board flash of the combined
image + confirm the board boots the app exactly as it did at the end of M3 (no regression).
That needs the bench; the code/build side of Seam 0 is done.

**Bench finding S0-1 (CM4 vector-table address).**
- *Expected:* our CM0+ calls `Cy_SysEnableCM4()` and the CM4 starts the FBL, same as the vendor
  prebuilt did through M1–M3.
- *Silicon:* immediate `[traveo2.cpu.cm4] clearing lockup after double fault`, HardFault with
  `pc 0x00006b52` — a low address nowhere near the CM4 image at `0x1002_0000`, i.e. the CM4
  fetched its MSP/reset vector from the wrong place and ran garbage. `main_cm0p.c` passed
  `Cy_SysEnableCM4(0x00020000)` — just the CM0+ region size. The PDL param is misleadingly
  named `vectorTableOffset`, but its own doc defines it as the offset "from memory address
  0x00000000" (i.e. the **absolute** address) and writes it straight into
  `CPUSS->CM4_VECTOR_TABLE_BASE`. So the CM4 was pointed at `0x0002_0000`, not `0x1002_0000`.
- *Fix:* pass the absolute `flash_base + CM0+_region = 0x1000_0000 + 0x20000 = 0x1002_0000`
  (`CM4_VECTOR_TABLE_ADDR`). The vendor prebuilt had always passed the full address; our
  minimal replacement dropped the flash base.
- *Lesson:* when you take over a responsibility a vendor blob was doing (starting the other
  core), the blob's *correct* parameter values leave with it — and a param named "offset" that
  the docs define as an absolute address is exactly the kind of thing that boots on the vendor's
  value and double-faults on a plausible-looking wrong one. The build compiled clean (the whole
  Seam-1 scaffold does); this was purely a runtime value.

**Bench finding S0-2 (CAN went dark — peripheral-config ownership shifted cores).**
- *Expected:* CAN keeps working after the split, as through M1–M3.
- *Silicon:* CAN dead — not even a bring-up echo. Cause: the BSP applies the Device
  Configurator's peripheral setup (CANFD clock, pins, peripheral — `design.modus`) inside
  `cybsp_init()` via `cycfg_config_init()`, gated in `cybsp.c` to exactly one core:
  `#if defined(CORE_NAME_CM0P_0) || … || (defined(CORE_NAME_CM4_0) && defined(CY_USING_PREBUILT_CM0P_IMAGE))`.
  In the M1–M3 COMBINED project (prebuilt CM0+), `CY_USING_PREBUILT_CM0P_IMAGE` was defined, so
  the **CM4** ran it. After the Seam-0 split, proj_cm4 no longer uses a prebuilt, so that macro
  is undefined and the gate hands the config to the **CM0+** — the CM4's `cybsp_init()` now
  *skips* it. `port_can.c` only calls `Cy_CANFD_Init()`; it never configured the clock/pins
  itself (it relied on cycfg), so CAN lost its clock/pins.
- *Fix:* call `cycfg_config_init()` explicitly on the CM4 in `fbl_main()` after `cybsp_init()` —
  the core that owns CAN configures CAN, restoring M1–M3 behaviour. Idempotent, and it runs
  after the CM0+ released the CM4 so it is the final word on those pins/clocks.
- *Lesson:* splitting a prebuilt-CM0+ project silently moves *every* cycfg-driven peripheral,
  not just the crypto you set out to add — the config was invisible because a define you didn't
  know about was steering it. When a peripheral dies right after a dual-core restructure, suspect
  `cycfg_config_init()` ownership before the peripheral driver.

**Bench finding S0-3 (CM0+ busy-poll wedged the CM4's flash erase).**
- *Expected:* UDS reprogramming (erase → download → program) works as in M3.
- *Silicon:* the erase step hung the CM4 (FBL LED froze on); after reset the old app still ran
  (erase never completed). Cause: this part faults on an instruction fetch from a flash macro
  that is mid-erase (M3 Seam-7 #11, now on the other core). Our custom CM0+ idled by
  busy-polling the mailbox from a tight loop *in its own flash*; the moment the CM4 began erasing
  the app region, the CM0+'s instruction fetch hit the busy macro and faulted/stalled, wedging
  the operation. The vendor CM0+ prebuilt avoided this by sleeping — it wasn't fetching flash.
- *Fix:* move the CM0+'s idle spin into RAM (`CY_SECTION_RAMFUNC_BEGIN`, `.cy_ramfunc` — copied
  to RAM at startup, already supported by the CM0+ BSP linker) **plus `__attribute__((noinline))`**:
  the first attempt was a no-op because the optimizer inlined the one-call `static` spin back into
  `main()` (flash), so the section placement had no effect — a known ramfunc gotcha. Forced
  out-of-line, the spin body genuinely runs from RAM: during a CM4 flash op the CM0+ fetches
  nothing from flash. Control only returns to the flash-resident dispatch when a request is
  actually pending, which never coincides with a flash op (verify runs at boot / after
  transferExit). CM0+ interrupts stay masked, so no flash-resident ISR is fetched either.
  *Verify it landed:* the `cm0p_wait_for_request` symbol must resolve to a RAM address
  (`0x0800_xxxx`) in the CM0+ `.map`, not flash (`0x1000_xxxx`).
- *Lesson:* on a shared-flash asymmetric dual-core, "the other core is just spinning harmlessly"
  is false during flash program/erase — *any* core fetching from the busy macro faults. A core
  that must stay live across the other's flash writes has to run that code from RAM (or sleep,
  as the vendor blob did). This is the flip side of M3 #11's single-core interrupt-mask fix.

## Seam 1 — the mailbox, empty-handed

**Goal:** prove the ADR-0018 transport mechanics on real silicon before any crypto content rides
on them — the cross-core analogue of M3 Seam 2 (echo ISO-TP before trusting UDS on top of it).

- Wire the real `ipc_port_if_t`: HW semaphore acquire/release, the shared-RAM mailbox region, the
  IPC channel notify with the request-length doorbell (the `notify(size_t req_len)` /
  `response_len()` split settled during host implementation — see ADR-0018 review history).
- Test: CM4 writes a known byte pattern, notifies with a length, CM0+ echoes it back unmodified.
  Confirm `fake_ipc_reset`'s host-side protocol (acquire→write→notify→wait→read→release) matches
  what the real semaphore/channel actually do.
- **(verify in TRM/board)** which IPC channel + semaphore index are actually free (PDL reserves
  some for syscall/PIPE use); the mailbox's placement in a SRAM region both linker scripts
  reserve and neither zero-inits under the other (ADR-0018 D5) — coordinate against
  `tviibe_partition.h`.

**Exit:** a byte round-trips CM4→CM0+→CM4 through the real semaphore/channel/notify, and
`ipc_transact()` (unmodified from the host-tested version) returns `IPC_OK` with the right bytes.

**Scaffolded (build-pending, then bench):** grounded in the PDL for this part —
`CY_IPC_CHAN_USER == 4` on CYT2B7 (chans 0–3 are SYSCALL_CM0/CM4/DAP + SEMA), and one IPC
channel supplies all three ADR-0018 D1 roles (its hardware **lock** = the mailbox-ownership
semaphore, its data register + `AcquireNotify` = the doorbell). Files:
`shared/crypto/include/ipc_mailbox_map.h` (shared address `0x0801_F500` + `{status,len,payload}`
layout, both cores include it); `proj_cm4/src/port_crypto.c` (+`fbl_crypto.h`) binds the
host-tested `ipc_port_if_t` to `Cy_IPC_Drv` on channel 4 and adds `fbl_crypto_bringup_echo()`;
`proj_cm0p/src/main_cm0p.c` grows a poll-and-echo server after `Cy_SysEnableCM4`; `fbl_cm4.ld`
reserves the mailbox as a pinned NOLOAD region directly below `.noinit` (only the CM4 linker
reserves it — CM0+ reaches it by absolute address), with cross-image ASSERTs. `ipc_mailbox.c`
joins `proj_cm4`'s build; both Makefiles gain `shared/crypto/include`. The host transport +
its tests are unchanged and still green.

**Exit MET on silicon.** After the S0-1 vector fix, `fbl_crypto_bringup_echo()` returns `true`
on the board — the 8-byte pattern round-trips CM4→CM0+→CM4 through the real hardware, so all
three open checks resolved at once: (1) IPC channel 4 is free (the `Cy_IPC_Drv_LockAcquire`
succeeded — no CyPipe/DDFT contention); (2) both cores read/write the pinned mailbox at
`0x0801_F500` with no default protection-unit block and no linker collision; (3) the
host-tested `ipc_transact()` behaves on silicon exactly as the fake modelled it — `IPC_OK`,
correct bytes, correct length. **The host transport core did not change between x86 and the
board** — the only silicon fault (S0-1) was in a target binding, the pattern M3 established.
Still a deliberate refinement, not a blocker: the CM0+ **polls** the mailbox; the
`AcquireNotify` ISR (letting it sleep between requests) lands later — the doorbell is already
rung by the CM4 side, so it's a receiver-side change only.

## Seam 2 — a real crypto primitive over the mailbox

**Goal:** prove the dispatch/op layer plus the PDL crypto driver, still without touching flash or
the trust decision — isolate "does our crypto op work" from "does verifying a real app work."

- Wire `CRYPTO_OP_HASH`: CM4 sends a buffer, `crypto_dispatch` on the CM0+ routes to a handler that
  calls `Cy_Crypto_Core_Sha` (or the block's SHA-256 entry point), returns the digest.
- Compare against a host-computed SHA-256 of the same bytes (`hashlib.sha256`) — an exact-match
  assertion, not just "didn't crash."
- **(verify in TRM/board)** the Crypto block's init sequence and buffer-alignment/size
  constraints for SHA-256 on this part.

**Exit:** `crypto_dispatch(CRYPTO_OP_HASH, ...)` on real silicon returns a SHA-256 digest that
matches a host reference for several buffer sizes (0 B edge case, one block, multi-block).

**Scaffolded (build-pending, then bench).** The crypto block is confirmed present on CYT2B7 —
`CPUSS_CRYPTO_PRESENT=1`, `CRYPTO_V2`, and the PDL ships `Cy_Crypto_Core_Sha` (+ ECC for Seam 3),
so ADR-0006's "wrap a vetted primitive" is concrete. Files: `proj_cm0p/src/crypto_ops_cm0p.c`
(the target-only SHA-256 handler — `Cy_Crypto_Core_Enable` once + `Cy_Crypto_Core_Sha(CRYPTO,
…, CY_CRYPTO_MODE_SHA256)` — behind the host-tested `crypto_handler_if_t`); `main_cm0p.c` now
enables the block, binds the op table, and feeds the mailbox envelope to `crypto_dispatch`
instead of echoing; `proj_cm4/src/port_crypto.c` gains `fbl_crypto_bringup_hash()` (frames a
HASH request for the NIST `"abc"` vector, checks the digest == the known
`ba7816bf…f20015ad`). `crypto_dispatch.c` + `crypto_msg.c` join the CM0+ build; `crypto_msg.c`
joins the CM4 build; `crypto_types.h` gains `CRYPTO_SHA256_DIGEST_LEN`. The host core stays
green, and the in-place `req==resp` dispatch the CM0+ relies on is verified on host (decode
copies the request before writing the response).

**Exit MET on silicon.** `fbl_crypto_bringup_hash()` returns `true` on the board — the CM0+'s
`Cy_Crypto_Core_Sha` (V2) computes SHA-256(`"abc"`), the digest crosses the mailbox, and it
matches the NIST reference `ba7816bf…f20015ad` byte-for-byte. So the full offload path is proven:
CM4 frames a request → transport → CM0+ `crypto_dispatch` → HW crypto primitive → digest back.
The `Cy_Crypto_Core_Enable`/`Sha` V2 sequence worked first try; the host-tested `crypto_dispatch`
+ `crypto_msg` again did not change between x86 and silicon — only the target op handler
(`crypto_ops_cm0p.c`) is new. The Seam-1 `fbl_crypto_bringup_echo()` was retired (the CM0+ now
runs dispatch, so a raw pattern → malformed envelope → ERROR, not an echo — a `false` there is
now *expected*, so the dead scaffold was removed to avoid confusion). Follow-ups, non-blocking:
extend to the 0-byte and multi-block SHA vectors; the `AcquireNotify` ISR still deferred.

## Seam 3 — `VERIFY_IMAGE`, against a bench image, not the live app

**Goal:** the actual authenticity check (ADR-0016 D1/D2, ADR-0017 D1) — but on a throwaway flash
region first, so a bug here can't strand the board's ability to boot its real app.

- Wire the CM0+ to **read app flash directly** (ADR-0017 D1's coarse-RPC dependency) — the
  TRM-pending item from ADR-0017. If the CM0+ cannot read-map the app region, fall back to the
  streaming variant named in that ADR (CM4 feeds blocks) — decide this here, not later.
- Wire `crypto_keystore` with the real embedded public key (ADR-0019 D2) and `CRYPTO_OP_VERIFY_IMAGE`
  calling the PDL ECDSA-verify entry point.
- Test against `host_tools/sign_image.py` output: flash a `sign_image.py`-signed test blob at a
  scratch address, ask the CM0+ to verify it, confirm `VALID`. Then flip one covered byte and
  confirm `INVALID`. Then sign with a *different* key and confirm `INVALID` (proves `key_id`
  selection actually gates, not just "any signature passes").
- **(verify in TRM/board)** ECDSA P-256 verify timing on this Crypto block — feeds the
  `CRYPTO_VERIFY_TIMEOUT_MS` bound sanity-checked in Seam 6.

**Exit MET on silicon.** `fbl_crypto_bringup_verify()`: `v_good = VALID`, `v_tampered = INVALID`,
`v_wrongkey = INVALID` — a correctly-signed image accepted, a one-byte tamper and a wrong-key
signature both refused, all via the real M0+ SHA-256-over-flash + ECDSA P-256, still isolated
from the boot decision. Took two silicon findings (S3-1 double-hash, S3-2 byte order) below, both
in target/host glue — the host-tested `crypto_dispatch`/`crypto_service`/`crypto_msg` never
changed. This is the milestone's crypto core proven; Seam 4 just points the boot decision at it.

**Scaffolded (build-pending, then bench).** ECDSA P-256 verify is available with no config work
— the default `cy_crypto_config.h` already enables `CY_CRYPTO_CFG_ECP_C` / `SECP256R1` /
`ECDSA_VERIFY_C` (same reason Seam-2 SHA linked). Files: `crypto_ops_cm0p.c` grows a
`VERIFY_IMAGE` handler — coarse RPC (ADR-0017 D1): the CM0+ SHA-256s the *real* flash body
itself, does stage-1 integrity (computed vs stored hash, ADR-0016 D2) then stage-2
`Cy_Crypto_Core_ECC_VerifyHash` over the digest, and keys off a one-row `crypto_keystore`
(`crypto_pubkey_dev.h`); `port_crypto.c` gains `fbl_crypto_bringup_verify()` driving the *real*
`crypto_verify_image` client; `crypto_keystore.c` joins the CM0+ build, `crypto_service.c` the
CM4 build. New host tool `gen_dev_key.py` emits the dev keypair (private PEM for `sign_image.py`,
public `X||Y` header for the CM0+) — the private PEM is `.gitignore`d (`*_private*`). Host core
stays green. **Bench sequence:** (0) `pip install cryptography`, run `gen_dev_key.py` to replace
the placeholder `crypto_pubkey_dev.h` + get the PEM; (1) `sign_image.py` a small test blob, flash
it to a SCRATCH address (not the app region), call `fbl_crypto_bringup_verify(scratch,len,1)` →
expect `VALID`; (2) flip a covered byte → `INVALID`; (3) sign with a *different* key → `INVALID`
(proves `key_id`/signature actually gate). **Bench checks:** (a) ECC input **byte order** — the
handler passes big-endian; if a known-good image gives stage-1 pass + stage-2 INVALID, reverse
X/Y/r/s/digest to LE (flagged in `do_verify_image`); (b) the Crypto block hashing directly from
flash (Seam 2 hashed RAM; flash is memory-mapped, expected fine — else `CY_REMAP_ADDRESS_FOR_CRYPTO`).

**Bench finding S3-1 (double-hash in the signing tool).**
- *Expected:* `fbl_crypto_bringup_verify()` of a correctly-signed image returns VALID.
- *Silicon:* returned INVALID (not ERROR — the M0+ ran the verify, the signature just didn't
  check out). Root cause was host-side: `sign_image.py` signed with
  `private_key.sign(digest, ec.ECDSA(hashes.SHA256()))`, which **hashes its input again** — so
  it signed `SHA256(digest)`. But the target's `Cy_Crypto_Core_ECC_VerifyHash` takes the
  *pre-computed* digest and does **not** re-hash (PDL doc: "the hash (message digest) that was
  signed"), so it verifies the signature against `digest` directly → mismatch → INVALID. It was
  NOT byte order: a host simulation of the exact target flow showed stage-1 (computed == stored
  hash) passing and only stage-2 failing, and the fixed signature verifying against the digest.
- *Fix (host-only):* sign the digest with `ec.ECDSA(Prehashed(hashes.SHA256()))` — sign the
  32-byte digest as-is, no second hash. `test_sign_image.py` was masking it (its verify also
  re-hashed, so it was self-consistent but didn't mirror the target); it now verifies with
  `Prehashed`, so it reproduces the target and guards the regression. No CM0+ / key change —
  just re-run `make_test_image.py` and reflash.
- *Lesson:* "sign a hash" is ambiguous — signing `H` with a hash-suite re-hashes to `Sign(H(H))`;
  the hardware verify wanted `Sign(H)`. A host test that simulates the *verifier faithfully*
  (same prehash semantics) catches this; one that's merely self-consistent does not.

**Bench finding S3-2 (ECC input byte order).**
- *Expected:* after S3-1, a correctly-signed image verifies VALID.
- *Silicon:* still INVALID — but a one-line diagnostic (stage-1 mismatch temporarily returns
  ERROR instead of INVALID) split it cleanly: `v_good` stayed **INVALID** (stage-1 hash MATCHED,
  ECDSA verify failed) while `v_tampered` became **ERROR** (stage-1 correctly mismatched). So the
  flash-SHA works — even catching a one-byte tamper — and only the signature verify was wrong.
  Cause: `Cy_Crypto_Core_ECC_VerifyHash` runs on the Crypto vector unit, which is
  **little-endian** — the driver's own stored P-256 constants (Gx = `0x96,0xc2,…` = the standard
  `6b17d1f2…` reversed) are LE, and it loads point coords + signature scalars as-is. We were
  passing everything big-endian (sign_image / gen_dev_key / SHA output).
- *Fix (CM0+ handler):* reverse the public key `X`/`Y` and the signature `r`/`s` to little-endian
  before `VerifyHash`; leave the **hash big-endian** (the driver inverts it internally — the one
  exception, confirmed by the explicit `RegInvertEndianness(p_o)` on only the hash operand).
- *Lesson:* a vendor crypto block's numeric byte order is its own, not the wire's — and it need
  not be uniform across operands (here: LE scalars, BE hash). The proof was in the driver's
  stored curve constants, and a stage-splitting diagnostic (ERROR vs INVALID) turned "it doesn't
  verify" into "stage 2 only", which named the fix.

## Seam 4 — wire it into the boot decision (the milestone)

**Goal:** flip `FBL_DIGEST_ALGO` to `SHA256` for real and prove the *unchanged* call site
(`fbl_app_image_valid()`, ADR-0016 D3 / ADR-0012 D6) now drives the cross-core path underneath it
— cash out D6's bet on real hardware, not just in the design session.

- Sign the actual Node A app with `sign_image.py` (a real dev keypair — generate one via the
  `cryptography` lib or `openssl ecparam -name prime256v1`; this is host-side only, ADR-0019 D1).
- Flash it via the M3 UDS path (`host_tools/uds_flash`) — but the tool needed a one-line change:
  it must download the pre-stamped image *verbatim* rather than re-stamping a raw hex (see bench
  finding S4-1 below; the original "unchanged" assumption was wrong).
- `ECUReset` → FBL boots → `fbl_run_boot()` calls the (now cross-core) verify → jumps only on
  `VALID`.
- Failure demo (mirrors M3's power-loss demo): flash a validly-CRC-complete-shaped but wrongly
  signed image (wrong key or a tampered byte past stamping) and confirm the FBL stays resident —
  never jumps, never hangs.

**Exit:** the milestone headline — a `sign_image.py`-signed app, flashed over the M3 bus path,
boots via a real M0+-computed ECDSA verdict; an unsigned/mis-signed image is refused. This is the
demo, recorded the same way M3's log recorded "the app's LED returns on its own, no power cycle."

**Wiring prepped + host-proven (activation deferred to after Seam 3 bench).** The D6 bet is
cashed: `fbl_app_image_valid()`'s call site is untouched — under `FBL_DIGEST_ALGO ==
FBL_DIGEST_SHA256` the digest-compare is replaced by `crypto_verify_image((uint32_t)base,
image_len, key_id)`, delegating to the M0+ (boot.c). `boot_types.h` gained the full trailer
geometry (`FBL_SIG_SIZE`/`FBL_KEY_ID_SIZE`/`FBL_TRAILER_SIZE = 100 B` for SHA-256 vs 4 for
CRC32) so the bounds checks reserve the whole excluded region; `fbl_main.c` binds the port
(`crypto_service_init(fbl_crypto_port())`) before `fbl_run_boot()`, compiled only in SHA-256
mode. A new host suite `test_boot_secure` (boot.c built `-DFBL_DIGEST_ALGO=FBL_DIGEST_SHA256`,
against the scripted fake M0+) proves the wiring **before the board**: VALID⇒valid,
INVALID⇒reject, **timeout⇒reject (fail-safe, ADR-0016 D5)**, and bad magic/vectors/short-region
rejected — 6/6, with `test_boot` (CRC32) still 37/37 and cppcheck clean. **Not yet flipped:**
`fbl_config.h` stays `FBL_DIGEST_ALGO = FBL_DIGEST_CRC32`, so the M1–M3 boot path is
byte-for-byte unchanged; flipping it to `SHA256` (+ signing the real app) is the one-line
activation once Seam 3 verifies on the bench. That flip *is* Seam 4 on silicon.

**Bench finding S4-1 (the flashing tool double-stamped the signed image).**
- *Expected (this plan, above):* "flash it via the existing M3 UDS path (`host_tools/uds_flash`)
  unchanged — the signed trailer is just bytes past `image_len` to that tool." **That assumption
  was wrong.**
- *Silicon:* `check-image` (0x31 FF01) returned NRC 0x72 on a correctly-signed app that verified
  VALID on the host. An OpenOCD flash dump (CM4) split it: header magic/vectors were correct but
  `image_len` at `0x10040108` read `0x604C` (the whole 24652-B file) instead of `0x5FE8` (the
  covered body). With the wrong `image_len` the FBL hashes body **plus** the real trailer and reads
  the "stored hash" from the download padding past EOF → guaranteed mismatch → `fbl_app_image_valid`
  rejects *before* the crypto call (so the M0+ verify never even runs — the `0x0801F640` capture
  stayed uninitialized, which is what pointed upstream of the crypto).
- *Cause:* `uds_flash.py` is M3-era — it took a *raw* app hex and `stamp_image()`d it (CRC32
  trailer, `image_len = len(body)`). In M4 the app arrives **already** stamped (SHA-256 header +
  hash/sig/key_id trailer, from `sign_image.py`), so the tool double-stamped: it treated the whole
  signed blob as the body, overwrote the header `image_len` with the full length, and replaced the
  signature trailer with a CRC32. Both stampers use the same magic `A9900D01`, which is why the
  header *looked* valid in the dump and hid the swap.
- *Fix (host-side only):* `build_download_image()` now downloads the pre-stamped hex **verbatim**
  (pad-to-row only, no `stamp_image`). Verified: the download image is byte-for-byte
  `app_stamped.bin` (`image_len=0x5FE8`, stored SHA `ADF496D0…`, real 100-B trailer).
- *Lesson (technical):* when a stage's inputs change shape (raw → pre-stamped) but a downstream
  tool's contract didn't, a shared *magic constant* can mask the regression — the header passed its
  own check while meaning something different. The dump discipline paid off: reading the **actual
  flashed bytes** (not the source file) is what exposed that the file and the flash disagreed on one
  field.
- *Lesson (process — the expensive one):* I debugged the wrong layer for too long. The signed file
  verified on the host and the FBL/CM0+/crypto were all correct — the defect was in the *tool that
  moved the bytes*, which I had implicitly trusted because it worked in M3. Two rules from this:
  1. **Verify the pre-conditions before debugging the code under test.** The check-image failure has
     two possible causes — "the verifier is wrong" or "the thing being verified isn't what I think."
     I jumped straight to instrumenting the verifier (FBL, then CM0+, then crypto byte-order) and
     only late dumped the *actual flashed image*. That dump — comparing what's in flash against what
     the source file says — should have been step 1, not step 8. Cheap, decisive, and it points at
     the layer *above* the code you suspect.
  2. **A tool that worked last milestone is not a verified pre-condition this milestone.** `uds_flash`
     was trusted because M3 proved it — but M4 changed the shape of its input (raw → pre-stamped),
     silently invalidating that trust. When a milestone changes an artifact's format, re-audit every
     tool that touches it; don't inherit trust across a contract change. A 5-line read of
     `build_download_image` would have found it before any board time.
  - *Rule of thumb going forward:* **confirm the input is what you think it is before concluding the
    processor is wrong.** Dump the artifact, diff it against the source of truth, then debug.

## Seam 5 — prove the isolation is real, not cosmetic

**Goal:** ADR-0017's TCB-isolation rationale (reason 2 in that ADR's Context) is a claim about
protection-unit configuration, not about which core the code happens to run on. Untested, it's
just an assertion. **Design + decisions in ADR-0020** (scope = *both* boundaries; rigor = *full*,
separate protection contexts + locked master structs).

**The honest claim under test:** a memory-safety bug in the CM4 FBL's attacker-reachable CAN/UDS
parser cannot read/corrupt/operate the key or the CRYPTO engine — the *bus fabric* refuses it,
independent of CM4's own code. (The pubkey isn't secret, so Boundary A's substance is *integrity*
/ write-deny; read-deny is the observable demo + the M5 capability — ADR-0020.)

- **Boundary A — key SMPU (ADR-0020 D3):** one SMPU struct in **match mode** over the CM0+ image
  flash (`0x1000_0000..0x1002_0000`, the compiled-in `crypto_pubkey_dev`) that matches **only the
  CM4's PC** and denies it; the CM0+ (a different PC) falls through untouched. Region geometry from
  the host-tested `shared/prot` helper.
- **PC split (ADR-0020 D2):** on silicon both CPUs boot in **PC2** (the DAP is PC0). So the CM0+ is
  left in PC2 and only the **CM4 is moved to ordinary PC3** in `main_cm0p.c` **before
  `Cy_SysEnableCM4`**. The FBL *and* the app inherit the CM4 PC → M5-ready.
- **Boundary B — crypto PPU (ADR-0020 D4) + D5 lock:** *design-only, default-off* — redundant with
  Boundary A (CM4 never touches CRYPTO), deferred to M5. See ADR-0020 "Scope as built".
- **Demo (ADR-0020 D6):** `proj_cm4/src/fbl_tcb_probe.c` (debug-build-only, `FBL_M4_SEAM5_PROBE`)
  reads a key byte + a CRYPTO register → BusFault/HardFault on CM4, caught by its
  `HardFault_Handler` (inspect `g_tcb_probe`, incl. `BFAR` = faulting address) over OpenOCD. Reads,
  not writes — a PPU-violating write can be AHB-buffered and OK'd (TRM). **Not** an `mdw` read —
  the DAP is PC0 and bypasses the wall (that's why our dumps have always worked).
**Status: DONE (Boundary A).** On silicon both CPUs run in PC2 (the DAP is PC0); the final wall is
an **SMPU in match mode** that denies only the CM4's PC (moved to PC3), CM0+ untouched in PC2. Host
slice `shared/prot/prot_region.{h,c}` + `test_prot_region` green; CM0+ config
`proj_cm0p/src/prot_config_cm0p.{c,h}` (`FBL_M4_SEAM5_PROT`) + CM4 fault-demo
`proj_cm4/src/fbl_tcb_probe.{c,h}` (`FBL_M4_SEAM5_PROBE`). **Boundary B (CRYPTO PPU) and the D5
master-lock are design-only** (default-off flags) — redundant defense-in-depth, deferred to M5;
see ADR-0020 "Scope as built".

**Bench proof (reproduced):**
| Build | proj_cm0p | proj_cm4 | Result |
|---|---|---|---|
| Baseline (control) | walls OFF | probe ON | ✅ key read succeeds — `stage==9` |
| Walled | walls ON | probe ON | ✅ key read **faults** — `faulted`, `CFSR` PRECISERR, `BFAR==0x1000_0000` |

**Exit (met for Boundary A):** a reproduced denial — CM4 code cannot reach the key (bus fault),
while the CM0+ still verifies and the FBL boots. The delta between the two rows is what makes
ADR-0017 "why offload" more than a design-session assertion.

## Seam 6 — fault injection: kill the M0+ and confirm the fail-safe

**Goal:** the property every host test already pins down (ADR-0016 D5) — prove it under an actual
non-responding second core, not just a fake that returns nothing.

Fault injected via two compile-time CM0+ hooks (the CM0+ can't be debugger-halted, and it must run
to release the CM4, so "don't start it" isn't available) — both default-off, in `main_cm0p.c`:
- `FBL_M4_SEAM6_DEAD` — CM0+ releases the CM4 then never services the mailbox → CM4 verify polls
  out to `CRYPTO_VERIFY_TIMEOUT_MS` (1 s) → `IPC_TIMEOUT` → `CRYPTO_VERDICT_ERROR` → stay in FBL.
- `FBL_M4_SEAM6_GARBAGE` — CM0+ corrupts a byte of the encoded reply → `crypto_msg_decode` rejects
  → `ERROR` → stay in FBL.

**Status: DONE.** Bench-confirmed on silicon: the *same good app* jumps with a working verifier,
and is **refused** (FBL enters programming mode, never `fbl_port_jump_to_app`) under both faults —
`DEAD` showing the visible ~1 s timeout, `GARBAGE` rejecting immediately. Same fail-safe the Unity
tests prove against a fake — the last "host-tested → safe on the board" link closed.

---

## Definition of done (M4)

FBL verifies the app's **authenticity** (SHA-256 + ECDSA P-256 via the M0+ crypto service) before
jumping, using the same call site as M1's CRC32 check (ADR-0016 D3 confirms ADR-0012 D6). A
`sign_image.py`-signed image flashed over the M3 UDS path boots; a tampered or wrong-key image is
refused; a non-responding M0+ is refused, never hung. The CM0+ crypto engine and key material are
confirmed unreachable from CM4 by protection-unit configuration, not by convention. Tag it:
`git tag -a m4-secure-boot -m "FBL authenticates the app via M0+ crypto offload"`.

## Don't, in M4

Lifecycle advancement / eFuse provisioning (ADR-0016 D4 — designed, deliberately not burned; the
board stays honestly "mechanism proven, root unprovisioned"). Key rotation / multi-key policy
beyond the one `key_id` row. SecOC / M5's MAC op (the dispatch table is additive-ready,
ADR-0017 D3, but the op itself is not built). A full HSM-grade key-management lifecycle
(revocation, expiry) — design-essay scope (roadmap EP.17), not this milestone.
