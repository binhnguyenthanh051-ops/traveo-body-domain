/*
 * port_backup.c — backup-domain (BREG) read/write (target).
 *
 * The boot-loop counter (+ its magic) lives in the BACKUP block's BREG[]
 * registers (BACKUP_Type::BREG[64], always-on domain): they survive hibernate
 * and warm resets, and clear on power loss (ADR-0007 D1/D6). The register map is
 * in boot_types.h — FBL block at [FBL_BREG_MAGIC_IDX, FBL_BREG_COUNTER_IDX];
 * the App partition is reserved from FBL_BREG_APP_BASE_IDX.
 *
 * We access BACKUP->BREG[idx] directly rather than via Cy_SysPm_BackupWordStore/
 * ReStore. Those wrappers CY_ASSERT_L3(index > 0), which rejects BREG[0] and
 * fires a BKPT — even though their own API doc says the index "starts with 0",
 * and their bodies are a plain BACKUP_BREG[idx] access. Attached, the BKPT just
 * halts; detached (e.g. after a power cycle) it escalates to a DEBUGEVT
 * HardFault. Direct access is identical in effect, without the spurious assert.
 */
#include "fbl_port.h"
#include "cy_pdl.h"

uint32_t fbl_port_backup_read(uint8_t idx)
{
    return BACKUP->BREG[idx];
}

void fbl_port_backup_write(uint8_t idx, uint32_t val)
{
    BACKUP->BREG[idx] = val;
}
