/*
 * port_jump.c — de-init + the VTOR/MSP/branch handover (target).
 *
 * Two pieces (ADR-0008 D4): deinit_for_jump() is ordinary C; jump_to_app() is a
 * naked asm helper so the compiler inserts no stack use between the MSP-set and
 * the branch. Written now against the B3 handover contract; it just cannot be
 * *run* until the board. Board-gated register writes are marked TODO.
 */
#include "fbl_port.h"

/*
 * B3 handover contract: leave all IRQ sources disabled and pending cleared,
 * peripherals toward reset state. The app re-enables interrupts and re-inits
 * clocks/peripherals in its own startup.
 */
void fbl_port_deinit_for_jump(void)
{
    __asm volatile ("cpsid i" ::: "memory");   /* disable IRQs (PRIMASK) */

    /* TODO(M1, board), via CMSIS device header:
     *   SysTick->CTRL = 0u;                       // stop SysTick
     *   SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;       // clear SysTick pending
     *   for (i in NVIC banks) { NVIC->ICER[i] = 0xFFFFFFFFu;
     *                           NVIC->ICPR[i] = 0xFFFFFFFFu; }
     *   // return FBL-touched peripherals (CAN, clocks) toward reset state
     */
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
