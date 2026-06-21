/*
 * boot.h — FBL boot core (host-testable decision logic).
 *
 * Hardware-independent (ADR-0001): all hardware access is via fbl_port.h.
 * Implements the reset-cause classification, the .noinit handshake, the
 * BREG boot-loop counter rule, image validity, the knock dwell, and the
 * top-level boot decision. Design: ADR-0007 and ADR-0008.
 */
#ifndef BOOT_H
#define BOOT_H

#include "boot_types.h"
#include <stdbool.h>

/* -------------------------------------------------------------------
 * Reset-cause classification (ADR-0007 D2/D3)
 * ----------------------------------------------------------------- */

/* Whether the .noinit region must be primed or can be read, prime-biased:
 * power-on / hibernate / unknown -> PRIME; software / watchdog / debug -> READ. */
fbl_noinit_action_t fbl_classify_noinit(fbl_reset_cause_t cause);

/* -------------------------------------------------------------------
 * .noinit handshake (ADR-0007 D1)
 * ----------------------------------------------------------------- */

/* CRC over the handshake excluding its own crc32 field. */
uint32_t fbl_handshake_crc(const fbl_handshake_t *h);

/* True iff magic matches and crc32 recomputes. */
bool     fbl_handshake_valid(const fbl_handshake_t *h);

/* Initialise to the safe default (mode = boot-app), set magic + crc32. */
void     fbl_handshake_init_default(fbl_handshake_t *h);

/* -------------------------------------------------------------------
 * BREG boot-loop counter rule (ADR-0007 D4)
 *
 * power-on                              -> clear to 0
 * software reset + programming-request  -> clear to 0  (deliberate reflash)
 * software reset without it             -> increment
 * watchdog reset                        -> increment
 * debug / hibernate                     -> unchanged
 * ----------------------------------------------------------------- */
uint32_t fbl_counter_update(fbl_reset_cause_t cause,
                            bool              programming_requested,
                            uint32_t          current);

/* -------------------------------------------------------------------
 * Image validity (ADR-0008 D3)
 * ----------------------------------------------------------------- */

/* Compute the image digest into out[FBL_DIGEST_SIZE]; returns FBL_DIGEST_SIZE.
 * M1 = CRC32; macro-switchable to SHA-256 for M4. */
size_t   fbl_digest(const uint8_t *data, size_t len, uint8_t *out);

/* Vector-table sanity (ADR-0008 D3, B8): initial MSP within SRAM bounds and
 * 8-byte aligned; reset vector within app flash with the Thumb bit set. */
bool     fbl_vector_sane(uint32_t msp, uint32_t reset);

/* Full image check: header magic, region fits, digest over
 * [base, base+image_len) matches the excluded trailer, and vectors sane. */
bool     fbl_app_image_valid(const uint8_t *base, uint32_t region_len);

/* -------------------------------------------------------------------
 * Knock dwell (ADR-0008 D2)
 * ----------------------------------------------------------------- */

/* Wait up to window_ms for fbl_port_tool_contact(); true if contact occurred. */
bool     fbl_dwell_for_tool(uint32_t window_ms);

/* -------------------------------------------------------------------
 * Top-level boot decision (ADR-0008 D1)
 *
 * Gathers inputs via fbl_port_*, reconciles the .noinit region and the BREG
 * counter (ADR-0007), runs the decision tree, and returns the action. The
 * caller performs the jump (reading vectors + fbl_port_jump_to_app) or enters
 * programming mode.
 * ----------------------------------------------------------------- */
fbl_boot_action_t fbl_run_boot(void);

#endif /* BOOT_H */
