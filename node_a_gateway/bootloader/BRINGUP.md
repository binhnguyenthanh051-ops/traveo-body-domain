# M1 board bring-up — "hardware is alive" checkpoint

Board: `CYTVII-B-E-1M-SK` (MCU `CYT2B7`, TRAVEO™ T2G). Goal of this step: a **bare blinky**
that flashes, blinks, and holds a debug session. **Nothing of our FBL goes on top until this
is solid** — this isolates "board + toolchain + debugger work" from "our code is correct."

> Division of labour: the steps below are run **on the bench** in ModusToolbox with the board
> connected (interactive + physical). The repo's job is to record the procedure, the
> acceptance criteria, and the integration plan — not to drive the IDE.

## A. Prove the board is alive (vendor code, not ours)

Use a **known-good vendor example** so a failure points at hardware/toolchain, not our logic.

1. **Toolchain check.** ModusToolbox 3.x installed; board connected via the onboard
   **KitProg3** (USB). Confirm KitProg3 is in **CMSIS-DAP/DAPLink mode** (KitProg3 has a mode
   button + status LED; see the kit guide). Toolchain = `GCC_ARM` (ships with MTB).
2. **Create the project.** *Project Creator* → select BSP **`CYTVII-B-E-1M-SK`** → pick a
   minimal starter (a **GPIO/Blinky** or **Hello World** code example for the kit). Let it
   pull the BSP + PDL/HAL libraries.
   - CLI equivalent: `project-creator-cli --board-id CYTVII-B-E-1M-SK --app-id <example> ...`
     (confirm the exact `--app-id` from `project-creator-cli --list` — names verify in MTB).
3. **Build.** Eclipse Quick Panel **Build**, or `make build -j` in the project dir.
4. **Program.** Quick Panel **Program**, or `make program`. The user LED should blink.
5. **Debug.** Quick Panel **Debug (KitProg3)**. Confirm: halt at `main`, set a breakpoint in
   the blink loop, single-step, read a variable, resume. This is the real "alive" proof —
   flashing alone isn't enough; the **debug session is the checkpoint**.

### Reference blink (TRAVEO T2G / PDL) — if you write it yourself instead of the example
Uses BSP macros so there is **no pin guessing** (the BSP defines the user LED for this kit):
```c
#include "cy_pdl.h"
#include "cybsp.h"

int main(void)
{
    (void)cybsp_init();          /* clocks + pin config from the Device Configurator */
    __enable_irq();
    for (;;)
    {
        Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
        Cy_SysLib_Delay(500U);   /* ms */
    }
}
```
Verify against the generated BSP: that `CYBSP_USER_LED` is exported, and that the LED pin is
configured as a strong-drive output in the **Device Configurator** (if not, add
`Cy_GPIO_Pin_FastInit(...)` or enable it there). Confirm the user-LED location in the kit guide.

## B. Acceptance — the checkpoint is "solid" when ALL hold

- [ ] `make build` clean with `GCC_ARM`.
- [ ] `make program` succeeds over KitProg3; the user LED blinks at the expected rate.
- [ ] Debug session attaches; halt at `main`, breakpoint hits, single-step works.
- [ ] Read/modify a variable in the debugger; resume; board keeps running.
- [ ] Power-cycle (unplug/replug) → the programmed blinky runs from **cold boot**, not just
      under the debugger. (Cold-boot-only behaviour is the classic trap — see ADR-0008 D4.)

Only when every box is ticked do we build the FBL on top.

## C. After alive — bring the FBL in (NEXT step, not now)

Staged so a regression is attributable:
1. Replace the example `main()` with our `fbl_main.c`; add `shared/boot` to the project
   include/sources; bring in the `port_*.c` from `src/`.
2. Fill the board-gated port stubs against the TRM, in dependency order:
   `port_time` (SysTick) → `port_reset` (SRSS reset cause) → `port_noinit` + ECC priming →
   `port_backup` (BREG) → `port_security` (lifecycle) → `port_can` (knock) → `port_jump`.
3. Reconcile the linker/startup: start from MTB's generated `.ld`/startup, then fold in our
   `.noinit` placement and the cold-boot SRAM-ECC-init from `linker/fbl.ld` / `startup_fbl.c`.
4. Validate each TRM assumption flagged in ADR-0007/0008 (reset-cause `cause==0 ⇒ POR`, ECC
   on uninitialised SRAM, BREG retention across hibernate/POR, the cold-boot jump).
5. **Keep the lifecycle OPEN (pre-SECURE) during bring-up** — do **not** burn fuses (ADR-0008
   D5; the fuse/lifecycle step is gated and deferred to ~M4).

## Repo notes
- MTB build output (`build/`, `libs/`, `bsps/` deps) is generated — already covered by
  `.gitignore` (`**/build/`, `**/libs/`). Commit the app sources, `*.modus` configurator
  files, `deps/*.mtb`, and `Makefile`; not the downloaded libraries or build artifacts.
- The MTB project lives under `node_a_gateway/bootloader/` alongside this scaffold.
