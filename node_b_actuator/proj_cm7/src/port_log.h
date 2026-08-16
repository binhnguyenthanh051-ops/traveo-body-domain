/*
 * port_log.h — Node B CM7 binding of shared/log to SCB0 / KitProg3 UART
 * (ADR-0023 D8, REQ-LOG-011).
 *
 * Port-singleton shape (docs/coding-standard.md): there is exactly one logger
 * per image, so log_port_* are free functions with one link-time implementation
 * — the fbl_port_* / app_port_* pattern, not an _if_t vtable.
 *
 * The ring is image-private and same-core (ADR-0023 D4), so it needs no MPU
 * non-cacheable step even though the CM7 has an L1 D-cache — unlike the IPC
 * mailbox, which does (port_crypto.h / ADR-0018 D6).
 */
#ifndef PORT_LOG_H
#define PORT_LOG_H

/* Configure SCB0 + P0[0]/P0[1] and bring the sink up, then log_init() the app
 * core. Call once at app start, BEFORE the scheduler: producers may log from
 * that point on, and early records queue until the drain task first runs.
 *
 * Safe to call before vTaskStartScheduler() — it touches no FreeRTOS object,
 * and log_port_now_ms() simply reads 0 until the tick starts.
 *
 * MUST be called AFTER SystemCoreClockUpdate() (main.c): the PDL's frequency
 * bookkeeping is per-core, and this core reads 0 until then — the clock setup
 * would correctly refuse and leave the console silent. */
void log_port_init(void);

/* Create the low-priority drain task. Node B has no tasks.h, so the creator is
 * declared here alongside the port it services. Call before the scheduler. */
void log_task_create(void);

#endif /* PORT_LOG_H */
