/*
 * port_image.c — app-image / flash access (target).
 *
 * Board-independent: code flash is memory-mapped, so the core just gets a
 * pointer + length and does the digest + vector checks itself (host-tested).
 * HW-CRC acceleration via the crypto block is optional and deferred.
 *
 * Addresses come from boot_types.h (FBL_APP_FLASH_BASE/SIZE) — verify in TRM.
 */
#include "fbl_port.h"

const uint8_t *fbl_port_app_image_base(void)
{
    /* MISRA Dir 1.1 / Rule 11.4 deviation: integer-to-pointer is required to
     * address memory-mapped flash. Confined to this accessor. */
    return (const uint8_t *)FBL_APP_FLASH_BASE;
}

uint32_t fbl_port_app_region_len(void)
{
    return FBL_APP_FLASH_SIZE;
}
