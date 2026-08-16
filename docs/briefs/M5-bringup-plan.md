# M5 SecOC bring-up plan (bench runbook)

Ordered bench procedure for the M5 SecOC target seams. Host logic is already CI-green
(`test_secoc`, `test_secoc_freshness`, `test_secoc_resync`, `test_freshness_store`,
`test_crypto_mac`) — everything below is the target-only half: ADR-0021 D1/D6/D7, ADR-0018 D5/D6.

**Work top to bottom.** The order is deliberate: cheapest and most-likely-to-fail first, one core
at a time, and nothing touches the real bus until the cross-core MAC offload is proven. Each stage
names what to watch and the first thing to check on failure.

Design refs: `docs/architecture/secoc-architecture.md`, `ADR-0021`, `ADR-0018` (D5/D6 + M5
addendum), `ADR-0017`. Sibling runbook: `M4-bringup-plan.md`.

---

## Stage 0 — Build only, no board (do this first)

Free, and it front-loads the single highest-risk item in the whole milestone: **the PDL CMAC
symbol names**. I wrote the one-shot CMAC call against the typical `mtb-pdl-cat1` API without a
toolchain to check it against, so treat a compile error here as expected, not alarming.

- [ ] **0.1 Node B CM0+** — `node_b_actuator/proj_cm0p`: `make getlibs && make build`
- [ ] **0.2 Node A CM0+** — `node_a_gateway/bootloader/proj_cm0p`: `make build`
- [ ] **0.3 Node B CM7** — `node_b_actuator/proj_cm7`: `make build`
- [ ] **0.4 Node A app** — `node_a_gateway/app`: `make build`

**Expected failure #1 — the CMAC API.** In `crypto_ops_cm0p.c` (both nodes), `cmac_compute()` uses:

```
Cy_Crypto_Core_Cmac(CRYPTO, msg, len, key, CY_CRYPTO_KEY_AES_128, tag, &aes_state)
cy_stc_crypto_aes_state_t aes_state;
```

If that does not resolve, check the installed PDL's `cy_crypto_core_cmac.h` /
`cy_crypto_core_cmac_v2.h` for the real names. Likely variants: `Cy_Crypto_Core_V2_Cmac`; a
separate `Cy_Crypto_Core_Aes_Init(CRYPTO, key, CY_CRYPTO_KEY_AES_128, &aes_state)` required
*before* the CMAC call; a distinct `cy_stc_crypto_cmac_state_t`; or the three-call
`_Cmac_Init/_Update/_Finish` form. Fix the call, keep everything else — the handler shape,
keystore lookup, and error mapping are independent of which spelling wins.

**Expected failure #2 — CM7 macros.** `port_crypto.c` uses `ARM_MPU_RBAR/RASR/SetRegion/Enable`
(CMSIS) and `CY_IPC_CHAN_USER`. If `CY_IPC_CHAN_USER` is absent on CAT1C, find the free user
channel in the PDL's `cy_ipc_config.h` / `cy_device.h` for `COMPONENT_CAT1C` — **do not guess an
index.**

- [ ] **0.5 Linker invariant (the cross-file one).** In BOTH Node B `.map` files, confirm
      `Cy_SecOc_IpcMailbox` resolves to the **same** address, expected `0x280E0000`
      (= `sram_base 0x28000000` + `cm0plus_sram 0x4000` + `cm7_0_sram 0xDC000` = base of
      `ram_noncache`). A mismatch here is a silent cross-core corruption bug later — catch it now.
- [ ] **0.6** Confirm `.secoc_ipc_mailbox` landed at `ORIGIN(ram_noncache)` in the CM7 map and
      that `.ram_noncache` (`.cy_sharedmem`) starts *after* the reserved `0x100`.

---

## Stage 1 — Node A M4 regression (do not break secure boot)

M5 modified files the M4 secure-boot path depends on: `crypto_types.h` (new op + tag length),
`crypto_msg.c` (MAC helpers), `crypto_keystore.h` (the row struct gained `type`/`secret`/
`secret_len`), and Node A's `crypto_ops_cm0p.c` (new dispatch row + keystore row).

- [ ] **1.1** Flash the Node A FBL + CM0+ as before; confirm **secure boot still verifies the app
      and jumps** (the M4 Seam-4 pass criterion).
- [ ] **1.2** Confirm the M4 fault-injection flags still fail safe: `FBL_M4_SEAM6_DEAD` →
      timeout → stay-in-FBL; `FBL_M4_SEAM6_GARBAGE` → decode reject → stay-in-FBL.

If 1.1 regresses, suspect the keystore struct change: Node A's ECDSA row is now a designated
initializer with `.type` defaulting to `CRYPTO_KEY_ECDSA_PUBLIC` (0). Verify
`crypto_keystore_lookup(CRYPTO_DEV_KEY_ID)` still returns the pubkey row with `pubkey_len == 64`.

---

## Stage 2 — AES-CMAC known-answer test, each CM0+ independently

Prove the *primitive* before trusting it across cores or across the bus. Run on **both** parts
(Node A CM0+ and Node B CM0+) — same algorithm, two different silicon families.

NIST **SP 800-38B** AES-128 CMAC vectors, key `2b7e151628aed2a6abf7158809cf4f3c`:

| Msg | Expected tag (16 B) |
|---|---|
| *(empty, len 0)* | `bb1d6929 e9593728 7fa37d12 9b756746` |
| `6bc1bee22e409f96e93d7e117393172a` (16 B) | `070a16b4 6b4d4144 f79bdd9d d04a287c` |

> Cross-check these two lines against your copy of SP 800-38B (Appendix D.1) before trusting a
> mismatch — they are quoted from memory. If the tag differs but is *stable and input-sensitive*,
> the wiring is fine and the vector is the suspect; if it differs *randomly*, the wiring is wrong.

- [ ] **2.1** Add a temporary bring-up hook on the CM0+ that calls `cmac_compute()` with the NIST
      key + message and parks the tag in a `volatile` global; read it in the debugger.
      (Ask me and I'll write the hook — it is ~15 lines, target-only.)
- [ ] **2.2** Both parts produce the expected tag. **AES-CMAC is byte-oriented — no endianness
      reversal** (unlike the ECDSA path's S3-2 finding). If your tag looks byte-reversed or
      word-swapped, that is the bug, not the vector.
- [ ] **2.3** Note whether an explicit `Cy_Crypto_Core_Aes_Init` was needed (feeds back to 0.1).

**Do not proceed until 2.1 passes on both parts.** Everything downstream assumes the primitive.

---

## Stage 3 — Node B cross-core MAC offload round trip

The seam that makes the offload real: CM7 client → mailbox (non-cacheable) → CM0+ server →
dispatch → CMAC → back.

- [ ] **3.1 Dual-core boot still works.** Flash both Node B images. Heartbeat LED (P5.0) blinks
      at ~500 ms. If it stopped, suspect `secoc_mpu_noncache_init()` — the MPU is now programmed
      before the scheduler. Comment that one call out to confirm/deny.
- [ ] **3.2 Phase A loopback unaffected.** `g_can_tx_count`, `g_can_isr_count`, `g_can_cb_count`,
      `g_can_rx_count` all still climbing. (`0x123` is not a command ID, so the SecOC RX path stays
      dormain in Phase A by design.)
- [ ] **3.3 The round trip.** Build `proj_cm7` with `DEFINES=SECOC_CRYPTO_BRINGUP=1` and call
      `secoc_crypto_bringup_mac()` from a task (e.g. once at the top of `can_task`). It must
      return **true**: same input ⇒ same tag (deterministic), one flipped byte ⇒ different tag,
      unknown `key_id` ⇒ false.
- [ ] **3.4** Confirm the MPU region is actually non-cacheable, and that the CM7 D-cache is
      enabled at all — if D-cache is off, 3.3 passing proves the protocol but **not** the D6
      coherency fix. Check `SCB->CCR.DC` / the BSP's cache init, and whether the BSP already
      programs an MPU region for `ram_noncache` (reconcile, don't duplicate — region 7 is assumed
      free).

**Failure triage for 3.3:** hangs → the CM0+ server never answered (is `cm0p_crypto_service_init()`
running before `Cy_SysEnableCM7`? is the IPC channel the one the client uses?). Returns false
immediately → `crypto_mac` got a verdict instead of a tag: unknown `key_id` (is
`SECOC_MAC_KEY_ID = 0x5E` in the CM0+ keystore?) or the CMAC call failed. Intermittent/garbage →
cache coherency (3.4) or the mailbox address invariant (0.5).

---

## Stage 4 — Cross-node key agreement

Both nodes hold the *same* secret from one header, so the same input must give the same tag.

- [ ] **4.1** MAC the identical byte string on Node A's CM0+ and Node B's CM0+; the 16-byte tags
      must match **exactly**. A mismatch means the secret differs (check both images include
      `security/include/secoc_shared_secret.h`) or one part's CMAC has an endianness/init quirk.

This is the cheapest possible proof that A↔B authentication *can* work, before any bus wiring.

---

## Stage 5 — Real bus (Phase B), both nodes

- [ ] **5.1** Terminate the bus (120 Ω both ends), VN1610 attached and observing.
- [ ] **5.2 The transceiver enable pin** — the known Node B gotcha: the onboard CAN transceiver's
      **STB/EN** may need a GPIO driven in `can_task_create()`. **If the bus is silent, check this
      first.** Confirm from the kit schematic.
- [ ] **5.3** Node B: `CAN_LOOPBACK_TEST=0` (also removes the internal-loopback test-mode config).
      Node A: already defaults to `0`.
- [ ] **5.4** Observe on the analyser, with SecOC framing visible: Node B's boot `0x2F0`
      FRESHNESS_SYNC (**14 B** = 2-byte floor + 4 freshness + 8 MAC), Node B's periodic `0x200`
      telemetry (**15 B** = 3 + 4 + 8), and Node A's `0x120`/`0x121` commands (**13 B** = 1 + 4 + 8)
      when bodyctl emits one. Each frame's last 12 bytes are freshness ∥ truncated MAC — visibly
      *not* a plain body message. That screenshot is the milestone's money shot.
- [ ] **5.5 The happy path end to end:** Node A's courtesy-light rule fires on Node B's door-ajar
      telemetry → Node A sends a secured `0x121` → Node B verifies and actuates
      (`actuator_light_pct()` changes). Watch that `g_secoc_drop_*` stay **0** on both nodes
      throughout — a working link must produce zero drops.

---

## Stage 6 — The security demonstrations (the point of the milestone)

Inject with the VN1610 and watch Node B refuse. Every case must leave the actuator in its **last
safe state** and bump exactly one counter.

- [ ] **6.1 Forged command (no valid MAC).** Send a plain `0x120` with payload `01` (DOOR_UNLOCK),
      no SecOC trailer. Expect: dropped, `g_secoc_drop_mac`++ (a 1-byte frame is shorter than the
      12-byte trailer ⇒ `SECOC_BAD_LENGTH`), door stays locked.
- [ ] **6.2 Bit-flipped MAC.** Capture a genuine `0x120`, flip one bit in the last 8 bytes, replay.
      Expect: `g_secoc_drop_mac`++, no actuation.
- [ ] **6.3 Replay (the freshness proof).** Capture a genuine `0x120` and replay it **unmodified**.
      Expect: MAC verifies, but freshness is stale ⇒ `g_secoc_drop_fresh`++, no actuation. This is
      the one that separates "authenticated" from "authenticated *and* fresh".
- [ ] **6.4 Cross-ID substitution (the ADR-0021 D2 fix).** Capture a genuine `0x120` frame and
      replay its **exact bytes on ID `0x121`**. Expect: `g_secoc_drop_mac`++ — the Data ID is bound
      into the MAC input, so the tag cannot validate on a different ID. Without D2 this would have
      passed both gates. Worth capturing carefully; it is the subtlest finding in the design.

---

## Stage 7 — Resync (D5): read this before hunting for it

**The D5 deadlock/resync cannot actually trigger in M5**, and that is expected, not a bug. The
freshness store is **RAM-backed** in M5 (`secoc_app.c`; `eeprom_emu` arrives in M6), so a Node B
reboot wipes its accepted-epoch high-water: `load()` returns cold ⇒ `floor = 1`, `current = 0`.
Node A (still at epoch 1) is therefore *never* below B's floor, so no deadlock forms, and the
`0x2F0` sync A receives asks for floor 1 — `tx_adopt_floor` correctly declines to move.

Consequences to state honestly in the write-up:
- The sync frame **is** observable on the bus and **is** authenticated (5.4 shows it) — the
  mechanism is proven, its *necessity* is not yet demonstrable.
- **Anti-replay across a receiver reboot is not real until M6.** Right after a Node B reset, an
  old captured `0x120` *will* be accepted (high-water is back to 0 and floor is only 1). Try it —
  6.3 passing before a reset and failing after one is the cleanest possible demonstration of *why*
  the store must be persistent.

- [ ] **7.1 (optional) Force the demo.** To see the deadlock + recovery now, temporarily make Node
      B's `store_load(SECOC_DOM_RX_A, …)` return a hardcoded epoch (say 5) so boot sets
      `floor = 6`. Then: A's epoch-1 commands are refused (`g_secoc_drop_fresh`++), B's `0x2F0`
      carries floor 6, A adopts 6, and commands flow again. Revert the hack afterwards.

---

## Findings log

Append silicon findings here as they land (the ADR-0017/0018 convention), then fold the
load-bearing ones back into `ADR-0021` / `ADR-0018 D6` Review history.

| # | Stage | Finding | Resolution |
|---|---|---|---|
| 1 | Log bring-up (ADR-0023) | **First light on Node A.** `traveo-body app build=unknown` observed on Tera Term @ 1 Mbps 8N1 via the KitProg3 bridge. Confirms on silicon: P0[0]/P0[1] → SCB0 HSIOM 17, the runtime-computed peripheral divider landing inside the 2% tolerance, and the whole `log_evt → ring → log_drain → Cy_SCB_UART_PutArray` path with the drain task at 20 ms. | Accepted. Configuring SCB0 **in code** (not via the regenerable BSP) is validated. Binary records still unproven — needs a host decoder + a first `log_evt` emitter. |
| 2 | Log bring-up | Banner reads `build=unknown`. Harmless at the desk, but the BVT will reflash between tests and a log that cannot name its own image makes "which build produced this trace?" unanswerable. | Open — carry a build id (git short SHA) into the banner. |
| 3 | Log bring-up (ADR-0023) | **Binary record path proven end-to-end.** `LOG_EVT_APP_ALIVE` observed ticking at 500 ms (= `APP_PERIOD_HEALTH_MS`) and rendering by name through `host_tools/logview`. Closes what finding #1 left open: `log_evt` → BASEPRI reserve/copy/publish → ring → 20 ms drain task → SCB0 → host CRC/resync → generated event table, all on silicon. Drain latency is comfortably inside `LOG_DRAIN_LATENCY_MS` (50). | Accepted. Steps 1–3 of the logger plan complete. Remaining: CM0+ ring (cross-core), FBL port, Node B. |
