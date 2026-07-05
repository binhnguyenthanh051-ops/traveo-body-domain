/*
 * uds_routine_control.h — 0x31 RoutineControl: erase + CRC verify (ADR-0012
 * D1 layer 3, D6/D8).
 *
 * Verify reuses shared/boot's fbl_digest()/fbl_app_image_valid() (ADR-0008
 * D3) directly -- no new verify_if_t interface. Both the boot-time jump-check
 * and this UDS-triggered check live in the same FBL image built with the same
 * FBL_DIGEST_ALGO macro, so M4 flips one macro and neither call site changes
 * (ADR-0012 D6).
 */
#ifndef UDS_ROUTINE_CONTROL_H
#define UDS_ROUTINE_CONTROL_H

#include "uds_handler.h"
#include "hal.h"

/* Routine identifiers (this project's own scheme, not a standard UDS list). */
#define UDS_ROUTINE_ERASE_MEMORY             0xFF00U
#define UDS_ROUTINE_CHECK_PROGRAMMED_IMAGE   0xFF01U

/* Bind the flash instance this routine erases (same instance as
 * uds_download.h's app-image flash -- shared, not duplicated), and the
 * memory-mapped app image view the check routine calls
 * fbl_app_image_valid() over. app_base/app_region_len are passed in rather
 * than fetched via a port call (e.g. fbl_port_app_image_base()) so this
 * module stays image-agnostic -- the FBL's composition root supplies its own
 * port's values; a future App-side instance would supply its own. Call once
 * at composition time. */
void uds_routine_control_init(const hal_flash_if_t *flash,
                               const uint8_t *app_base, uint32_t app_region_len);

const uds_handler_if_t *uds_routine_control_handler(void);

#endif /* UDS_ROUTINE_CONTROL_H */
