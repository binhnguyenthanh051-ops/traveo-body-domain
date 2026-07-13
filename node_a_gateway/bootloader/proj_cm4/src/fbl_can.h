/*
 * fbl_can.h — the FBL's own CANFD bring-up (M3 Seam 1; ADR-0004, ADR-0011,
 * ADR-0013 D5). Bootloader-internal: not in shared/can/include because the
 * App's own CAN target code (node_a_gateway/app/src/can_task.c) isn't there
 * either -- shared/can only holds the plain interface type (can_hal_if_t);
 * each image owns its target wiring. Kept in src/, not a public include/,
 * per the internal-header convention in docs/coding-standard.md.
 */
#ifndef FBL_CAN_H
#define FBL_CAN_H

#include "can_hal.h"

/* Bring up CANFD (Cy_CANFD_Init from the Device Configurator's generated
 * channel config). Call once, early in fbl_main(), before fbl_run_boot() --
 * the knock window (ADR-0008 D2) needs CAN already alive to detect a knock. */
void fbl_can_init(void);

/* The FBL's can_hal_if_t instance (send + the M3 poll-based recv). For Seam
 * 2's composition root to hand to isotp_init(). */
const can_hal_if_t *fbl_can_hal(void);

/* Best-effort bounded wait for the last send to actually leave the TX buffer
 * (silicon-verified during Seam 4 bring-up: a response sent immediately
 * before ECUReset's system reset needs this, or the reset can cut the
 * in-flight frame's transmission short before it ever reaches the bus). */
void fbl_can_wait_tx_complete(void);

#endif /* FBL_CAN_H */
