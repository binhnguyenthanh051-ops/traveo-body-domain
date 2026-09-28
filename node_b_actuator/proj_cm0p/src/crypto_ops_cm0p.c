/*
 * crypto_ops_cm0p.c (Node B) — CM0+ crypto back end for the Body High Lite part
 * (CYT4BF, CAT1C). Node B's M0+ serves exactly ONE op: CRYPTO_OP_MAC (AES-CMAC).
 * There is no HASH and no VERIFY_IMAGE here — Node B has no app secure boot in
 * M5 (secoc-architecture §4/§8.2), so the only reason this core exists at
 * runtime is to hold the SecOC secret and compute MACs behind the offload.
 *
 * The target-only back end behind the host-tested dispatch (shared/crypto/
 * crypto_dispatch.c). Mirrors Node A's crypto_ops_cm0p.c but trimmed to the MAC
 * row. The MXCRYPTO block IS present on CYT4BF8CDS (CY_IP_MXCRYPTO in
 * cyt4bf8cds.h; the PDL compiles cy_crypto_core_cmac_v2) — the earlier bsp.mk
 * "no MXCRYPTO" note (see proj_cm0p/Makefile) was a false alarm, resolved in
 * ADR-0021 D1 / secoc-architecture §7.2. mbedTLS mbedtls_cipher_cmac stays the
 * documented software fallback but is not expected.
 *
 * @impl ADR-0021 D7 : CRYPTO_OP_MAC handler (Node B server side)
 */
#include "crypto_dispatch.h"
#include "crypto_keystore.h"
#include "crypto_msg.h"
#include "crypto_types.h"
#include "secoc_shared_secret.h"  /* secoc_shared_secret[16], SECOC_MAC_KEY_ID (ADR-0021 D6) */
#include "cy_pdl.h"               /* CRYPTO base, Cy_Crypto_Core_* */
#include <string.h>

/* -------------------------------------------------------------------
 * CRYPTO_OP_MAC — AES-CMAC over opaque bytes (ADR-0021 D7)
 * ----------------------------------------------------------------- */
static bool cmac_compute(const uint8_t *key, const uint8_t *msg, uint16_t len,
                         uint8_t tag[CRYPTO_CMAC_TAG_LEN])
{
    /* Vetted PDL CMAC (ADR-0006 — no hand-rolled subkey/padding). One-shot form;
     * aes_state is the required scratch workspace. Byte-oriented, so no
     * endianness reversal of key/message/tag. VERIFY at bring-up (ADR-0021 D1):
     * exact prototype / explicit-init need is mtb-pdl-cat1-version-specific, and
     * correctness is proven against the NIST SP 800-38B KAT on THIS silicon (the
     * same tag must come out on Node A and Node B — they share the key). */
    cy_stc_crypto_aes_state_t aes_state;
    (void)memset(&aes_state, 0, sizeof aes_state);

    cy_en_crypto_status_t st = Cy_Crypto_Core_Cmac(CRYPTO, msg, (uint32_t)len, key,
                                                   CY_CRYPTO_KEY_AES_128,
                                                   tag, &aes_state);
    return (st == CY_CRYPTO_SUCCESS);
}

static bool mac_handler(const crypto_msg_t *req, crypto_msg_t *resp)
{
    uint32_t       key_id  = 0U;
    const uint8_t *msg     = NULL;
    uint16_t       msg_len = 0U;

    if (!crypto_parse_mac_request(req, &key_id, &msg, &msg_len))
    {
        crypto_make_verdict(CRYPTO_OP_MAC, CRYPTO_VERDICT_ERROR, resp);
        return true;
    }

    const crypto_key_entry_t *k = crypto_keystore_lookup(key_id);
    if ((k == NULL) || (k->type != CRYPTO_KEY_AES_SECRET) ||
        (k->secret == NULL) || (k->secret_len != SECOC_AES128_KEY_BYTES))
    {
        crypto_make_verdict(CRYPTO_OP_MAC, CRYPTO_VERDICT_ERROR, resp);
        return true;
    }

    uint8_t tag[CRYPTO_CMAC_TAG_LEN];
    if (!cmac_compute(k->secret, msg, msg_len, tag))
    {
        crypto_make_verdict(CRYPTO_OP_MAC, CRYPTO_VERDICT_ERROR, resp);
        return true;
    }

    crypto_make_mac_response(tag, resp);
    return true;   /* always answers (a tag, or a verdict) */
}

/* -------------------------------------------------------------------
 * Table + init — MAC only (no HASH / VERIFY_IMAGE on Node B)
 * ----------------------------------------------------------------- */
static const crypto_handler_if_t g_handlers[] = {
    { CRYPTO_OP_MAC, mac_handler }
};

/* One-row keystore: the shared AES-CMAC secret. Unknown key_id => NULL => fail
 * (never key 0). No ECDSA key — Node B verifies no images.
 * @impl ADR-0021 D6 : shared AES secret held only in the M0+ image
 * @impl REQ-SECOC-011 : app never holds key bytes; secret resides in this image */
static const crypto_key_entry_t g_keys[] = {
    { .key_id = SECOC_MAC_KEY_ID, .type = CRYPTO_KEY_AES_SECRET,
      .secret = secoc_shared_secret, .secret_len = SECOC_AES128_KEY_BYTES }
};

/* Called once at the crypto seam by main_cm0p.c (after it starts CM7_0), before
 * the mailbox server loop begins. Enables the Crypto block, binds the keystore
 * and the dispatch table. */
void cm0p_crypto_service_init(void)
{
    (void)Cy_Crypto_Core_Enable(CRYPTO);
    crypto_keystore_init(g_keys, sizeof g_keys / sizeof g_keys[0]);
    crypto_dispatch_init(g_handlers, sizeof g_handlers / sizeof g_handlers[0]);
}

#if defined(CRYPTO_BRINGUP_KAT) && (CRYPTO_BRINGUP_KAT != 0)
/* -------------------------------------------------------------------
 * M5 bench Stage 2 + 4.1: AES-CMAC known-answer test (bring-up only)
 *
 * Compiled only with DEFINES=CRYPTO_BRINGUP_KAT=1. Runs once at boot, before
 * the other core is released, and parks everything in globals for the
 * debugger — the CM0+ has no log sink. Pass: g_kat_result == KAT_PASS_ALL (7).
 * The tags are kept even on a pass, so a mismatch can be read as "stable
 * and input-sensitive" (the vector is suspect) vs "random" (wiring is wrong).
 *
 * Vectors: NIST SP 800-38B App. D.1 (AES-128), cross-checked on the host
 * against pyca/cryptography CMAC on 2026-09-27.
 * g_kat_shared_tag is Stage 4.1: the same input MACed with the SecOC shared
 * secret. It must be byte-identical on Node A and Node B.
 * ----------------------------------------------------------------- */
#define KAT_PASS_EMPTY  (0x1U)
#define KAT_PASS_16B    (0x2U)
#define KAT_SHARED_OK   (0x4U)
#define KAT_PASS_ALL    (KAT_PASS_EMPTY | KAT_PASS_16B | KAT_SHARED_OK)

volatile uint32_t g_kat_result = 0U;   /* 0 = not run; KAT_PASS_ALL = pass */
volatile uint8_t  g_kat_tag_empty[CRYPTO_CMAC_TAG_LEN];
volatile uint8_t  g_kat_tag_16b[CRYPTO_CMAC_TAG_LEN];
volatile uint8_t  g_kat_shared_tag[CRYPTO_CMAC_TAG_LEN];

static void kat_park(volatile uint8_t *dst, const uint8_t *src)
{
    for (uint32_t i = 0U; i < CRYPTO_CMAC_TAG_LEN; i++)
    {
        dst[i] = src[i];
    }
}

void cm0p_cmac_kat(void)
{
    static const uint8_t key[16] = {
        0x2bU, 0x7eU, 0x15U, 0x16U, 0x28U, 0xaeU, 0xd2U, 0xa6U,
        0xabU, 0xf7U, 0x15U, 0x88U, 0x09U, 0xcfU, 0x4fU, 0x3cU };
    static const uint8_t msg16[16] = {
        0x6bU, 0xc1U, 0xbeU, 0xe2U, 0x2eU, 0x40U, 0x9fU, 0x96U,
        0xe9U, 0x3dU, 0x7eU, 0x11U, 0x73U, 0x93U, 0x17U, 0x2aU };
    static const uint8_t exp_empty[CRYPTO_CMAC_TAG_LEN] = {
        0xbbU, 0x1dU, 0x69U, 0x29U, 0xe9U, 0x59U, 0x37U, 0x28U,
        0x7fU, 0xa3U, 0x7dU, 0x12U, 0x9bU, 0x75U, 0x67U, 0x46U };
    static const uint8_t exp_16b[CRYPTO_CMAC_TAG_LEN] = {
        0x07U, 0x0aU, 0x16U, 0xb4U, 0x6bU, 0x4dU, 0x41U, 0x44U,
        0xf7U, 0x9bU, 0xddU, 0x9dU, 0xd0U, 0x4aU, 0x28U, 0x7cU };
    uint8_t  tag[CRYPTO_CMAC_TAG_LEN];
    uint32_t result = 0U;

    /* Empty message: pass a valid pointer anyway, length 0. */
    if (cmac_compute(key, msg16, 0U, tag))
    {
        kat_park(g_kat_tag_empty, tag);
        if (memcmp(tag, exp_empty, sizeof tag) == 0)
        {
            result |= KAT_PASS_EMPTY;
        }
    }
    if (cmac_compute(key, msg16, (uint16_t)sizeof msg16, tag))
    {
        kat_park(g_kat_tag_16b, tag);
        if (memcmp(tag, exp_16b, sizeof tag) == 0)
        {
            result |= KAT_PASS_16B;
        }
    }
    /* Stage 4.1: no expected value here (the secret stays in this image);
     * the check is A == B, compared by hand in the debugger. */
    if (cmac_compute(secoc_shared_secret, msg16, (uint16_t)sizeof msg16, tag))
    {
        kat_park(g_kat_shared_tag, tag);
        result |= KAT_SHARED_OK;
    }
    g_kat_result = result;
}
#endif /* CRYPTO_BRINGUP_KAT */
