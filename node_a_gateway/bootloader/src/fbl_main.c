/*
 * fbl_main.c — FBL entry: wire the host-tested boot core (shared/boot) to the
 * target port. Deliberately minimal (ADR-0004): no scheduler, no RTOS.
 *
 * The decision logic lives in fbl_run_boot() (shared/boot, host-tested). This
 * file only performs the terminal action the core returns.
 */
#include "boot.h"
#include "fbl_port.h"
#include "fbl_can.h"    /* M3 Seam 1: CAN must be alive before the knock window polls it */
#include "fbl_time.h"   /* M3 Seam 2 prerequisite: real ms tick for N_Cr / the knock dwell */
#include "cybsp.h"      /* target-only: clocks + BSP pins (incl. the user LED) */
#include "cy_pdl.h"     /* target-only: __enable_irq (CMSIS core intrinsic) */

int main(void)
{
    /* Bring up clocks and BSP pins so timing + the LED heartbeat work. */
    (void)cybsp_init();

    /* The vendor startup leaves PRIMASK set (interrupts globally masked)
     * after SystemInit()/cybsp_init(), expecting whatever runs next to turn
     * them back on -- an RTOS scheduler start does this for the App for
     * free (ADR-0010). The FBL is bare-metal (ADR-0004: no RTOS), so nothing
     * else will ever do this; without it, SysTick counts down correctly in
     * hardware but its interrupt is never taken, so fbl_port_now_ms() never
     * advances (silicon-verified during M3 Seam 3 bring-up: PRIMASK read
     * back as 1, s_tick_ms stuck at 0 -- the knock dwell's timeout branch
     * could then only ever be escaped by a knock, never by elapsed time). */
    __enable_irq();

    /* Real timing before anything that depends on fbl_port_now_ms() being
     * wall-clock accurate (the knock dwell just below, and ISO-TP's N_Cr
     * timeout once Seam 2 is wired in). */
    fbl_time_init();

    /* CAN must be up before fbl_run_boot(), not only once programming mode is
     * entered: the knock window (ADR-0008 D2) needs fbl_port_tool_contact()
     * to see real frames during the dwell, which happens inside
     * fbl_run_boot() itself. If CAN fails to come up, tool_contact() simply
     * never reports contact -- indistinguishable from "nobody knocked" to
     * the boot decision (ADR-0015 D6: no escalation needed here either). */
    fbl_can_init();

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
