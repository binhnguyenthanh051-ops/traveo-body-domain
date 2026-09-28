/*
 * main_cm0p.c — Node B CM0+ entry.
 *
 * Two jobs (ADR-0017 D4, mirrored for Node B):
 *   Seam 1: release CM7_0, then idle. (bring-up — done.)
 *   Seam 3 (here): stand up the CRYPTO_OP_MAC service. Enable the Crypto block
 *     and bind the dispatch table (cm0p_crypto_service_init, crypto_ops_cm0p.c)
 *     BEFORE releasing CM7_0, then poll the cross-core mailbox: an encoded crypto
 *     envelope in -> crypto_dispatch (host-tested) -> AES-CMAC handler -> encoded
 *     response out. The CM0+ is the mailbox SERVER — it reads/writes the shared
 *     region directly and does NOT link ipc_mailbox.c (ADR-0018; mirrors Node A).
 *
 * Deliberately SIMPLER than Node A's CM0+ loop — the two hazards that forced
 * Node A's complexity do not exist on Node B in M5:
 *   - No RAM-resident idle spin (.cy_ramfunc). Node A must idle from RAM because
 *     the CM4 erases app flash during UDS reprogramming and a CM0+ flash fetch
 *     mid-erase faults (RWW). Node B has no FBL / no reprogramming in M5, and the
 *     freshness store is a host fake now / eeprom_emu in M6 — nothing erases flash
 *     while this loop runs, so a normal flash-resident poll is safe. REVISIT at M6
 *     when eeprom_emu writes work flash.
 *   - No SROM-syscall (NvicMux0/1) setup. That services the OTHER core's flash
 *     erase/program syscalls; Node B does no cross-core flash ops in M5.
 * Both are VERIFY-on-bench items for CAT1C, not assumptions to trust blindly.
 *
 * The loop POLLS the mailbox status; the IPC notify ISR (letting the CM0+ sleep
 * between MACs) is a later refinement, exactly as on Node A.
 */
#include "cybsp.h"
#include "cy_pdl.h"
#include "ipc_mailbox_map.h"   /* IPC_MAILBOX_B, ipc_mbx_status_t (Node B map) */
#include "crypto_dispatch.h"   /* crypto_dispatch (shared/crypto) */
#include "crypto_types.h"      /* CRYPTO_MAX_PAYLOAD */

/* CM7_0 image base = code_flash_base (0x1000_0000) + cm0plus_code_flash_reserve
 * (0x8_0000, 512K) — must equal _base_CODE_FLASH_CM7_0 in the COMPONENT_CM7
 * linker.ld. */
#define CM7_0_VECTOR_TABLE_ADDR   0x10080000UL
#define CM7_0_CORE_INDEX          CORE_CM7_0

/* Defined in crypto_ops_cm0p.c: enable the HW Crypto block + bind the MAC op table. */
void cm0p_crypto_service_init(void);

#if defined(CRYPTO_BRINGUP_KAT) && (CRYPTO_BRINGUP_KAT != 0)
/* M5 bench Stage 2 + 4.1 (crypto_ops_cm0p.c): CMAC known-answer test. */
void cm0p_cmac_kat(void);
#endif

/* Service one mailbox request in place (mirrors Node A's service_mailbox_once).
 * crypto_dispatch decodes into a local copy first, so req==resp reuse of the
 * payload buffer is safe, and it ALWAYS answers — a bad op / unknown key / HW
 * failure comes back as an ERROR verdict, never a dropped request. */
static void service_mailbox_once(void)
{
    if (IPC_MAILBOX_B->status == (uint32_t)IPC_MBX_REQUEST)
    {
        size_t resp_len = 0U;
        (void)crypto_dispatch((const uint8_t *)(uintptr_t)IPC_MAILBOX_B->payload,
                              (size_t)IPC_MAILBOX_B->len,
                              (uint8_t *)(uintptr_t)IPC_MAILBOX_B->payload,
                              CRYPTO_MAX_PAYLOAD,
                              &resp_len);
        IPC_MAILBOX_B->len = (uint32_t)resp_len;
        __DMB();                                     /* len lands before status */
        IPC_MAILBOX_B->status = (uint32_t)IPC_MBX_RESPONSE;
    }
}

int main(void)
{
    cy_rslt_t result = cybsp_init();

    if (result != CY_RSLT_SUCCESS)
    {
        for (;;)
        {
            /* Halt on BSP bring-up failure. */
        }
    }

    /* SystemInit prepared the SROM IRQ plumbing; main() must unmask IRQs. */
    __enable_irq();

    /* Enable the HW Crypto block + bind the MAC op table BEFORE CM7_0 starts, so
     * the service is ready by the time the app issues its first MAC request. */
    cm0p_crypto_service_init();

#if defined(CRYPTO_BRINGUP_KAT) && (CRYPTO_BRINGUP_KAT != 0)
    /* Bench Stage 2: prove the CMAC primitive on this silicon before the other
     * core can ask for a MAC. Result in g_kat_result (debugger). */
    cm0p_cmac_kat();
#endif

    Cy_SysEnableCM7(CM7_0_CORE_INDEX, CM7_0_VECTOR_TABLE_ADDR);

    for (;;)
    {
        service_mailbox_once();
    }
}
