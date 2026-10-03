/*
 * secoc.c — secured-frame layer (ADR-0021 D1/D2/D9).
 *
 * Builds and verifies one CAN FD frame [ PDU | freshness(4) | trunc-MAC(8) ],
 * with the Data ID bound into the MAC input (not transmitted). The MAC oracle
 * (M0+ AES-CMAC) is injected; truncation to 8 bytes and the constant-time
 * compare live here, on the app side. A failed verify never yields a PDU
 * (drop-before-decode).
 *
 * Host-testable: no vendor/RTOS/CAN symbols. MISRA C:2012 (guard clauses
 * deviate from 15.5 deliberately, matching shared/crypto).
 *
 * @impl ADR-0021 D1 : AES-CMAC (via oracle), 8-byte truncation, constant-time compare
 * @impl ADR-0021 D2 : frame layout + DataID-in-MAC (cross-ID substitution fix)
 * @impl ADR-0021 D9 : verify failure => drop-before-decode (no PDU emitted)
 * @impl REQ-SECOC-001 : authenticate or drop
 * @impl REQ-SECOC-002 : truncated MAC, constant-time compare
 * @impl REQ-SECOC-003 : secured frame layout, little-endian freshness
 * @impl REQ-SECOC-004 : Data ID bound into the MAC input
 * @impl REQ-SECOC-013 : telemetry rides the identical path
 */
#include "secoc.h"

/* Largest authentic PDU the frame layer handles (config; body PDUs are tiny and
 * a CAN FD data field is 64 B, of which 12 B is the SecOC trailer). */
#ifndef SECOC_MAX_PDU
#define SECOC_MAX_PDU   64U
#endif

#define SECOC_MAC_INPUT_MAX  (SECOC_DATAID_LEN + SECOC_FRESHNESS_LEN + SECOC_MAX_PDU)

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* @impl ADR-0021 D1 : constant-time compare (no data-dependent early-out) */
bool secoc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    if ((n != 0U) && ((a == NULL) || (b == NULL)))
    {
        return false;
    }
    uint8_t diff = 0U;
    for (size_t i = 0U; i < n; ++i)
    {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (diff == 0U);
}

/* Assemble MAC input = DataID(2 LE) | freshness(4) | pdu into buf. */
static size_t build_mac_input(uint16_t can_id, const uint8_t *freshness4,
                              const uint8_t *pdu, size_t pdu_len, uint8_t *buf)
{
    put_u16le(&buf[0], can_id);
    buf[2] = freshness4[0];
    buf[3] = freshness4[1];
    buf[4] = freshness4[2];
    buf[5] = freshness4[3];
    for (size_t i = 0U; i < pdu_len; ++i)
    {
        buf[(size_t)6U + i] = pdu[i];
    }
    return (size_t)6U + pdu_len;
}

size_t secoc_secure(const secoc_crypto_t *cy, uint16_t can_id,
                    uint16_t epoch, uint16_t counter,
                    const uint8_t *pdu, size_t pdu_len,
                    uint8_t *out, size_t cap)
{
    if ((cy == NULL) || (cy->oracle == NULL) || (cy->oracle->mac == NULL) ||
        (out == NULL) || ((pdu == NULL) && (pdu_len != 0U)))
    {
        return 0U;
    }
    if (pdu_len > (size_t)SECOC_MAX_PDU)
    {
        return 0U;
    }

    size_t secured = pdu_len + (size_t)SECOC_TRAILER_LEN;
    if (cap < secured)
    {
        return 0U;
    }

    uint8_t fresh[SECOC_FRESHNESS_LEN];
    put_u16le(&fresh[0], epoch);
    put_u16le(&fresh[2], counter);

    uint8_t macin[SECOC_MAC_INPUT_MAX];
    size_t macin_len = build_mac_input(can_id, fresh, pdu, pdu_len, macin);

    uint8_t tag[SECOC_FULL_TAG_LEN];
    if (!cy->oracle->mac(cy->key_id, macin, macin_len, tag))
    {
        return 0U;                      /* oracle failure => produce nothing */
    }

    for (size_t i = 0U; i < pdu_len; ++i)
    {
        out[i] = pdu[i];
    }
    out[pdu_len + 0U] = fresh[0];
    out[pdu_len + 1U] = fresh[1];
    out[pdu_len + 2U] = fresh[2];
    out[pdu_len + 3U] = fresh[3];
    for (size_t i = 0U; i < (size_t)SECOC_MAC_LEN; ++i)
    {
        out[pdu_len + (size_t)SECOC_FRESHNESS_LEN + i] = tag[i];
    }
    return secured;
}

/* Shared by both entry points: verify the MAC over a frame whose authentic PDU
 * is the first `plen` bytes, then strip. Length policy is the CALLER's — it
 * differs between "infer" and "configured" — so this core never decides it. */
static secoc_verify_result_t verify_core(const secoc_crypto_t *cy, uint16_t can_id,
                                         const uint8_t *frame, size_t plen,
                                         uint16_t *epoch, uint16_t *counter,
                                         uint8_t *pdu_out, size_t *pdu_len)
{
    const uint8_t *pdu    = &frame[0];
    const uint8_t *fresh  = &frame[plen];
    const uint8_t *rx_mac = &frame[plen + (size_t)SECOC_FRESHNESS_LEN];

    uint8_t macin[SECOC_MAC_INPUT_MAX];
    size_t macin_len = build_mac_input(can_id, fresh, pdu, plen, macin);

    uint8_t tag[SECOC_FULL_TAG_LEN];
    if (!cy->oracle->mac(cy->key_id, macin, macin_len, tag))
    {
        return SECOC_MAC_ERROR;
    }
    if (!secoc_ct_equal(tag, rx_mac, (size_t)SECOC_MAC_LEN))
    {
        return SECOC_BAD_MAC;           /* drop before decode */
    }

    for (size_t i = 0U; i < plen; ++i)
    {
        pdu_out[i] = pdu[i];
    }
    *pdu_len = plen;
    *epoch = get_u16le(&fresh[0]);
    *counter = get_u16le(&fresh[2]);
    return SECOC_OK;
}

static bool verify_args_ok(const secoc_crypto_t *cy, const uint8_t *frame,
                           const uint16_t *epoch, const uint16_t *counter,
                           const uint8_t *pdu_out, const size_t *pdu_len)
{
    return (cy != NULL) && (cy->oracle != NULL) && (cy->oracle->mac != NULL) &&
           (frame != NULL) && (epoch != NULL) && (counter != NULL) &&
           (pdu_out != NULL) && (pdu_len != NULL);
}

secoc_verify_result_t secoc_verify(const secoc_crypto_t *cy, uint16_t can_id,
                                   const uint8_t *frame, size_t frame_len,
                                   uint16_t *epoch, uint16_t *counter,
                                   uint8_t *pdu_out, size_t cap, size_t *pdu_len)
{
    if (!verify_args_ok(cy, frame, epoch, counter, pdu_out, pdu_len))
    {
        return SECOC_MAC_ERROR;         /* misuse => fail-safe, no PDU emitted */
    }
    if (frame_len < (size_t)SECOC_TRAILER_LEN)
    {
        return SECOC_BAD_LENGTH;
    }

    size_t plen = frame_len - (size_t)SECOC_TRAILER_LEN;
    if ((plen > (size_t)SECOC_MAX_PDU) || (plen > cap))
    {
        return SECOC_BAD_LENGTH;
    }
    return verify_core(cy, can_id, frame, plen, epoch, counter, pdu_out, pdu_len);
}

size_t secoc_fd_frame_len(size_t n)
{
    /* CAN FD data-field lengths: 0..8, then 12, 16, 20, 24, 32, 48, 64. */
    static const size_t k_fd_len[] = { 12U, 16U, 20U, 24U, 32U, 48U, 64U };
    if (n <= 8U)
    {
        return n;
    }
    for (size_t i = 0U; i < (sizeof k_fd_len / sizeof k_fd_len[0]); ++i)
    {
        if (n <= k_fd_len[i])
        {
            return k_fd_len[i];
        }
    }
    return 0U;                          /* does not fit one CAN FD frame */
}

secoc_verify_result_t secoc_verify_len(const secoc_crypto_t *cy, uint16_t can_id,
                                       const uint8_t *frame, size_t frame_len,
                                       size_t pdu_len_cfg,
                                       uint16_t *epoch, uint16_t *counter,
                                       uint8_t *pdu_out, size_t cap, size_t *pdu_len)
{
    if (!verify_args_ok(cy, frame, epoch, counter, pdu_out, pdu_len))
    {
        return SECOC_MAC_ERROR;         /* misuse => fail-safe, no PDU emitted */
    }
    if ((pdu_len_cfg > (size_t)SECOC_MAX_PDU) || (pdu_len_cfg > cap))
    {
        return SECOC_BAD_LENGTH;
    }

    /* Accept exactly the secured length, or that length padded up to the next
     * CAN FD length — what a real sender's controller puts on the wire. Shorter
     * cannot hold the trailer; longer is not a frame this ID's sender produces. */
    size_t secured = pdu_len_cfg + (size_t)SECOC_TRAILER_LEN;
    size_t padded  = secoc_fd_frame_len(secured);
    if ((frame_len < secured) || (frame_len > padded))
    {
        return SECOC_BAD_LENGTH;
    }
    return verify_core(cy, can_id, frame, pdu_len_cfg, epoch, counter, pdu_out, pdu_len);
}
