# M4 Secure Boot — Bring-up Log

A live journal of bringing M4 up on the CYT2B7 (TRAVEO™ T2G Body Entry): the FBL
authenticates the app (SHA-256 + ECDSA P-256) before it jumps, offloading the
crypto to the 100 MHz Cortex-M0+ acting as a small HSM, and walling the key from
the untrusted CM4 with a bus-fabric protection unit. As in M1–M3, the
hardware-independent core — the IPC framing (`crypto_msg`), the dispatch table,
the FBL-side verify client and its fail-safe, the boot decision — was proven on
x86 under Unity *before* the board, and **again not one silicon fault reached it**.
But M4 moved the boundary of where the hard bugs live: for the first time they
weren't only in a port, they were in **a second core we now own end to end**, in
the **HW crypto engine's conventions**, and — hardest — in the **bus protection
units, the one seam with no host model to catch a wrong assumption early**.

Format per entry: *what I expected · what the silicon did · the lesson*. Brought
up one seam at a time. The milestone is Seam 4 (a signed app boots on a real
CM0+-computed verdict); Seams 5–6 turn "it verifies" into "it can't be bypassed."

---

## Seam 0 — our own CM0+ image

Through M1–M3 the CM0+ ran the vendor `COMPONENT_CM0P_SLEEP` prebuilt. M4 replaces
it with our own buildable image (the crypto service has to land somewhere), so the
CM0+'s job becomes: bring up clocks, **start the CM4**, then serve crypto. Owning
that image surfaced three faults before a single crypto byte moved.

### 1. `Cy_SysEnableCM4` wants the absolute vector-table address, not the offset

- **Expected:** release the CM4 with `Cy_SysEnableCM4(vectorTableOffset)` — pass the
  CM4 image's offset past the CM0+ region, `0x2_0000`.
- **Silicon:** immediate HardFault/lockup on the CM4. The parameter is *named*
  "vectorTableOffset", but its doc defines it as the offset "from memory address
  0x00000000" — i.e. the **absolute** address, written straight into
  `CPUSS.CM4_VECTOR_TABLE_BASE`. `0x2_0000` pointed the CM4 at `0x0002_0000` →
  garbage vectors.
- **Fix:** pass the absolute `0x1002_0000` (flash base + `FLASH_CM0P_SIZE`),
  1024-byte aligned. Kept the constant named so it stays grep-able against the
  linker.
- **Lesson:** a parameter's *name* is not its contract — the doc said "from
  0x00000000" and meant it. This is the very first instruction the CM4 fetches; get
  it wrong and there's nothing to debug but a lockup.

### 2. The CM0+ idling from its own flash faults the instant the CM4 erases

- **Expected:** after starting the CM4, the CM0+ busy-polls the mailbox for a
  request.
- **Silicon:** during a UDS reflash the CM4 erases app flash and the CM0+ faulted.
  On this part an instruction fetch from a flash macro that is **mid-erase is a
  read-while-write violation** (the same hazard as M3 #11, now on the other core).
  A CM0+ that idles by fetching its poll loop from flash faults the moment the CM4
  starts an erase — exactly what the vendor prebuilt sidestepped by *sleeping*.
- **Fix:** the idle spin runs from **RAM** (`.cy_ramfunc`, `noinline` so the
  optimizer can't inline it back into flash); the CM0+ enters flash-resident
  dispatch only when a request is pending, which never coincides with a flash op.
- **Lesson:** on an asymmetric pair sharing one code-flash, "idle" is not free — the
  idling core must not fetch from a flash the other core might be erasing.

### 3. The custom CM0+ never wired the SROM syscall path — so the CM4 hung on erase

- **Expected:** with the CM0+ alive, the CM4's flash erase/program just works (it
  did in M3, under the prebuilt).
- **Silicon:** the CM4 hung forever inside `Cy_Srom_CallApi`. Flash program/erase
  on this part is an **SROM system call serviced by the CM0+** over NvicMux0/1; the
  BSP's `PrepareSystemCallInfrastructure()` wires that up — but it only runs inside
  the vendor prebuilt's startup, which we'd replaced. A fixed shared-SRAM debug
  struct (read from the CM4's OpenOCD, since we can't attach to the CM0+) showed the
  CM0+'s IRQ0/1 vectors still pointing into flash and NvicMux0/1 disabled — and
  `PRIMASK` still `1`, the M3 #5 masked-interrupt bug reincarnated on the CM0+ twin.
- **Fix:** replicate it — point the CM0+'s IRQ0/1 vectors at the SROM handlers,
  enable NvicMux0/1, `__enable_irq()`. The CM4's SROM call is serviced from then on.
- **Lesson:** replacing a vendor prebuilt means inheriting the *invisible* work its
  startup did. And with no debugger on the CM0+, a fixed shared-SRAM "progress +
  register-capture" struct, read from the *other* core's OpenOCD, is how you see a
  core you can't attach to — a pattern that paid off again in Seam 5.

## Seams 1–2 — the mailbox and the first real hash

Seam 1 (IPC echo) and Seam 2 (HW SHA-256 of the NIST "abc" vector, round-tripped
through the mailbox to the CM0+ and back) came up **without a silicon fault** — the
host-tested transport and framing carried over intact, and the Crypto block's SHA
path matched the known digest first try. Worth noting: the CM0+ hashes the *real*
memory-mapped flash directly (ADR-0017 D1, the coarse RPC), which worked without
the `CY_REMAP_ADDRESS_FOR_CRYPTO` fallback the design had hedged for.

## Seam 3 — ECDSA P-256 over a flash range

Add the two-stage verify: SHA-256 over the image body, then ECDSA over the digest.
The staging worked; the signature didn't — for one very specific reason.

### 4. The Crypto VU is little-endian; a known-good signature verified INVALID

- **Expected:** feed the public key (X, Y), the signature (r, s), and the hash to
  `Cy_Crypto_Core_ECC_VerifyHash` in their natural big-endian form.
- **Silicon:** a signature I'd already verified on the host with the same key came
  back **INVALID** on the board. The Crypto VU works **little-endian** — its stored
  curve constants (Gx, order) are LE and it loads the point coordinates and
  signature scalars as-is — while the *hash* stays big-endian (the driver inverts it
  internally). Passing everything big-endian is a silent wrong answer, not an error.
- **Fix:** reverse the key X/Y and the signature r/s to LE before the call; leave
  the digest alone (`reverse32`, bench finding S3-2).
- **Lesson:** "INVALID" from a crypto accelerator is as often an *input-convention*
  bug as a real mismatch. Endianness at the HW-engine boundary is not negotiable and
  not documented where you'd hope.

## Seam 4 — a signed app boots (the milestone)

`sign_image.py`-signed app → flashed over the M3 UDS bus → the FBL asks the CM0+ for
a verdict → jumps. Everything the host tests covered was right; the bug was in the
*tool* that delivers the image.

### 5. The flasher double-stamped the image and clobbered its own signature

- **Expected:** flash the signed `.hex`; the FBL's `check-image` returns VALID.
- **Silicon:** NRC `0x72` on a correctly-signed app. The M3-era `uds_flash.py` still
  `stamp_image()`d whatever it was handed — appending a CRC32 trailer and rewriting
  `image_len`. In M3 the app arrived raw; in M4 it arrives **already SHA-256-signed**,
  so the tool clobbered `image_len` (`0x5FE8` → `0x604C`) and overwrote the ECDSA
  trailer with CRC32. Dumping the on-device bytes and diffing against the signed file
  — not instrumenting the verifier — showed it in seconds.
- **Fix:** the flasher downloads the pre-signed hex **verbatim** (pad-to-row only).
- **Lesson:** a tool that was correct last milestone is not a verified precondition
  this one. When the artifact's *shape* changes, re-audit the tools that touch it —
  and diff the actual bytes on the device before suspecting the code under test.

## Seam 5 — the wall (TCB isolation)

Turn "the CM4 doesn't *code-path* to the key" into "the CM4 *can't* reach it — the
bus fabric refuses, independent of CM4's own code." This was the hardest seam of the
milestone, for a structural reason: **it's the first with no host model.** Every
prior seam had a Unity test to catch a wrong assumption in milliseconds; here the
only oracle was the silicon, answering one rebuild at a time. Findings 6–8 below are
the *same bug class* — a wrong mental model of the protection unit — each caught only
by the board.

### 6. Both cores run in PC2 — the DAP is the PC0 our dumps were reading

- **Expected:** the CM0+ boots in the unrestricted PC0; wall the key region against
  "every non-zero protection context" and the CM0+ (PC0) bypasses freely.
- **Silicon:** the CM0+ **double-faulted** during wall setup and never released the
  CM4. Reading `PROT_MPU0.MS_CTL` over OpenOCD: `PC = 2`. Both CPUs boot in **PC2**;
  the PC0 I'd conflated them with is the **debug access port** — which is precisely
  *why* every `mdw` dump has "just worked" (the DAP bypasses the walls). So "deny
  every non-zero PC" denied the CM0+ its own code flash, and since it executes from
  that flash, the next fetch faulted — and the fault handler, also in flash, faulted
  again → lockup.
- **Fix:** move only the CM4 to a *distinct* ordinary context (PC3) and deny **only
  PC3**; leave the CM0+ in PC2, unnamed by the wall.
- **Lesson:** never assume which protection context you're in — read it. The
  debugger's context is not the CPU's, even though from the mdw side they both looked
  like the unrestricted "PC0."

### 7. An SMPU in access-evaluation mode governs *every* access — so the trusted core has to satisfy it too

- **Expected:** a region rule that "denies CM4" leaves every other master alone.
- **Silicon:** even after targeting only the CM4's PC, the CM0+ still bricked — at
  the slave-struct *enable*. A per-step marker written to shared SRAM pinned the
  faulting call exactly (no host model, so instrument the target itself). The cause
  was the SMPU's default mode (`PC_MATCH=0`, *access evaluation*): once a struct's
  address matches, it governs **all** accesses in the region, and each must pass
  perm **and** NS **and** PC — so the CM0+'s own fetches now had to satisfy a rule
  written for the CM4. (A detour through the secure/NS bit — was the region secure
  enough for the CM0+? — was a red herring; both NS settings failed identically.)
- **Fix:** switch the struct to **match mode** (`PC_MATCH=1`): it then *matches only*
  accesses whose PC is in the mask, so with the mask = CM4 alone, the CM0+ (a
  different PC) doesn't match and **falls through to the open background**, untouched.
  That is the right idiom for protecting a region the trusted core must keep
  executing from.
- **Lesson:** with no host model, a step counter in SRAM turns "it hangs somewhere in
  setup" into "it faults on *this* call." And read the *mode* before the mask bits:
  access-evaluation vs match mode is the whole difference between "wall everyone,
  re-allow the good" and "wall only the bad."

### 8. The PPU is the SMPU's mirror image — you deny a PC by writing *its own* slot

- **Expected:** the CRYPTO PPU works like the SMPU — one attribute set, gate it by a
  PC mask.
- **Silicon (bench):** an "allow everyone but CM4" PPU config let the CM4 read the
  CRYPTO block anyway; a later "deny CM4" then **hung the FBL** (the CM0+ lost its own
  crypto grant). Reading the PDL's write loop settled it: the fixed PPU's ATT is a
  **per-PC permission table**, not a single att gated by a mask. `pcMask` selects
  *which PCs' slots get overwritten*; unnamed PCs keep their default. So "allow all
  but CM4" wrote everyone else and left CM4's slot at its permissive default (no
  deny), while a bare "deny CM4" that stopped writing the CM0+'s slot dropped the
  grant the CM0+ needs to run crypto.
- **Fix:** two writes — grant the CM0+ (and others) explicitly, then set **CM4's own
  slot** to `DISABLED`. Same goal as the SMPU, mirror-image API.
- **Lesson:** two units in the same family can take **opposite** configuration
  shapes; read what the register *is* (a per-PC table) before assuming it mirrors its
  sibling.

### 9. What actually shipped: one proven wall, and the honest scope to stop there

- **Planned:** wall both boundaries (key SMPU + CRYPTO PPU) and lock the walls
  against the CM4 — the "full rigor" design.
- **Reality:** the key SMPU (Boundary A) is **proven on silicon** — the CM4, as the
  untrusted PC3 master, reads the key region and takes a precise BusFault
  (`BFAR = 0x1000_0000`), against a clean walls-off control where the same read
  succeeds. The CRYPTO PPU and the master-struct tamper-lock hit the quirks above and
  are **redundant** for M4 regardless: the CM4 never touches CRYPTO (all crypto
  crosses the IPC mailbox to the CM0+), and the only persistent secret — the key — is
  already walled by Boundary A.
- **Decision:** ship Boundary A; keep the PPU and the lock as design-only behind
  default-off flags, deferred to M5 (whose runtime MAC key would actually live in the
  engine). The crown-jewel claim is demonstrated; the redundant walls aren't
  over-built.
- **Lesson:** knowing which wall is *necessary* is part of the design. Proving the key
  is unreachable — and declining to burn bench time on a wall the architecture
  already makes moot — is the stronger result, and the honest one.

## Seam 6 — the fail-safe, under a real dead/lying CM0+

Every host test already pins "a non-VALID verdict never jumps." Prove it on silicon
with the actual second core misbehaving — the last "host-tested → safe on the board"
link.

### 10. A dead or lying CM0+ is refused, and never hangs

- **Expected:** if the verifier can't be trusted, the FBL stays put.
- **Silicon:** with the CM0+ compiled to **play dead** (release the CM4, then never
  service the mailbox), the FBL's verify polled out to the 1 s
  `CRYPTO_VERIFY_TIMEOUT_MS`, returned `IPC_TIMEOUT` → `ERROR`, and stayed in FBL —
  the *same good app* that boots with a live verifier does **not** boot, and the
  ~1 s delay is visible on the bench. With the CM0+ compiled to **corrupt the reply**,
  `crypto_msg_decode` rejected it → `ERROR` → stay in FBL, immediately. Neither hung.
- **Confirmation, not a fix:** the fault was *injected* with two default-off CM0+
  compile hooks — the CM0+ can't be debugger-halted, and it must run far enough to
  release the CM4, so "don't start it" was never an option.
- **Lesson:** the bounded-wait + collapse-to-ERROR the host tests proved against a
  fake behaves identically against a real non-responding core — which is exactly the
  payoff of designing the fail-safe where a host test *can* reach it, and only
  confirming the timing on silicon.

---

## Where M4 stands

Secure boot is end to end on silicon: a `sign_image.py`-signed app, flashed over the
M3 UDS path, boots on a real CM0+-computed **SHA-256 + ECDSA P-256** verdict; a
tampered or wrong-key image is refused; a **dead or lying CM0+ is refused and never
hangs**; and the public key is **unreachable from the untrusted CM4 by an SMPU
wall**, proven by a CM4-side BusFault against a walls-off control. Tagged
`m4-secure-boot`.

Ten findings, and the M1–M3 pattern held once more: **the host-tested core never
changed** — the crypto framing, dispatch, verify client, fail-safe, and boot
decision that Unity covered were right on the board. Every fault lived where the
host tests can't reach: bringing up a **second core we now own** (the CM4 start
vector, the RAM idle, the SROM-syscall handoff), the **HW crypto engine's
conventions** (little-endian ECC inputs), the **tooling** (the double-stamping
flasher), and — the milestone's hard lesson — the **bus protection units**, the one
seam with no host model, where the same wrong mental model was caught, three
rounds running, only by the board. That seam also changed shape under contact:
from "wall everything with full rigor" to "prove the key is unreachable, and stop."
**M4 bring-up is complete.**
