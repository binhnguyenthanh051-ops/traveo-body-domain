/*
 * crypto_msg.c — envelope framing (ADR-0017 D3, layer 2).
 *
 * Wire layout: [0] op_code | [1..2] length (LE) | [3..] payload[length].
 * Decode rejects any inconsistency so a garbled M0+ reply becomes a clean
 * failure the client maps to an ERROR verdict (ADR-0016 D5), never a misparse.
 */
#include "crypto_msg.h"

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void payload_zero_from(crypto_msg_t *m, uint16_t from)
{
    for (uint16_t i = from; i < (uint16_t)CRYPTO_MAX_PAYLOAD; ++i)
    {
        m->payload[i] = 0U;
    }
}

size_t crypto_msg_encode(const crypto_msg_t *m, uint8_t *buf, size_t cap)
{
    if ((m == NULL) || (buf == NULL))
    {
        return 0U;
    }
    if (m->length > (uint16_t)CRYPTO_MAX_PAYLOAD)
    {
        return 0U;
    }

    size_t total = (size_t)CRYPTO_MSG_HDR_SIZE + (size_t)m->length;
    if (cap < total)
    {
        return 0U;
    }

    buf[0] = m->op_code;
    buf[1] = (uint8_t)(m->length & 0xFFU);
    buf[2] = (uint8_t)((m->length >> 8) & 0xFFU);
    for (uint16_t i = 0U; i < m->length; ++i)
    {
        buf[(size_t)CRYPTO_MSG_HDR_SIZE + (size_t)i] = m->payload[i];
    }
    return total;
}

bool crypto_msg_decode(const uint8_t *buf, size_t len, crypto_msg_t *m)
{
    if ((buf == NULL) || (m == NULL))
    {
        return false;
    }
    if (len < (size_t)CRYPTO_MSG_HDR_SIZE)
    {
        return false;
    }

    uint16_t length = (uint16_t)((uint16_t)buf[1] | (uint16_t)((uint16_t)buf[2] << 8));
    if (length > (uint16_t)CRYPTO_MAX_PAYLOAD)
    {
        return false;
    }
    if ((size_t)length > (len - (size_t)CRYPTO_MSG_HDR_SIZE))
    {
        return false;
    }

    m->op_code = buf[0];
    m->length = length;
    for (uint16_t i = 0U; i < length; ++i)
    {
        m->payload[i] = buf[(size_t)CRYPTO_MSG_HDR_SIZE + (size_t)i];
    }
    payload_zero_from(m, length);
    return true;
}

void crypto_make_verify_request(uint32_t base, uint32_t len, uint32_t key_id,
                                crypto_msg_t *m)
{
    if (m == NULL)
    {
        return;
    }
    m->op_code = (uint8_t)CRYPTO_OP_VERIFY_IMAGE;
    m->length = 12U;
    put_u32le(&m->payload[0], base);
    put_u32le(&m->payload[4], len);
    put_u32le(&m->payload[8], key_id);
    payload_zero_from(m, 12U);
}

bool crypto_parse_verify_request(const crypto_msg_t *m,
                                 uint32_t *base, uint32_t *len, uint32_t *key_id)
{
    if ((m == NULL) || (base == NULL) || (len == NULL) || (key_id == NULL))
    {
        return false;
    }
    if (m->op_code != (uint8_t)CRYPTO_OP_VERIFY_IMAGE)
    {
        return false;
    }
    if (m->length < 12U)
    {
        return false;
    }
    *base = get_u32le(&m->payload[0]);
    *len = get_u32le(&m->payload[4]);
    *key_id = get_u32le(&m->payload[8]);
    return true;
}

void crypto_make_verdict(crypto_op_t echo_op, crypto_verdict_t v, crypto_msg_t *m)
{
    if (m == NULL)
    {
        return;
    }
    m->op_code = (uint8_t)echo_op;
    m->length = 1U;
    m->payload[0] = (uint8_t)v;
    payload_zero_from(m, 1U);
}

bool crypto_parse_verdict(const crypto_msg_t *m, crypto_verdict_t *v)
{
    if ((m == NULL) || (v == NULL))
    {
        return false;
    }
    if (m->length < 1U)
    {
        return false;
    }
    uint8_t raw = m->payload[0];
    if (raw > (uint8_t)CRYPTO_VERDICT_ERROR)
    {
        return false;
    }
    *v = (crypto_verdict_t)raw;
    return true;
}
