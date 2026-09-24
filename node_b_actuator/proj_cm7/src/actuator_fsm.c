/*
 * actuator_fsm.c — Node B actuator state (M5 seam 3). See header.
 *
 * The "actuation" here updates state and is where a real build would drive the
 * lock motor / light PWM GPIO — kept as state + a hook comment so the security
 * path is the focus, not the board wiring. door_ajar telemetry reflects the
 * door state.
 */
#include "actuator_fsm.h"

static bool    s_door_locked;
static uint8_t s_light_pct;

void actuator_fsm_init(void)
{
    s_door_locked = true;   /* safe default: locked */
    s_light_pct = 0u;       /* safe default: off */
    /* TODO(bring-up): drive lock/light outputs to the safe state here. */
}

void actuator_fsm_apply(const body_msg_t *msg)
{
    if (msg == NULL)
    {
        return;
    }
    switch (msg->kind)
    {
        case BODY_MSG_DOOR_CMD:
            s_door_locked = (msg->u.door_cmd.cmd == DOOR_LOCK);
            /* TODO(bring-up): drive the lock actuator GPIO. */
            break;

        case BODY_MSG_LIGHT_CMD:
            s_light_pct = msg->u.light_cmd.brightness_pct;
            /* TODO(bring-up): set the light PWM duty. */
            break;

        default:
            /* Unknown/none: hold last safe state. */
            break;
    }
}

bool actuator_door_locked(void)
{
    return s_door_locked;
}

uint8_t actuator_light_pct(void)
{
    return s_light_pct;
}
