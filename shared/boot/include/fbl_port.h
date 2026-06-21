/*
 * fbl_port.h — the FBL hardware seam (the shared/hal pattern).
 *
 * The boot core (boot.c) depends ONLY on these functions; the linker selects
 * either the target implementation (ModusToolbox: SRSS reset cause, backup
 * registers, CAN poll, flash, the VTOR/MSP jump) or the host fake for tests.
 * This mirrors the scheduler's sched_port.h: a singleton hardware seam kept
 * with its module rather than in the generic hal.h. (ADR-0001, ADR-0007/0008.)
 */
#ifndef FBL_PORT_H
#define FBL_PORT_H

#include "boot_types.h"
#include <stdbool.h>

/* ---- Reset cause + lifecycle (ADR-0007 D2, ADR-0008 D2) ---- */

/* Normalised reset cause; target reads + decodes the SRSS register(s). */
fbl_reset_cause_t fbl_port_reset_cause(void);

/* Clear the reset-cause register so the next boot reads a clean cause. */
void              fbl_port_clear_reset_cause(void);

/* Device lifecycle stage (gates the knock window). */
fbl_lifecycle_t   fbl_port_lifecycle(void);

/* ---- .noinit shared region + ECC priming (ADR-0007 D1/D3) ---- */

/* Pointer to the .noinit handshake region (a fake buffer on host). */
fbl_handshake_t  *fbl_port_noinit(void);

/* Prime the region: write it to establish valid ECC (cold/hibernate). On host
 * this just zeroes the backing buffer. */
void              fbl_port_prime_noinit(void);

/* ---- Backup registers — FBL-private (ADR-0007 D1/D6) ---- */

uint32_t          fbl_port_backup_read(uint8_t idx);
void              fbl_port_backup_write(uint8_t idx, uint32_t val);

/* ---- Time + tool contact for the knock dwell (ADR-0008 D2) ---- */

/* Monotonic milliseconds. */
uint32_t          fbl_port_now_ms(void);

/* True once a tool has made contact: M1 = a fixed knock frame on the
 * diagnostic CAN ID; M3 = a UDS DiagnosticSessionControl request. */
bool              fbl_port_tool_contact(void);

/* ---- App image access (ADR-0008 D3) ---- */

/* Base of the app image in (memory-mapped) flash; a fake buffer on host. */
const uint8_t    *fbl_port_app_image_base(void);

/* Size of the app flash region (bounds the digest range). */
uint32_t          fbl_port_app_region_len(void);

/* ---- Terminal actions (ADR-0008 D4) ---- */

/* De-init before the jump (ADR-0008 D4 step 3, B3 contract): disable IRQs, stop
 * SysTick and clear its pending bit, disable + clear-pending all NVIC IRQs,
 * return FBL-touched peripherals toward reset state. Ordinary C; host no-op. */
void              fbl_port_deinit_for_jump(void);

/* Perform the context handover: set VTOR/MSP and branch to the app. Target is a
 * naked asm helper with nothing stack-dependent between MSP-set and the branch;
 * never returns. Host fake records the call. */
void              fbl_port_jump_to_app(uint32_t msp, uint32_t reset);

/* Enter programming mode (M1: minimal wait; M3: UDS services). */
void              fbl_port_enter_programming_mode(void);

/* System reset. */
void              fbl_port_system_reset(void);

#endif /* FBL_PORT_H */
