/*
 * port_log.h — Node A APP binding of shared/log to SCB0 / KitProg3 UART
 * (ADR-0023 D8, REQ-LOG-011).
 *
 * Port-singleton shape (docs/coding-standard.md): there is exactly one logger
 * per image, so log_port_* are free functions with one link-time implementation
 * — the fbl_port_* / app_port_* pattern, not an _if_t vtable.
 *
 * The M4 has no cache, so — unlike Node B's CM7 — the rings need no MPU
 * non-cacheable step (cf. port_crypto.h).
 */
#ifndef PORT_LOG_H
#define PORT_LOG_H

/* Configure SCB0 + P0[0]/P0[1] and bring the sink up, then log_init() the app
 * core. Call once at app start, BEFORE the scheduler: producers may log from
 * that point on, and early records queue until the drain task first runs.
 *
 * Safe to call before vTaskStartScheduler() — it touches no FreeRTOS object,
 * and log_port_now_ms() simply reads 0 until the tick starts. */
void log_port_init(void);

#endif /* PORT_LOG_H */
