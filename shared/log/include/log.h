/*
 * log.h -- target logging channel: producer + drain API (ADR-0023).
 *
 * Structured binary event records over UART, decoded on the host. NOT a printf
 * facility: the target never formats (ADR-0023 D1). Two reasons, in order of
 * weight:
 *
 *   1. Some call sites are hot -- a SecOC verify runs per received frame, and
 *      integer formatting there is jitter in the very path M5 measures.
 *   2. A formatted string is not a stable interface. The BVT bench asserts on
 *      SecOC accept/reject events (docs/briefs/BVT-bench-brief.md), and prose
 *      gets reworded while an event ID is a contract (ADR-0023 D6).
 *
 * No <stdio.h>, no varargs, no format strings -- so no MISRA deviation is
 * needed to have a console (ADR-0003).
 *
 * Layering (ADR-0001): this file and log.c are pure logic, host-tested with
 * plain GCC. UART, clock, interrupt masking and the ring storage itself live
 * behind log_port.h, which has one link-time implementation per image and a
 * fake in tests/.
 *
 * Requirements: REQ-LOG-001..014 (docs/requirements/logging.md).
 */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "log_types.h"

/* Initialise the calling core's ring and sequence state. Call once, early,
 * before the first log_evt() on that core. On the UART-owning core this also
 * emits the ASCII banner (see log_banner). */
void log_init(log_core_t core);

/* Emit one event record. THE call site everywhere in the firmware.
 *
 * Safe from task or ISR context, on any core. Bounded and NON-BLOCKING: one
 * short interrupt-masked region covering reserve+copy+publish as an
 * indivisible step -- never the UART (ADR-0023 D7). Splitting that region
 * lets an ISR publish over a task's unwritten reservation; see the comment on
 * log_try_enqueue() in log.c.
 *
 * Returns void deliberately. A full ring, a dead sink and an uninitialised
 * port are all non-errors at the call site: there is nothing a caller could
 * sensibly do, and offering a status would invite error paths on a diagnostic
 * facility. Loss is never silent -- it is counted and announced (D5), which is
 * the consumer's problem to notice, not the producer's to handle.
 *
 * evt   -- event ID from log_events.h (generated; ADR-0023 D9).
 * arg0  -- event-specific u32, meaning fixed per event by events.csv.
 * arg1  -- event-specific u16, same.
 *
 * One function, not a family of arities: the record always carries both
 * fields, and MISRA discourages the function-like macros a "convenience"
 * overload would need. Pass 0 for an unused argument.
 */
void log_evt(uint16_t evt, uint32_t arg0, uint16_t arg1);

/* Emit a run of raw ASCII, passed through to the host verbatim (ADR-0023 D3).
 *
 * Legal because LOG_SYNC is outside ASCII, so text and records share the
 * stream unescaped. For humans only: NO TEST MAY ASSERT ON TEXT (D6). Reach
 * for log_evt() whenever the thing being reported might ever be checked.
 *
 * Bytes with the high bit set are rejected (they would forge a sync); s must
 * be NUL-terminated. Same non-blocking, may-drop contract as log_evt().
 */
void log_text(const char *s);

/* Fixed ASCII banner identifying image + build. Emitted by log_init() on the
 * UART-owning core.
 *
 * Its job is bring-up: it proves TX works in a plain terminal (PuTTY) before
 * the host decoder exists -- which is exactly when you need evidence and have
 * no tooling. Keep it ASCII and keep it short.
 */
void log_banner(void);

/* Drain queued bytes to the sink. Called by a LOW-PRIORITY task on the
 * UART-owning core only; it walks every core's ring (ADR-0023 D4).
 *
 * If this task never runs, producers degrade to dropping -- nothing stalls.
 * max_bytes bounds one call so draining cannot monopolise the core; pass 0 for
 * "as much as the sink accepts".
 */
log_drain_t log_drain(size_t max_bytes);

/* Drain until every ring is empty, or timeout_ms elapses. Returns true if the
 * rings emptied.
 *
 * The one legitimate use of a bounded WAIT on the log path: at a hand-off where
 * the sink is about to be taken away or the core is about to stop, cyclic
 * draining has no next opportunity. The FBL emits LOG_EVT_APP_JUMP and then
 * jumps -- the app re-inits SCB0 and the queued record is gone -- and an app
 * about to ECUReset has the same problem. Without a flush those records are
 * lost exactly when they matter most, which for the secure-boot BVT test is the
 * entire evidence.
 *
 * NOT a general-purpose "make sure this gets out" call. Using it on a healthy
 * path reintroduces the blocking that D7 forbids; the drain task is what moves
 * bytes in normal operation. Bounded by log_port_now_ms(), so a dead sink costs
 * timeout_ms once, not forever.
 */
bool log_flush(uint32_t timeout_ms);

/* Records dropped since boot (ADR-0023 D5).
 *
 * The BVT MUST treat a non-zero delta across a test window as a FAILED run,
 * not a passed one: several tests assert on the ABSENCE of an event, and a
 * lossy window cannot distinguish "never emitted" from "emitted and lost". A
 * suite that reports green from a window it knows was lossy is worse than one
 * that reports an error.
 */
uint32_t log_dropped(void);

/* Fault-handler path: bypasses the ring and writes the record with a blocking,
 * polled, interrupt-free UART write (ADR-0023 D7).
 *
 * The ring is worthless from a fault handler -- no task will ever drain it --
 * so this is the ONE place logging is permitted to spin. Legitimate precisely
 * because nothing else is going to run anyway. Do not call it from normal code
 * to "make sure the message gets out"; that reintroduces blocking on healthy
 * paths, which D7 exists to forbid.
 */
void log_panic(uint16_t evt, uint32_t arg0, uint16_t arg1);

/* -------------------------------------------------------------------
 * Encoding helpers -- exposed so the host decoder round-trip test can drive
 * the SAME encoder the target uses (ADR-0023 D8). A format drift between the
 * C encoder and the Python decoder then fails a host unit test rather than a
 * bench run at midnight; a drifted decoder produces confidently WRONG test
 * results, which is the worst failure mode a merge gate can have.
 * ----------------------------------------------------------------- */

/* Encode one record into out[LOG_REC_SIZE]. Pure; no ring, no port. */
void log_rec_encode(uint8_t *out, log_core_t core, uint8_t seq,
                    uint16_t evt, uint32_t ts_ms, uint32_t arg0, uint16_t arg1);

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR)
 * over len bytes. Guards RESYNC, not the wire: a decoder attaching mid-stream
 * is guaranteed to land mid-record, so it locks onto a candidate LOG_SYNC,
 * validates the CRC, then steps LOG_REC_SIZE at a time (ADR-0023 D2). */
uint16_t log_crc16(const uint8_t *data, size_t len);

#endif /* LOG_H */
