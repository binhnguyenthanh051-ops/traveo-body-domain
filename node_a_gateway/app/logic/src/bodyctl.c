/*
 * bodyctl.c — Node A body-control state machine (ADR-0010 D6).
 *
 * Pure logic, host-testable. No FreeRTOS, no vendor headers — see bodyctl.h.
 */
#include "bodyctl.h"

void bodyctl_init(bodyctl_state_t *st)
{
    st->door_locked = false;
    st->light_pct   = 0U;
    st->door_ajar   = false;
}

void bodyctl_step(bodyctl_state_t *st, const body_msg_t *in, bodyctl_output_t *out)
{
    /* Every call fully writes out; commands default to "no command this step". */
    out->lock_cmd_valid  = false;
    out->lock_locked     = false;
    out->light_cmd_valid = false;
    out->light_pct       = 0U;

    if (in == NULL)
    {
        return;
    }

    switch (in->kind)
    {
        case BODY_MSG_DOOR_CMD:
            st->door_locked  = (in->u.door_cmd.cmd == DOOR_LOCK);
            out->lock_cmd_valid = true;
            out->lock_locked    = st->door_locked;
            break;

        case BODY_MSG_LIGHT_CMD:
            st->light_pct       = in->u.light_cmd.brightness_pct;
            out->light_cmd_valid = true;
            out->light_pct       = st->light_pct;
            break;

        case BODY_MSG_SENSOR_REPORT:
            st->door_ajar = (in->u.sensor_report.door_ajar != 0U);
            /* Courtesy-light rule: a door left ajar turns the light fully on;
             * closing it returns the light to off. Minimal, but a real reaction
             * the host test can pin. */
            if (st->door_ajar)
            {
                st->light_pct        = 100U;
                out->light_cmd_valid = true;
                out->light_pct       = st->light_pct;
            }
            else
            {
                st->light_pct        = 0U;
                out->light_cmd_valid = true;
                out->light_pct       = st->light_pct;
            }
            break;

        case BODY_MSG_NONE:
        default:
            /* Unknown / empty — no state change, no command. */
            break;
    }
}
