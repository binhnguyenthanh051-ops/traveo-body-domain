#include "body_msgs.h"

size_t pack_light_cmd(const light_cmd_msg_t *in, uint8_t *buf, size_t buf_len)
{
    if (in == NULL || buf == NULL || buf_len < 1u) return 0u;
    if (in->brightness_pct > 100u) return 0u;   /* validate at the boundary */
    buf[0] = in->brightness_pct;
    return 1u;
}

size_t pack_sensor_report(const sensor_report_msg_t *in, uint8_t *buf, size_t buf_len)
{
    if (in == NULL || buf == NULL || buf_len < 3u) return 0u;
    if (in->ambient_raw > 1023u) return 0u;
    if (in->door_ajar > 1u)      return 0u;
    buf[0] = (uint8_t)((in->ambient_raw >> 8) & 0xFFu);  /* big-endian */
    buf[1] = (uint8_t)(in->ambient_raw & 0xFFu);
    buf[2] = in->door_ajar;
    return 3u;
}

int unpack_light_cmd(const uint8_t *buf, size_t len, light_cmd_msg_t *out)
{
    if (buf == NULL || out == NULL || len < 1u) return 0;
    if (buf[0] > 100u) return 0;
    out->brightness_pct = buf[0];
    return 1;
}

int unpack_sensor_report(const uint8_t *buf, size_t len, sensor_report_msg_t *out)
{
    if (buf == NULL || out == NULL || len < 3u) return 0;
    uint16_t ambient = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    if (ambient > 1023u) return 0;
    if (buf[2] > 1u)     return 0;
    out->ambient_raw = ambient;
    out->door_ajar   = buf[2];
    return 1;
}

int body_decode(uint32_t id, const uint8_t *buf, size_t len, body_msg_t *out)
{
    if (out == NULL) return 0;
    out->kind = BODY_MSG_NONE;

    switch (id)
    {
        case MSG_ID_DOOR_CMD:
            if (buf == NULL || len < 1u || buf[0] > 1u) return 0;
            out->u.door_cmd.cmd = (door_cmd_t)buf[0];
            out->kind = BODY_MSG_DOOR_CMD;
            return 1;

        case MSG_ID_LIGHT_CMD:
            if (!unpack_light_cmd(buf, len, &out->u.light_cmd)) return 0;
            out->kind = BODY_MSG_LIGHT_CMD;
            return 1;

        case MSG_ID_SENSOR_RPT:
            if (!unpack_sensor_report(buf, len, &out->u.sensor_report)) return 0;
            out->kind = BODY_MSG_SENSOR_REPORT;
            return 1;

        default:
            return 0;
    }
}
