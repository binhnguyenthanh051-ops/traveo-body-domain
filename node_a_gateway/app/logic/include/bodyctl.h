/*
 * bodyctl.h — Node A body-control state machine (ADR-0010 D6, the M2-8 seam).
 *
 * Pure logic: consumes decoded body_msg_t structs over this interface, never a
 * CAN frame and never a FreeRTOS queue handle. It MUST compile on the host with
 * plain GCC (no FreeRTOS, no vendor headers) — that build is what structurally
 * prevents the kernel from leaking into the routing logic (ADR-0001). App_Cyclic
 * Task is the only thing that owns the queue and calls bodyctl_step().
 *
 * M2 scope is deliberately minimal: track lock/light state, and a courtesy-light
 * rule on a door-ajar sensor report. Enough to demonstrate the architecture.
 */
#ifndef BODYCTL_H
#define BODYCTL_H

#include "body_msgs.h"
#include <stdint.h>
#include <stdbool.h>

/* Retained body-control state. */
typedef struct {
    bool    door_locked;
    uint8_t light_pct;      /* 0..100 */
    bool    door_ajar;      /* last reported */
} bodyctl_state_t;

/* Commands produced by a step. Each *_valid flag says whether the paired field
 * carries a fresh command this step (the actuator/CAN layer acts on valid ones).
 * Keeping outputs as data — not direct hardware calls — keeps this host-testable. */
typedef struct {
    bool    lock_cmd_valid;
    bool    lock_locked;
    bool    light_cmd_valid;
    uint8_t light_pct;
} bodyctl_output_t;

/* Reset to a safe default state (unlocked, light off, not ajar). */
void bodyctl_init(bodyctl_state_t *st);

/* Advance the state machine by one decoded message and emit any commands.
 * `out` is fully written each call (all *_valid default to false). A NULL or
 * BODY_MSG_NONE input is a no-op that still clears `out`. */
void bodyctl_step(bodyctl_state_t *st, const body_msg_t *in, bodyctl_output_t *out);

#endif /* BODYCTL_H */
