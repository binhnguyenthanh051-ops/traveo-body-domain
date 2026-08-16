/*
 * log_types.h -- pure types for the target logging channel, no functions.
 *
 * Shared by the producer/drain core (log.c), the port implementations, and --
 * via the generated table -- the host decoder. Design: ADR-0023.
 *
 * The record is defined as EXPLICIT BYTE OFFSETS, not as a struct overlaid on
 * a buffer (ADR-0023 D2). Same discipline as crypto_msg.c / shared/secoc: no
 * packing pragmas, no padding assumptions, no implementation-defined bitfield
 * layout, and the layout reads identically on host and target.
 */
#ifndef LOG_TYPES_H
#define LOG_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------
 * Wire record (ADR-0023 D2)
 *
 *   off  size  field
 *    0    1    sync      LOG_SYNC (0xA5)
 *    1    1    core_seq  (core_id << 6) | (seq & 0x3F)
 *    2    2    evt       u16 LE
 *    4    4    ts_ms     u32 LE
 *    8    4    arg0      u32 LE
 *   12    2    arg1      u16 LE
 *   14    2    crc16     CRC-16/CCITT-FALSE over bytes 0..13
 *
 * 16 bytes on purpose: a power-of-two record in a power-of-two ring turns
 * every index computation into a mask rather than a division.
 * ----------------------------------------------------------------- */
#define LOG_REC_SIZE        16U

/* 0xA5 is >= 0x80, i.e. OUTSIDE ASCII. That is what lets raw text and binary
 * records share one UART with no escaping and no mode switch (ADR-0023 D3):
 * the decoder passes any non-record byte run through verbatim. Do not change
 * this to an ASCII-range value. */
#define LOG_SYNC            0xA5U

#define LOG_OFF_SYNC        0U
#define LOG_OFF_CORE_SEQ    1U
#define LOG_OFF_EVT         2U
#define LOG_OFF_TS_MS       4U
#define LOG_OFF_ARG0        8U
#define LOG_OFF_ARG1        12U
#define LOG_OFF_CRC         14U

/* core_seq packing. seq is 6 bits (wraps at 64) and exists so a consumer can
 * detect DROPPED records independently of LOG_EVT_OVERFLOW -- see ADR-0023 D5.
 * Without it, "event absent" and "event lost" are indistinguishable, which is
 * exactly the ambiguity the SecOC rejection tests must not have. */
#define LOG_CORE_SHIFT      6U
#define LOG_SEQ_MASK        0x3FU
#define LOG_CORE_MASK       0x03U

/* Which core produced a record -- a WIRE field, not a ring selector.
 *
 * Named by ROLE, not by part, so one decoder table serves both nodes.
 *
 * Only LOG_CORE_APP produces today. The security core (CM0+) deliberately does
 * NOT log: per ADR-0021 D1 the MAC truncation and the constant-time compare
 * happen in shared/secoc on the APP core, so the M0+ is a generic CMAC oracle
 * that never learns whether a frame was accepted or rejected -- no contract
 * event originates there. A shared-RAM ring out of the core that holds the AES
 * secret (REQ-SECOC-011) would be TCB surface across the security boundary with
 * no contract evidence in return, so it is not built (ADR-0023 D4).
 *
 * LOG_CORE_SECURITY stays defined because it costs two bits already reserved in
 * the record and the decoder already renders it: if the decision is ever
 * revisited, no wire-format change is needed. */
typedef enum {
    LOG_CORE_APP      = 0,   /* CM4 (Node A) / CM7 (Node B) -- owns the UART */
    LOG_CORE_SECURITY = 1    /* CM0+ -- reserved, no producer */
} log_core_t;

/* -------------------------------------------------------------------
 * Event-ID classes (ADR-0023 D6)
 *
 * The split is what makes "may a test assert on this?" a MECHANICAL question
 * with one right answer, instead of a judgement call:
 *
 *   CONTRACT   (< LOG_EVT_DIAG_BASE) -- part of the tested interface. BVT and
 *              regression tests MAY assert on these. Changing or removing one
 *              is a breaking change and must update the tests in the same
 *              commit.
 *   DIAGNOSTIC (>= LOG_EVT_DIAG_BASE) -- developer telemetry. NO test may
 *              assert on these, so they stay free to add/reword/delete.
 *   TEXT       -- ASCII runs (ADR-0023 D3). Human-only, never assertable.
 * ----------------------------------------------------------------- */
#define LOG_EVT_DIAG_BASE   0x1000U

/* Module prefixes inside the contract range (high byte). */
#define LOG_EVT_MOD_BOOT    0x0000U
#define LOG_EVT_MOD_SECOC   0x0100U
#define LOG_EVT_MOD_CRYPTO  0x0200U
#define LOG_EVT_MOD_DIAG    0x0300U
#define LOG_EVT_MOD_APP     0x0400U

/* Name this IMAGE puts in its banner. Overridden per image via -D; the FBL and
 * the app are two CM4 binaries that both log as LOG_CORE_APP (the core role is
 * the same), so without this a capture spanning the jump is ambiguous about
 * which image emitted what -- which is exactly when you are reading it. */
#ifndef LOG_IMAGE_NAME
#define LOG_IMAGE_NAME      "app"
#endif

/* Build identity, ideally the git short SHA passed in as -DLOG_BUILD_ID=\"...\".
 * M5 bring-up finding #2: the BVT reflashes between tests, so a log that cannot
 * name its own image makes "which build produced this trace?" unanswerable. */
#ifndef LOG_BUILD_ID
#define LOG_BUILD_ID        "nobuildid"
#endif

/* Consecutive no-progress iterations after which log_flush() gives up,
 * INDEPENDENTLY of its timeout.
 *
 * The timeout alone is not a bound, because it assumes the clock runs. It may
 * not: the FBL calls fbl_port_deinit_for_jump() -- which stops SysTick -- and a
 * flush after that point would see a frozen log_port_now_ms() and spin forever,
 * meaning the logger bricks the very boot path it exists to observe. This cap
 * makes termination independent of the time base. */
#define LOG_FLUSH_MAX_STALL 1000U

/* Longest ASCII run log_text() will enqueue; anything beyond is truncated.
 *
 * This is a CONCURRENCY bound, not a style preference. The producer holds the
 * interrupt mask across reserve+copy+publish (ADR-0023 D7), so the longest
 * single enqueue sets the worst-case masked duration. 64 bytes keeps that in
 * the sub-microsecond range on both nodes. Raising it lengthens the window in
 * which an ISR cannot run. */
#define LOG_TEXT_MAX        64U

/* -------------------------------------------------------------------
 * Published drain latency (REQ-LOG-014, ADR-0023 D10)
 *
 * Worst case from log_evt() returning to the record leaving the sink. Output
 * is CYCLIC, not immediate -- so a consumer asserting the ABSENCE of an event
 * must wait at least this long before concluding absence. Without a published
 * bound, every absence assertion in the BVT (replay, forgery) is a race
 * against the drain task.
 *
 * The drain task's period must not exceed this. Override per image if a node's
 * scheduling makes the default unachievable -- but publish the new number;
 * the value being KNOWN matters more than it being small.
 * ----------------------------------------------------------------- */
#ifndef LOG_DRAIN_LATENCY_MS
#define LOG_DRAIN_LATENCY_MS  50U
#endif

/* -------------------------------------------------------------------
 * Drain outcome. Reported for observability only -- a caller has nothing
 * useful to do about a busy sink except call again later (ADR-0023 D7).
 * ----------------------------------------------------------------- */
typedef enum {
    LOG_DRAIN_IDLE = 0,   /* ring empty, nothing to do */
    LOG_DRAIN_SENT,       /* bytes handed to the sink */
    LOG_DRAIN_BLOCKED     /* sink not ready; bytes remain queued */
} log_drain_t;

#endif /* LOG_TYPES_H */
