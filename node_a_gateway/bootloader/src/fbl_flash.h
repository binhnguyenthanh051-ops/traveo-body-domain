/*
 * fbl_flash.h — the FBL's flash driver bring-up (M3 Seam 6). Bootloader-
 * internal, same rationale as fbl_can.h/fbl_time.h (src/, not include/).
 *
 * Bound to the app-image region only (FBL_APP_FLASH_BASE/SIZE, boot_types.h)
 * -- never the FBL's own code (0x1002_0000-0x1004_0000). port_flash.c
 * enforces this at every call, not just by convention: writing/erasing
 * outside the app region on real hardware is not a recoverable mistake.
 */
#ifndef FBL_FLASH_H
#define FBL_FLASH_H

#include "hal.h"

/* Enable program/erase of MAIN (code) flash, where the app image lives.
 * REQUIRED before any write/erase: this part's flash controller has a safety
 * register that defaults to writes-DISABLED, and program/erase return
 * CY_FLASH_DRV_FLASH_SAFTEY_ENABLED until it is turned on (silicon-verified
 * during Seam 6 bring-up). Call once when the FBL commits to programming mode
 * -- deliberately NOT on the boot/jump path, so main-flash writes stay
 * disabled whenever the FBL hands off to the app. */
void fbl_flash_init(void);

/* The FBL's hal_flash_if_t instance for the app-image region. */
const hal_flash_if_t *fbl_flash_hal(void);

/* The hardware erase-sector size (bytes) of the sector containing addr.
 * Code flash on this part is MIXED geometry: 32 KB large sectors at the
 * bottom, 8 KB small sectors at the top (TRM / bench-confirmed) -- so this
 * is 32768 or 8192 depending on addr. Exposed for target-side callers that
 * must walk real sector boundaries (e.g. the Seam 6 bring-up test); the
 * generic hal_flash_if_t stays sector-size-agnostic and callers in
 * shared/diag erase the whole app region via erase_range(), which handles
 * the mixed geometry internally. */
uint32_t fbl_flash_sector_size(uint32_t addr);

#endif /* FBL_FLASH_H */
