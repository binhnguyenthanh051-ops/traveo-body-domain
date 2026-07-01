# M2 Node A — CAN (CANFD) Bring-up Log

A focused journal of bringing up the gateway's CANFD seam on the CYT2B7 — the most
debugging-intensive part of M2. The general M2 log (`M2-bringup_log.md`) has one entry
per seam; this is the CAN seam in detail, in the order it actually happened.

**Channel:** CANFD0, channel 1 · **Pins:** P0.2 (TX) / P0.3 (RX) · **Bitrates:**
500 kbit/s nominal, 2 Mbit/s data, **BRS** (ADR-0011 D1) · **RX:** FIFO 0, accept-all,
`RF0N` interrupt → NVIC level 5.

Brought up in two deliberate phases, so the software was proven before the bus was in
the picture:
- **Phase A — internal loopback:** TX routed to RX on-chip; no transceiver / bus / tool.
- **Phase B — real bus:** a Vector VN1610 sends frames and checks the echo.

---

## 0. What the configurator generates vs. what we write

The ModusToolbox Device Configurator generates the channel config (`canfd_0_chan_1_config`:
bit timing, message-RAM partition, RX FIFO 0 + filter, and `.rxCallback = can_rx_callback`)
and assigns the CANFD clock in `cybsp_init()` — but it does **not** call `Cy_CANFD_Init`.
Our `can_task.c` provides the FreeRTOS glue the configurator doesn't:
- `can_rx_callback` — packs a `can_raw_frame_t`, `xQueueSendFromISR` to `raw_frame_q`;
- `can_rx_isr` — `Cy_CANFD_IrqHandler` then `portYIELD_FROM_ISR`;
- `can_tx` — the echo (re-transmit a received frame);
- `can_task_create` — `Cy_CANFD_Init` + the interrupt wiring.

## 1. The debugging instrument: per-stage counters

When RX didn't work, guessing was expensive. Adding four counters made the failure point
observable in one flash (all under `#if CAN_LOOPBACK_TEST`):

```
g_can_tx_count   self-transmit attempts
g_can_isr_count  can_rx_isr entries
g_can_cb_count   PDL rx-callback invocations
g_can_rx_count   frames dequeued by the task
```

Reading rule: `tx` up but `isr` 0 ⇒ frame issued, never reached the NVIC. `isr` up, `cb` 0
⇒ ISR fired but no RF0N. `cb` up, `rx` 0 ⇒ callback ran, queue send failed. This localized
every fault below without a logic analyzer.

## 2. Phase A findings (internal loopback)

### 2a. RX FIFO 0 with **0 elements** silently drops every frame
- **Symptom:** init OK, no RX.
- **Cause:** the configurator generated `rxFifo0Config.numberOfFIFOElements = 0`. Routing
  was correct (`nonMatchingFramesStandard = ACCEPT_IN_RXFIFO_0`) but the FIFO had no
  storage, so accepted frames were dropped and `RF0N` never fired.
- **Fix:** set RX FIFO 0 element count (8) in the configurator. *"Enable FIFO 0" and "size
  FIFO 0" are two different settings.*

### 2b. Internal loopback bits are **protected** — wrap the change in config mode
- **Symptom:** `Cy_CANFD_TestModeConfig(…INTERNAL_LOOP_BACK)` after `Init` did nothing.
- **Cause:** `CCCR.TEST`/`.MON` and `TEST.LBCK` are writable only when `CCCR.CCE=1 && INIT=1`;
  after `Init` the channel is operational, so a bare call is ignored.
- **Fix:** wrap it — `Cy_CANFD_ConfigChangesEnable` → `TestModeConfig` → `…Disable`. Verified
  by reading back `CCCR = 0x3A0` (TEST+MON+FDOE+BRSE) and `TEST = 0x90` (LBCK).

### 2c. **"Init OK, no RX" — the TRAVEO CM4 NVIC mux** (the big one)
- **Symptom:** loopback engaged (2b), clock running, FIFO sized (2a), `RF0N` enabled by
  `Cy_CANFD_Init` on interrupt line 0 — yet `g_can_tx_count` climbed while `g_can_isr_count`
  stayed **0**. The frame looped internally and set `RF0N` *in the peripheral*, but no ISR
  fired.
- **Cause:** on TRAVEO T2G the **CM4 reaches peripheral interrupts through an 8-channel NVIC
  mux** (`NvicMux0..7`). The CANFD IRQ enum `canfd_0_interrupts0_1_IRQn = 58` is a
  **system-interrupt index, not a CM4 NVIC line**. The PSoC-6-style wiring
  (`Cy_SysInt_Init({58, 5})` + `NVIC_EnableIRQ(58)`) silently mapped sysint 58 → `NvicMux0`
  (the mux-channel bits were 0) while enabling a non-existent NVIC line 58.
- **Fix:** pack a mux channel into `intrSrc` and enable the **mux channel**:
  ```c
  .intrSrc = (NvicMux3_IRQn << CY_SYSINT_INTRSRC_MUXIRQ_SHIFT) | canfd_0_chan_1_IRQ_0;
  NVIC_EnableIRQ(NvicMux3_IRQn);
  ```
- **Aftermath:** `g_can_rx_count == g_can_tx_count` — the full path (TX → internal loopback →
  RX ISR → callback → queue → `CAN_CyclicTask` → `body_decode` → `App_CyclicTask` → `bodyctl`)
  proven with zero external hardware.
- **Lesson:** I'd *flagged* the NVIC mux as the watch-for in ADR-0011 D3, then shipped the
  bare-IRQn form anyway. The `tx`-vs-`isr` counters (§1) are what caught it in one flash.

## 3. Phase B — the real bus (Vector VN1610)

### 3a. Tool: `python-can`, not CANalyzer
CANalyzer is separately licensed; for an echo test it's overkill and a personal-laptop
license is a policy question. The VN1610 is driven license-free by **`python-can`** (`vector`
backend) — only the free **XL Driver Library** is needed. Captured as a config-driven bench
tool: `host_tools/can_echo_probe/`.

### 3b. `vxlapi64.dll not found`
- **Cause:** the "Vector Driver" (device driver) is **not** the "XL Driver Library" (the
  `vxlapi64.dll` API python-can loads via ctypes). Installing the device driver alone leaves
  the API DLL absent (confirmed: nothing in `System32`/`SysWOW64`).
- **Fix:** install the free **Vector XL Driver Library** → `vxlapi64.dll` lands in `System32`
  (64-bit Python needs the 64-bit DLL). Then the channel opens.

### 3c. The gateway-side switch
`CAN_LOOPBACK_TEST = 0` (now the default): drops internal loopback + self-transmit + the stage
counters, re-enables the echo. Plus 120 Ω termination at both ends and the kit transceiver
enabled.

### 3d. Result
```
python host_tools/can_echo_probe/can_echo_probe.py
[ 0] echo OK   id=0x200 data=00 64 00
[ 1] echo OK   id=0x200 data=00 64 01
...
10/10 echoes received
```
The alternating `door_ajar` byte (00/01) confirms RX → decode → `bodyctl` → echo end to end,
now with a real transceiver, real bit timing, and a second node's ACK.

---

## Summary — what each phase proved

| Phase | Proves | Hardware |
|---|---|---|
| A (internal loopback) | the whole software path + ISR priority + the NVIC-mux routing | just the kit |
| B (real bus) | transceiver, bit timing on the wire, ACK from a second node | kit + VN1610 |

The two-phase split is the point: every fault in §2 was a *software/config* fault, isolated
from the bus. By the time the VN1610 was connected, the only new variables were physical
(transceiver, termination, timing on the wire) — and it worked first try.
