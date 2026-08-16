/*
 * log_port.h -- the target seam under shared/log (ADR-0023 D8).
 *
 * Port-singleton free functions, not an _if_t vtable: there is exactly one
 * logger per image, so this is the fbl_port_* / app_port_* shape from
 * docs/coding-standard.md. One link-time implementation per image; tests link
 * log_port_fake.c instead.
 *
 * Everything hardware-shaped lives here so log.c stays host-testable and free
 * of vendor headers (ADR-0001).
 */
#ifndef LOG_PORT_H
#define LOG_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "log_types.h"

/* -------------------------------------------------------------------
 * Ring storage
 *
 * Target-specific because WHERE it lives is the whole point: the rings sit in
 * the same MPU NON-CACHEABLE shared-RAM region as the IPC mailbox (ADR-0018
 * D6, reused by ADR-0023 D4). On Node B the CM7's L1 D-cache makes a naively
 * placed shared buffer silently incoherent -- that problem is already solved
 * for the mailbox, and the log rings inherit the solution rather than
 * rediscovering the bug.
 *
 * NOTE: the rings do NOT go through the IPC mailbox itself. The mailbox is
 * synchronous and single-outstanding (ADR-0018 D3); routing log traffic
 * through it would serialise logging behind crypto RPCs and couple logging
 * latency into SecOC verify latency. Same memory region, different structure.
 *
 * One ring per PRODUCING core, single-producer/single-consumer, byte-oriented
 * (so an ASCII run and a 16-byte record can share it). Capacity MUST be a
 * power of two -- log.c masks rather than divides.
 */
uint8_t *log_port_ring(log_core_t core);
size_t   log_port_ring_cap(log_core_t core);   /* power of two */

/* Head/tail cells, also in the shared non-cacheable region. Split out so the
 * fake can expose them to tests and so the target implementation controls
 * their placement and alignment. Producer owns head, consumer owns tail. */
volatile uint32_t *log_port_head(log_core_t core);
volatile uint32_t *log_port_tail(log_core_t core);

/* Publish/observe barrier around the head/tail handoff. On target these are
 * DMB; on host they are no-ops. Explicit rather than implied so the ordering
 * requirement is visible at the call site instead of buried in a comment. */
void log_port_publish_barrier(void);

/* -------------------------------------------------------------------
 * Producer-side critical section
 *
 * Guards ONLY the byte reservation -- a handful of instructions -- never the
 * copy and never the UART (ADR-0023 D7). It exists because a task and an ISR
 * on the SAME core can both call log_evt(); cross-core needs no lock at all,
 * since each core has its own ring.
 *
 * On target: raise BASEPRI to mask interrupts at or below the configured
 * priority, and restore the PREVIOUS value -- hence the save/restore token
 * rather than a bare enable/disable pair, which would wrongly re-enable
 * interrupts when nested inside an existing critical section.
 * ----------------------------------------------------------------- */
uint32_t log_port_lock(void);            /* returns the previous mask state */
void     log_port_unlock(uint32_t state);

/* Monotonic milliseconds on the CALLING core.
 *
 * Today each core stamps from its own SysTick, which is a PER-CORE peripheral
 * -- so cross-core ordering carries uncorrected skew (ADR-0023 D4). Good
 * enough to read a boot sequence; NOT a basis for cross-core timing claims.
 *
 * The fix is decided but not yet implemented: a dedicated TCPWM 32-bit counter
 * in continuous timer mode at 1 MHz, read by both cores over the peripheral
 * bus (ADR-0023, "Cross-core time base"). When it lands, this function's
 * CONTRACT is unchanged -- only the source moves -- so no call site changes.
 * Both cores will need read access, which is a PPU question (ADR-0020). */
uint32_t log_port_now_ms(void);

/* -------------------------------------------------------------------
 * Sink (UART-owning core only)
 * ----------------------------------------------------------------- */

/* Non-blocking write. Returns how many of len bytes the sink accepted (0 is
 * normal and means "busy"); log_drain() leaves the remainder queued. Must
 * never spin. */
size_t log_port_tx(const uint8_t *data, size_t len);

/* Blocking, polled, interrupt-free write -- log_panic() only (ADR-0023 D7).
 * The one place logging may spin, because it runs from a fault handler where
 * no drain task will ever execute. */
void log_port_tx_blocking(const uint8_t *data, size_t len);

/* True once the sink is configured. Producers may run before it is: their
 * records queue, and the boot banner plus early events survive because
 * overflow drops the NEWEST record, not the oldest (ADR-0023 D5). */
bool log_port_sink_ready(void);

/* True when the sink has put every accepted byte ON THE WIRE -- not merely
 * queued it.
 *
 * The distinction is load-bearing at a hand-off. log_port_tx() reports bytes
 * accepted into the TX FIFO, so an empty ring does NOT mean an empty UART: at
 * 1 Mbps a 16-byte record still needs ~160 us to shift out, and a jump or reset
 * arrives far sooner than that. Whoever re-initialises the SCB next (the app's
 * log_port_init after the FBL jumps) resets the FIFO and those bytes are gone.
 *
 * log_flush() therefore waits on THIS, not just on the rings (REQ-LOG-015).
 * Implementations map it to the peripheral's transmit-complete status. */
bool log_port_sink_idle(void);

#endif /* LOG_PORT_H */
