/*
 * main_cm0p.c — Node B CM0+ bring-up entry.
 *
 * M5 Seam 1: this core owns the BSP-wide configuration and releases CM7_0, then
 * idles. No IPC/crypto yet — the shared mailbox (and its Node-B-specific map,
 * reserved in both linker scripts and placed in the ram_noncache region per
 * ADR-0018 D5/D6) lands at the crypto seam. Do NOT reuse Node A's
 * ipc_mailbox_map.h here: its address (0x0801F500) and IPC channel (CAT1A) are
 * CYT2B7-specific.
 */
#include "cybsp.h"
#include "cy_pdl.h"

/* CM7_0 image base = code_flash_base_address (0x1000_0000) +
 * cm0plus_code_flash_reserve (0x8_0000, 512K) — must equal _base_CODE_FLASH_CM7_0
 * in the BSP's COMPONENT_CM7 linker.ld. */
#define CM7_0_VECTOR_TABLE_ADDR   0x10080000UL
#define CM7_0_CORE_INDEX          CORE_CM7_0

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

    Cy_SysEnableCM7(CM7_0_CORE_INDEX, CM7_0_VECTOR_TABLE_ADDR);

    for (;;)
    {
        __WFI();
    }
}
