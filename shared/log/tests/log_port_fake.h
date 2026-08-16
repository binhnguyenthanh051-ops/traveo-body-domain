/*
 * log_port_fake.h -- test hooks for the scripted log port (ADR-0023 D8).
 *
 * Host-side stand-in for the UART, the clock, the interrupt mask and the
 * shared-RAM rings. Test harness -- MISRA does not apply here
 * (docs/coding-standard.md).
 */
#ifndef LOG_PORT_FAKE_H
#define LOG_PORT_FAKE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "log_types.h"

/* Small on purpose: overflow (REQ-LOG-006) must be reachable in a few writes
 * rather than a thousand. Power of two, as the port contract requires. */
#define FAKE_RING_CAP   64U

/* Zero everything: rings, head/tail, sink capture, clock, lock accounting. */
void log_fake_reset(void);

/* Forget captured sink bytes ONLY -- leaves rings, clock and counters alone.
 * Lets a test discard the boot banner without disturbing the state under
 * test. */
void log_fake_reset_sink_only(void);

/* Scripted clock -- log_port_now_ms() returns exactly this. */
void log_fake_set_now(uint32_t ms);

/* Auto-advance the clock by `step` ms on every log_port_now_ms() call.
 * Default 0 = frozen, which is what most tests want (a frozen clock is how you
 * prove ts_ms is sampled at production time). Set non-zero to exercise a
 * bounded-wait loop's TIMEOUT path -- the same trick ipc_port_fake.c uses. */
void log_fake_set_now_step(uint32_t step);

/* Sink admission control. n = bytes log_port_tx() will accept per call;
 * SIZE_MAX means "accept everything". 0 models a busy UART. */
void log_fake_set_tx_accept(size_t n);

/* Sink readiness (log_port_sink_ready()). Defaults to true after reset. */
void log_fake_set_sink_ready(bool ready);

/* Sink transmit-complete (log_port_sink_idle()). Defaults to true after reset.
 * Set false to model bytes still shifting out of the TX FIFO -- the state that
 * loses a record across a jump or reset if log_flush() ignores it. */
void log_fake_set_sink_idle(bool idle);

/* Bytes the sink has received, in order, via the non-blocking path. */
const uint8_t *log_fake_sink(void);
size_t         log_fake_sink_len(void);

/* Bytes written via the BLOCKING path -- log_panic() only (REQ-LOG-010).
 * Kept separate so a test can prove the panic path bypassed the ring. */
const uint8_t *log_fake_panic_sink(void);
size_t         log_fake_panic_sink_len(void);

/* Critical-section accounting (REQ-LOG-005). balanced() catches a missing
 * unlock; max_depth catches unintended nesting; the byte counter proves the
 * masked region stayed short -- a copy performed inside the lock shows up
 * here. */
bool     log_fake_lock_balanced(void);
uint32_t log_fake_lock_max_depth(void);
uint32_t log_fake_lock_enter_count(void);

/* Bytes currently queued in a core's ring (head - tail, mod capacity). */
size_t log_fake_ring_used(log_core_t core);

/* Publish barrier calls -- ordering must be explicit, not assumed. */
uint32_t log_fake_barrier_count(void);

#endif /* LOG_PORT_FAKE_H */
