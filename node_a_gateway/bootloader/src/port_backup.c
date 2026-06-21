/*
 * port_backup.c — backup-domain (BREG) read/write (target).  SKELETON.
 *
 * The boot-loop counter lives in the backup domain so it survives hibernate and
 * warm resets but clears on power loss (ADR-0007 D1/D6). The shape is here; the
 * register access is board-gated.
 *
 * TODO(M1, board): map idx to the BACKUP->BREG[] registers (e.g. via the PDL /
 * CMSIS device header). Honour the register map in boot_types.h: FBL block at
 * [FBL_BREG_MAGIC_IDX, FBL_BREG_COUNTER_IDX]; the App partition is reserved from
 * FBL_BREG_APP_BASE_IDX. Confirm BREG count + retention semantics in the TRM.
 */
#include "fbl_port.h"

uint32_t fbl_port_backup_read(uint8_t idx)
{
    (void)idx;
    /* TODO(M1, board): return BACKUP->BREG[idx]; */
    return 0U;       /* 0 => magic mismatch => counter treated as fresh (safe) */
}

void fbl_port_backup_write(uint8_t idx, uint32_t val)
{
    (void)idx;
    (void)val;
    /* TODO(M1, board): BACKUP->BREG[idx] = val; */
}
