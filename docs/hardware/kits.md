# Kit facts — the things the user guides don't make obvious

Board facts we had to find by hand during bring-up, so nobody has to find them twice.
Each line says where it came from. "Bench" means measured or observed on our kits.

## Node B — KIT_T2G-B-H_LITE (CYT4BF8CDS, Body High)

**Guide:** `docs/references/Infineon_KIT_T2G_B_H_LITE_user_guide_UserManual_v0-3304082.pdf`
(002-37237 Rev. *A). The schematics (§4.3, p. 24 on) are **images only**, so `pdftotext` can't read
them. The CAN transceiver is on **p. 35, "Figure 24 CAN and QSPI Interface"**.

### CAN FD

| Fact | Value | Source |
|---|---|---|
| Transceiver | **TLE9251VSJ (U8)** | guide, pin table |
| MCU pins | **P12_0 = CAN_TX → U8 TxD**, **P12_1 = CAN_RX ← U8 RxD**; CANFD0 **channel 2** | guide; BSP |
| Pin routing | **The BSP leaves P12_0/P12_1 as analog GPIO.** Routed in code: `can_pins_init()`, `P12_0_CANFD0_TTCAN_TX2` / `P12_1_CANFD0_TTCAN_RX2` (mux 21) | BSP `cycfg_pins.h`; runbook #15 |
| Connector **J5** | **pin 1 = CANL, pin 2 = CANH**, pin 3 = GND | bench (user, 2026-10-04) |
| Termination | **J23 (120 Ω jumper) unmounted**: no on-board termination, so fit 120 Ω externally at this end | bench (user) |
| **J32** | **Must be removed for CAN to work.** Fitted, the transceiver never echoes its own TX: Bit0 errors and bus-off within 1 s. Function per silkscreen: *TBD* | bench, runbook #16 |
| Bit timing | CAN clock 40 MHz (100 MHz peri ÷ 2.5). **The configurator's nominal timing gives 571 kbit/s**; overridden in `can_task.c` to 5 × (1+11+4) = 500 kbit/s, SP 75%. Data 1 × 20 = 2 Mbit/s | runbook #15 |

### LEDs and buttons — the silkscreen trap

| Code name | Pin | **Silkscreen** | Notes |
|---|---|---|---|
| USER LED1 (`CYBSP_USER_LED`) | P5_0 | **LED3** | heartbeat |
| USER LED2 (`CYBSP_USER_LED2`) | P5_1 | **LED4** | courtesy light (Stage 5.5). The BSP leaves it ANALOG; set in code |
| USER LED3 (`CYBSP_USER_LED3`) | P5_2 | **LED5** | |
| — | — | **LED2** | **power LED, always on.** It is *not* USER LED2 |
| — | — | LED1 | KitProg3 status (amber) |
| USER1 button (`CYBSP_USER_BTN1`) | **P5_3** | **SW3** | door-ajar switch (Stage 5.5). Active low; **the BSP leaves it HIGHZ**, so a pull-up is set in code |
| USER2 button (`CYBSP_USER_BTN2`) | P17_0 | SW4 | unused |

LEDs are active LOW (guide).

## Node A — CYTVII-B-E-1M-SK (CYT2B7, Body Entry)

| Fact | Value | Source |
|---|---|---|
| CAN | CANFD0 **channel 1**, P0_2 (TX) / P0_3 (RX), routed by the BSP. 500 kbit/s / 2 Mbit/s, nominal 5 × (1+11+4), SP 75% | `can_task.c`, BSP |
| Real bus | Proven since M2 (echo probe 10/10) | M2 bring-up log |

## The bench

| Fact | Value | Source |
|---|---|---|
| Log ports (KitProg3 UART bridge) | **COM4 = Node A**, **COM6 = Node B**. Windows can renumber them if the USB socket changes: check the banner (`fbl`/`app` vs `actuator`) | user, 2026-10-03 |
| Flashing | Goes over the probe's USB debug interface, **not** the COM port. With both kits plugged in, the probe is chosen by serial number (F-012, open) | — |
| CAN analyser | Vector VN1611 (1 CAN FD channel) or VN1610, via python-can (`host_tools/can_echo_probe/config.json`, app `python-can`, channel 0) | bench |
| Bus | Node A ↔ Node B J5 ↔ VN161x, **120 Ω at both ends** (Node B has none on board) | bench |
| Echo probe | Node echo of unknown IDs is **off by default** (`CAN_ECHO_TEST=1` to enable). Run it with only **one** echoing node on the bus, or the two echo each other forever | runbook #19 |
