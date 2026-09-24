/*
 * actuator_fsm.h — Node B actuator state (M5 seam 3).
 *
 * Consumes decoded body_msg_t commands and holds the door/light state. Only
 * reached for commands that SecOC already verified (secoc_app_verify_and_decode),
 * so authentication is upstream — this layer just tracks and drives state, and
 * holds its last safe state when no valid command arrives (secoc-architecture §4).
 */
#ifndef ACTUATOR_FSM_H
#define ACTUATOR_FSM_H

#include <stdbool.h>
#include <stdint.h>
#include "body_msgs.h"

/* Safe defaults: door LOCKED, light OFF. */
void actuator_fsm_init(void);

/* Apply a decoded, already-authenticated command. Non-command kinds are ignored
 * (state held). */
void actuator_fsm_apply(const body_msg_t *msg);

bool    actuator_door_locked(void);
uint8_t actuator_light_pct(void);

#endif /* ACTUATOR_FSM_H */
