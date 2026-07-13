/*
 * main_cm0p.c — CM0+ entry (ADR-0017 D4, M4-bringup-plan.md).
 *
 * Replaces the vendor COMPONENT_CM0P_SLEEP prebuilt with OUR OWN buildable
 * CM0+ image, so shared/crypto's dispatch loop has a real project to land in.
 *   Seam 0: bring up clocks, start the CM4, idle.
 *   Seam 1: poll the shared mailbox and echo requests (proved on silicon).
 *   Seam 2 (here): the mailbox now carries an ENCODED crypto envelope — feed it
 *     to crypto_dispatch (host-tested), which routes CRYPTO_OP_HASH to the HW
 *     SHA-256 handler (crypto_ops_cm0p.c) and writes an encoded response back.
 *     Seam 3 adds the VERIFY_IMAGE (ECDSA) op.
 *
 * The loop POLLS the mailbox status rather than taking the IPC notify
 * interrupt; the notify ISR (letting the CM0+ sleep between requests) is a
 * later refinement.
 *
 * UNVALIDATED without hardware — the cybsp_init() bring-up call, the crypto
 * block enable/SHA sequence, and cross-core mailbox access are bench checks
 * (see the Makefile note and the bring-up plan). The CM4 vector-table address
 * was the S0-1 bench finding — now the absolute 0x1002_0000.
 */
#include "cy_pdl.h"
#include "cybsp.h"
#include "ipc_mailbox_map.h"   /* IPC_MAILBOX, ipc_mbx_status_t (shared/crypto) */
#include "crypto_dispatch.h"   /* crypto_dispatch (shared/crypto) */
#include "crypto_types.h"      /* CRYPTO_MAX_PAYLOAD */
#include "prot_config_cm0p.h"  /* cm0p_prot_install_walls (M4 Seam 5, ADR-0020) */

/* Defined in crypto_ops_cm0p.c: enable the HW Crypto block + bind the op table. */
void cm0p_crypto_service_init(void);

/* ABSOLUTE address of the CM4 image's vector table = flash base + the CM0+
 * region (fbl_cm4.ld: flash ORIGIN 0x1000_0000, FLASH_CM0P_SIZE 0x20000, so
 * CM4 .text/.isr_vector at 0x1002_0000). NOTE: Cy_SysEnableCM4()'s param is
 * misleadingly named "vectorTableOffset" but its doc defines it as the offset
 * "from memory address 0x00000000" — i.e. the ABSOLUTE address, written
 * straight into CPUSS->CM4_VECTOR_TABLE_BASE. Passing just 0x20000 (as an
 * earlier draft did) points the CM4 at 0x0002_0000 → garbage vectors →
 * immediate HardFault/lockup (M4 Seam-0 bring-up finding). Must be 1024-byte
 * aligned; 0x1002_0000 is. Kept named so it stays grep-able against the linker. */
#define CM4_VECTOR_TABLE_ADDR   0x10020000UL

/* S0-3 diagnostic (now 0 = real service): the WFI sleep test showed the CM0+
 * being active wasn't the erase problem — S0-4 (interrupts masked, so the flash
 * SROM IPC never serviced) was. Back to the normal mailbox service loop; the
 * flash IPC interrupt (now enabled) preempts it to run the SROM handler. */
#define CM0P_ERASE_TEST_SLEEP   0

/* Spin waiting for a request — FROM RAM, not flash (M4 Seam-0 finding S0-3).
 *
 * The CM4 erases/programs the app flash during UDS reprogramming. On this part,
 * an instruction fetch from a flash macro that is mid-erase is a read-while-
 * write violation and FAULTS (the same hazard as M3 Seam-7 #11, but on the
 * other core). If the CM0+ idles by busy-polling from its own flash, that fetch
 * faults the instant the CM4 starts an erase and wedges the operation — which
 * is exactly what the vendor CM0+ prebuilt avoided by sleeping. This idle spin
 * runs from RAM (.cy_ramfunc, copied at startup), so while the CM4 is busy
 * erasing flash the CM0+ fetches nothing from flash. No crypto request arrives
 * during a flash op (verify happens at boot / after transferExit, never mid-
 * erase), so control only leaves this RAM spin — back into the flash-resident
 * dispatch — when the flash is idle. Interrupts stay masked on the CM0+, so no
 * flash-resident ISR is fetched either. */
/* noinline is load-bearing: without it the optimizer inlines this one-call spin
 * back into main() (flash), defeating the .cy_ramfunc placement — the spin would
 * still fetch from flash and still fault during the CM4's erase. Forcing it
 * out-of-line makes the body genuinely execute from RAM. */
CY_SECTION_RAMFUNC_BEGIN
__attribute__((noinline)) static void cm0p_wait_for_request(void)
{
    while (IPC_MAILBOX->status != (uint32_t)IPC_MBX_REQUEST)
    {
        /* pure RAM spin; do not call into flash here */
    }
}
CY_SECTION_RAMFUNC_END

/* Seam 2: the mailbox payload is an encoded crypto envelope. Dispatch decodes
 * it into a local copy first (crypto_dispatch), so the in-place req==resp use
 * of the payload buffer is safe. crypto_dispatch always answers — a bad op or
 * a HW failure comes back as an ERROR verdict, never a dropped request. */
static void service_mailbox_once(void)
{
    if (IPC_MAILBOX->status == (uint32_t)IPC_MBX_REQUEST)
    {
        size_t resp_len = 0U;
        (void)crypto_dispatch((const uint8_t *)(uintptr_t)IPC_MAILBOX->payload,
                              (size_t)IPC_MAILBOX->len,
                              (uint8_t *)(uintptr_t)IPC_MAILBOX->payload,
                              CRYPTO_MAX_PAYLOAD,
                              &resp_len);
        IPC_MAILBOX->len = (uint32_t)resp_len;
        __DMB();                                     /* len lands before status */
        IPC_MAILBOX->status = (uint32_t)IPC_MBX_RESPONSE;
    }
}

/* ------------------------------------------------------------------
 * TEMP S0-4 DIAGNOSTIC — the CM0+ writes its state to a FIXED shared-SRAM
 * address so the CM4's OpenOCD session can read it (no CM0+ debug needed):
 *
 *     > mdw 0x0801F600 8
 *
 * The 8 words at 0x0801F600, in order:
 *   +0x00 progress : how far init got (1 cybsp, 2 crypto, 3 irq, 4 loop)
 *   +0x04 alive    : increments every loop pass -> CM0+ alive & looping
 *   +0x08 faulted  : 0xDEADF00D -> the CM0+ crashed (can't service SROM)
 *   +0x0C vtor     : SCB->VTOR (want RAM 0x0800_xxxx)
 *   +0x10 primask  : PRIMASK after __enable_irq (want 0)
 *   +0x14 iser0    : NVIC ISER[0]; bits 0 and 1 set => NvicMux0/1 (SROM) enabled
 *   +0x18 irq0_vec : active vector[16] = NvicMux0 handler (want a ROM addr, low)
 *   +0x1C irq1_vec : active vector[17] = NvicMux1 handler (want a ROM addr, low)
 *
 * 0x0801F600 sits in the mailbox region fbl_cm4.ld reserves (0x0801F500..+0x200),
 * past the ~136-byte mailbox — free scratch both cores can reach.
 * ------------------------------------------------------------------ */
#define CM0P_DBG_ADDR   0x0801F600UL
typedef struct {
    volatile uint32_t progress;
    volatile uint32_t alive;
    volatile uint32_t faulted;
    volatile uint32_t vtor;
    volatile uint32_t primask;
    volatile uint32_t iser0;
    volatile uint32_t irq0_vec;
    volatile uint32_t irq1_vec;
} cm0p_dbg_t;
#define CM0P_DBG   ((volatile cm0p_dbg_t *)CM0P_DBG_ADDR)

/* Override the weak fault handler so a CM0+ crash is visible (and stops here). */
void HardFault_Handler(void)
{
    CM0P_DBG->faulted = 0xDEADF00DU;
    for (;;) { /* trapped */ }
}

static void dbg_capture_state(void)
{
    const volatile uint32_t *vt = (const volatile uint32_t *)SCB->VTOR;
    CM0P_DBG->vtor     = SCB->VTOR;
    CM0P_DBG->primask  = __get_PRIMASK();
    CM0P_DBG->iser0    = NVIC->ISER[0];
    CM0P_DBG->irq0_vec = vt[16];   /* IRQ0 = NvicMux0 (SROM) */
    CM0P_DBG->irq1_vec = vt[17];   /* IRQ1 = NvicMux1 (SROM) */
}

/* S0-4 FIX: the BSP's PrepareSystemCallInfrastructure() (in SystemInit) never
 * ran on our custom CM0+ (dump showed IRQ0/1 vectors still in flash, NvicMux0/1
 * disabled). Replicate it: point the CM0+'s IRQ0/IRQ1 vectors at the SROM's own
 * handlers (SROM vector table @ 0x0000_0000, indices 16/17) and enable
 * NvicMux0/1, so this core services the CM4's flash erase/program SROM syscalls
 * (which block the CM4 on Cy_Srom_CallApi until we do). VTOR already points to
 * the RAM vector table, so we patch that copy. */
static void cm0p_setup_srom_syscalls(void)
{
    volatile uint32_t       *ramVec  = (volatile uint32_t *)SCB->VTOR;
    const volatile uint32_t *sromVec = (const volatile uint32_t *)0x00000000UL;

    ramVec[16] = sromVec[16];      /* IRQ0 = NvicMux0 -> SROM handler */
    ramVec[17] = sromVec[17];      /* IRQ1 = NvicMux1 -> SROM handler */
    __DSB();
    __ISB();

    NVIC_SetPriority(NvicMux0_IRQn, 1);
    NVIC_SetPriority(NvicMux1_IRQn, 0);
    NVIC_EnableIRQ(NvicMux0_IRQn);
    NVIC_EnableIRQ(NvicMux1_IRQn);
}

int main(void)
{
    /* Minimal clock/system bring-up. cybsp_init() itself is the App/FBL-side
     * convention (CLAUDE.md, ADR-0004); confirm at Seam 0 bring-up whether
     * this project needs the same call or a lighter CM0+-only init path —
     * cybsp.c's own comment notes peripheral config runs on "the first core
     * running user code... the CM0+ if available and not running a prebuilt
     * image" (cybsp.c ~line 124), which after this seam is exactly this
     * image, not the vendor prebuilt. */
    /* Zero the debug scratch first (its region is NOLOAD = garbage at boot). */
    CM0P_DBG->progress = 0U; CM0P_DBG->alive = 0U; CM0P_DBG->faulted = 0U;

    (void)cybsp_init();
    CM0P_DBG->progress = 1U;

    /* Enable the HW Crypto block + bind the op table BEFORE the CM4 starts, so
     * the service is ready by the time the FBL issues its first request. */
    cm0p_crypto_service_init();
    CM0P_DBG->progress = 2U;

    /* S0-4: the CM4's flash erase/program is an SROM system call serviced by the
     * CM0+ via NvicMux0/1. The BSP's PrepareSystemCallInfrastructure() that wires
     * that up never ran on our custom image (dump: IRQ0/1 in flash, NvicMux0/1
     * off), so install it now; then clear PRIMASK so those IRQs can fire (the
     * vendor startup leaves it masked — the CM0+ twin of M3 #5). Without both,
     * the CM4 hangs forever in Cy_Srom_CallApi's `IsLockAcquired(syscall)` wait. */
    cm0p_setup_srom_syscalls();
    __enable_irq();
    dbg_capture_state();       /* VTOR / PRIMASK / NVIC / SROM vectors, post-enable */
    CM0P_DBG->progress = 3U;

    /* M4 Seam 5 (ADR-0020 D2): raise the TCB-isolation walls (SMPU over the key
     * flash, PPUs over CRYPTO) and assign the CM0+/CM4 protection contexts
     * BEFORE releasing CM4 — the walls must exist before the untrusted core runs
     * one instruction. No-op unless FBL_M4_SEAM5_PROT is defined. */
    cm0p_prot_install_walls();

    /* Release the CM4 core to start executing the FBL at its vector table. */
    Cy_SysEnableCM4(CM4_VECTOR_TABLE_ADDR);
    CM0P_DBG->progress = 4U;

#if CM0P_ERASE_TEST_SLEEP
    /* TEMP S0-3 DIAGNOSTIC: sleep like the vendor prebuilt did — definitively
     * off the flash bus (no fetch, no optimizer/ramfunc uncertainty). If the
     * CM4 erase now COMPLETES, the CM0+ was the cause and the real fix is a
     * RAM-resident idle + notify-ISR wake. If it STILL hangs, the CM0+ is NOT
     * the cause. No wake source here, so crypto verify will time out — fine for
     * the erase test (the unsigned app is rejected anyway). Revert to 0 after. */
    (void)cm0p_wait_for_request;   /* keep referenced (no -Wunused) while sleeping */
    (void)service_mailbox_once;
    for (;;)
    {
        __WFI();
    }
#else
    /* Idle by spinning in RAM (cm0p_wait_for_request, .cy_ramfunc), NOT flash:
     * during the 49-row download the CM4 programs flash, and a CM0+ instruction
     * fetch from the busy macro faults (RWW, S0-3). This spin fetches nothing
     * from flash, so the CM0+ survives the whole download; it enters the
     * flash-resident dispatch only when a crypto request is pending (check-image
     * / boot verify), which never coincides with a flash op. Combined with the
     * S0-4 SROM-syscall setup above, this is the complete fix. */
    for (;;)
    {
        cm0p_wait_for_request();
        service_mailbox_once();
    }
#endif
}
