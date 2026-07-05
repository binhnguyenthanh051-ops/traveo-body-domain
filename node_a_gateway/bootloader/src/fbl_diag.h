/*
 * fbl_diag.h — M3 Seam 4: the real UDS diagnostic stack composition root.
 * Bootloader-internal, same rationale as fbl_can.h/fbl_time.h (src/, not
 * include/).
 */
#ifndef FBL_DIAG_H
#define FBL_DIAG_H

#include <stdint.h>

/* Wire the dispatch table and bind isotp to the CAN instance. Call once,
 * inside fbl_port_enter_programming_mode(), before the first fbl_diag_tick(). */
void fbl_diag_init(void);

/* Drive one iteration of the real UDS session. Call once per FBL super-loop
 * iteration while resident in programming mode. */
void fbl_diag_tick(uint32_t now_ms);

#endif /* FBL_DIAG_H */
