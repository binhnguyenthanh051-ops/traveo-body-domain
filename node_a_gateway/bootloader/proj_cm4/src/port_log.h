/*
 * port_log.h — FBL log seam (ADR-0023, REQ-LOG-009/015).
 *
 * The FBL is the second image to bind shared/log, and the one the BVT needs
 * most: LOG_EVT_APP_JUMP / LOG_EVT_APP_REJECT are the only positive evidence
 * that secure boot made a decision at all. Without them, "the FBL refused to
 * jump" is inferred from silence — indistinguishable from a dead board.
 *
 * Super-loop, so there is no drain task: see fbl_log_service() and, before the
 * jump, log_flush().
 */
#ifndef PORT_LOG_H
#define PORT_LOG_H

#include <stdint.h>

/* Bytes moved per fbl_log_service() call. Bounds how long one service call can
 * sit inside the programming-mode loop, where ISO-TP has its own N_Cr timing to
 * keep (ADR-0013 D3) and must not be starved by logging. */
#define FBL_LOG_SERVICE_MAX_BYTES   128U

/* Budget for the pre-jump flush. Generous relative to the few records the FBL
 * emits (~5 records = 80 B ≈ 0.8 ms at 1 Mbps), and bounded twice over:
 * log_flush() also stops after LOG_FLUSH_MAX_STALL no-progress iterations, so a
 * frozen SysTick cannot hang the handover. */
#define FBL_LOG_FLUSH_MS            20U

/* Configure SCB0 + P0[0]/P0[1], bring the sink up, emit the banner. Call after
 * fbl_time_init() — the banner is timestamped, and log_flush() later needs a
 * running clock to honour its timeout. */
void fbl_log_init(void);

/* Move queued bytes to the UART. Call from the programming-mode loop; the FBL
 * has no drain task. Non-blocking and bounded by FBL_LOG_SERVICE_MAX_BYTES. */
void fbl_log_service(void);

#endif /* PORT_LOG_H */
