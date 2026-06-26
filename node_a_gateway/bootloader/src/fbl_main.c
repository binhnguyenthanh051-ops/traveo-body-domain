/*
 * fbl_main.c — FBL entry: wire the host-tested boot core (shared/boot) to the
 * target port. Deliberately minimal (ADR-0004): no scheduler, no RTOS.
 *
 * The decision logic lives in fbl_run_boot() (shared/boot, host-tested). This
 * file only performs the terminal action the core returns.
 */
#include "boot.h"
#include "fbl_port.h"
#include "cybsp.h"      /* target-only: clocks + BSP pins (incl. the user LED) */

int main(void)
{
    /* Bring up clocks and BSP pins so timing + the LED heartbeat work. */
    (void)cybsp_init();

    fbl_boot_action_t action = fbl_run_boot();

    if (action == FBL_ACTION_JUMP_APP)
    {
        /* De-init to the handover contract (ADR-0008 D4 / review B3), then jump.
         * jump_to_app reads MSP/reset from the app vector table and never
         * returns. */
        fbl_port_deinit_for_jump();
        fbl_port_jump_to_app((uint32_t)fbl_port_app_image_base());
    }

    /* Programming mode (M1: minimal; M3: UDS programming services). */
    fbl_port_enter_programming_mode();

    for (;;)
    {
        /* never returns to the startup code */
    }
}
