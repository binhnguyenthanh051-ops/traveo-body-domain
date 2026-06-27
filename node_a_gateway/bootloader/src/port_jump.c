/*
 * port_jump.c — de-init + the VTOR/MSP/branch handover (target).
 *
 * Two pieces (ADR-0008 D4): deinit_for_jump() is ordinary C; jump_to_app() is a
 * naked asm helper so the compiler inserts no stack use between the MSP-set and
 * the branch. Written now against the B3 handover contract; it just cannot be
 * *run* until the board. Board-gated register writes are marked TODO.
 */
#include "fbl_port.h"
#include "cy_pdl.h"   /* CMSIS core: SysTick, SCB, NVIC */

/*
 * B3 handover contract: leave all IRQ sources disabled and pending cleared so a
 * leftover exception cannot vector through the app's (not-yet-initialised)
 * vector table after VTOR is switched. The app re-enables interrupts and
 * re-inits clocks/peripherals in its own startup.
 *
 * This MUST be real: skipping it means a clean POR jump works but a jump taken
 * after the app has run (e.g. a software reset) carries dirty interrupt state
 * into the app and HardFaults.
 */
void fbl_port_deinit_for_jump(void)
{
    __disable_irq();                              /* PRIMASK = 1 during the swap */

    /* Stop SysTick and drop any pending SysTick/PendSV exception. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* Disable and clear every NVIC peripheral interrupt (CM4: 8 banks). */
    for (uint32_t bank = 0U; bank < 8U; bank++)
    {
        NVIC->ICER[bank] = 0xFFFFFFFFU;           /* disable  */
        NVIC->ICPR[bank] = 0xFFFFFFFFU;           /* un-pend  */
    }

    __DSB();
    __ISB();
    /* PRIMASK left set; the app (or its RTOS) re-enables IRQs in its startup. */
}

/*
 * Given the app vector-table base in r0: set VTOR, load the initial MSP and
 * reset vector, and branch. Nothing stack-dependent runs between the MSP-set
 * and the branch. Never returns.
 *
 * MISRA Dir 1.1 / Rule 4.3 deviation: assembly encapsulated in this dedicated
 * port function (see docs/coding-standard.md).
 */
__attribute__((naked, noreturn))
void fbl_port_jump_to_app(uint32_t app_base)
{
    (void)app_base;   /* consumed via r0 in the asm below */
    __asm volatile (
        "ldr  r1, [r0]            \n"   /* r1 = app_base[0] = initial MSP   */
        "ldr  r2, [r0, #4]        \n"   /* r2 = app_base[1] = reset vector  */
        "ldr  r3, =0xE000ED08     \n"   /* r3 = &SCB->VTOR                  */
        "str  r0, [r3]            \n"   /* VTOR = app_base                  */
        "dsb                      \n"
        "msr  msp, r1             \n"   /* set MSP (no stack use after this)*/
        "isb                      \n"
        "bx   r2                  \n"   /* branch to app reset; no return   */
    );
}
