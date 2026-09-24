/*
 * crypto_ops_cm0p.c — CM0+ crypto op handlers (M4 Seam 2/3, ADR-0017 layer 4).
 *
 * The target-only back end behind the host-tested dispatch (shared/crypto/
 * crypto_dispatch.c): one handler per op, calling the VETTED HW crypto
 * primitive (Cy_Crypto_Core_*, ADR-0006 — we implement no algorithm).
 *   Seam 2: CRYPTO_OP_HASH (SHA-256).
 *   Seam 3: CRYPTO_OP_VERIFY_IMAGE (SHA-256 over a flash range + ECDSA P-256),
 *     the coarse RPC (ADR-0017 D1) — the CM0+ hashes the REAL flash itself, so
 *     the trusted core verifies reality, not a CM4-reported hash.
 *
 * The Crypto block is a CPUSS sub-block on CYT2B7 (CPUSS_CRYPTO_PRESENT=1,
 * CRYPTO_V2), owned by the CM0+ (ADR-0017), enabled once here.
 *
 * Proven on silicon (Seam 3/4): the ECC input byte order (see do_verify_image,
 * S3-2) and the Crypto block hashing directly from memory-mapped flash both work.
 */
#include "crypto_dispatch.h"
#include "crypto_keystore.h"
#include "crypto_msg.h"
#include "crypto_types.h"
#include "crypto_pubkey_dev.h"   /* crypto_pubkey_dev[64], CRYPTO_DEV_KEY_ID */
#include "secoc_shared_secret.h" /* secoc_shared_secret[16], SECOC_MAC_KEY_ID (ADR-0021 D6) */
#include "cy_pdl.h"              /* CRYPTO base, Cy_Crypto_Core_* */
#include <string.h>

/* Trailer geometry past the covered range (ADR-0008 D3 / ADR-0019 D4):
 * hash[32] + signature[64] (P-256 r||s) + key_id[4]. */
#define TRAILER_HASH_OFF   0U
#define TRAILER_SIG_OFF    32U
#define ECC_P256_BYTES     32U

/* -------------------------------------------------------------------
 * CRYPTO_OP_HASH — SHA-256 over the request payload (Seam 2)
 * ----------------------------------------------------------------- */
static bool hash_handler(const crypto_msg_t *req, crypto_msg_t *resp)
{
    uint8_t digest[CY_CRYPTO_SHA256_DIGEST_SIZE];

    if (Cy_Crypto_Core_Sha(CRYPTO, req->payload, (uint32_t)req->length,
                           digest, CY_CRYPTO_MODE_SHA256) != CY_CRYPTO_SUCCESS)
    {
        return false;
    }
    resp->op_code = (uint8_t)CRYPTO_OP_HASH;
    resp->length = (uint16_t)CY_CRYPTO_SHA256_DIGEST_SIZE;
    (void)memcpy(resp->payload, digest, sizeof digest);
    return true;
}

/* Reverse a 32-byte big-endian field into little-endian. The Crypto VU works
 * little-endian (its stored curve constants — Gx/order — are LE), so the point
 * coordinates and the signature scalars must be LE. Bench-confirmed Seam 3
 * (S3-2): passing them big-endian made a known-good signature verify INVALID. */
static void reverse32(uint8_t *dst, const uint8_t *src)
{
    for (size_t i = 0U; i < 32U; ++i)
    {
        dst[i] = src[31U - i];
    }
}

/* -------------------------------------------------------------------
 * CRYPTO_OP_VERIFY_IMAGE — two-stage verify over a flash range (Seam 3)
 * ----------------------------------------------------------------- */
static crypto_verdict_t do_verify_image(uint32_t base, uint32_t len,
                                        const uint8_t *pubkey_xy)
{
    const uint8_t *body    = (const uint8_t *)(uintptr_t)base;
    const uint8_t *trailer = body + len;
    const uint8_t *stored_hash = &trailer[TRAILER_HASH_OFF];   /* [0..32)  */
    const uint8_t *sig         = &trailer[TRAILER_SIG_OFF];    /* [32..96) r||s */

    /* Hash the REAL flash body (ADR-0017 D1). */
    uint8_t digest[CY_CRYPTO_SHA256_DIGEST_SIZE];
    cy_en_crypto_status_t sha_st = Cy_Crypto_Core_Sha(CRYPTO, body, len, digest,
                                                      CY_CRYPTO_MODE_SHA256);
    if (sha_st != CY_CRYPTO_SUCCESS)
    {
        return CRYPTO_VERDICT_ERROR;
    }

    /* Stage 1 (integrity, ADR-0016 D2): the flash body must hash to the stored
     * value — localizes "corrupt/incomplete image" from "wrong signature".
     * (Bench-confirmed working: a tampered body correctly mismatches here.) */
    int m = memcmp(digest, stored_hash, sizeof digest);
    if (m != 0)
    {
        return CRYPTO_VERDICT_INVALID;
    }

    /* Stage 2 (authenticity): ECDSA P-256 verify over the digest.
     *
     * BYTE ORDER (Seam-3 bench finding S3-2): the Crypto VU is little-endian —
     * its stored curve constants (Gx/order) are LE, and it loads the point
     * coordinates + signature scalars as-is. So the public key (X, Y) and the
     * signature (r, s) must be reversed to LE here; the HASH stays big-endian
     * (the driver inverts it internally). Passing them all big-endian made a
     * known-good signature verify INVALID. */
    uint8_t key_x_le[32];
    uint8_t key_y_le[32];
    uint8_t sig_le[64];
    reverse32(key_x_le, &pubkey_xy[0]);              /* X  -> LE */
    reverse32(key_y_le, &pubkey_xy[ECC_P256_BYTES]); /* Y  -> LE */
    reverse32(&sig_le[0], &sig[0]);                  /* r  -> LE */
    reverse32(&sig_le[ECC_P256_BYTES], &sig[ECC_P256_BYTES]); /* s -> LE */

    cy_stc_crypto_ecc_key key;
    (void)memset(&key, 0, sizeof key);
    key.type    = PK_PUBLIC;
    key.curveID = CY_CRYPTO_ECC_ECP_SECP256R1;
    key.pubkey.x = key_x_le;
    key.pubkey.y = key_y_le;
    key.k = NULL;

    uint8_t stat = 0U;
    cy_en_crypto_status_t ecc_st = Cy_Crypto_Core_ECC_VerifyHash(CRYPTO, sig_le, digest,
                                                                (uint32_t)sizeof digest, &stat,
                                                                &key);
    if (ecc_st != CY_CRYPTO_SUCCESS)
    {
        return CRYPTO_VERDICT_ERROR;
    }
    return (stat == 1U) ? CRYPTO_VERDICT_VALID : CRYPTO_VERDICT_INVALID;
}

static bool verify_handler(const crypto_msg_t *req, crypto_msg_t *resp)
{
    uint32_t base = 0U;
    uint32_t len = 0U;
    uint32_t key_id = 0U;
    crypto_verdict_t v = CRYPTO_VERDICT_ERROR;

    if (crypto_parse_verify_request(req, &base, &len, &key_id))
    {
        const crypto_key_entry_t *k = crypto_keystore_lookup(key_id);
        if ((k != NULL) && (k->pubkey != NULL) && (k->pubkey_len == (2U * ECC_P256_BYTES)))
        {
            v = do_verify_image(base, len, k->pubkey);
        }
        else
        {
            /* unknown key_id -> not a valid image (ADR-0019 D3), not an error */
            v = CRYPTO_VERDICT_INVALID;
        }
    }
    crypto_make_verdict(CRYPTO_OP_VERIFY_IMAGE, v, resp);
    return true;   /* always answers with a verdict */
}

/* -------------------------------------------------------------------
 * CRYPTO_OP_MAC — AES-CMAC over opaque bytes (M5 SecOC, ADR-0021 D7)
 *
 * SecOC-agnostic: the M0+ MACs the bytes it is handed (DataID∥freshness∥PDU,
 * assembled by shared/secoc on the app side) and returns the full 16-byte tag.
 * Truncation to 8 B and the constant-time compare live in shared/secoc, not
 * here. An unknown/typemismatched key_id returns an ERROR VERDICT, never a tag,
 * so the client (crypto_mac) drops the frame (REQ-SECOC-010).
 *
 * @impl ADR-0021 D7 : CRYPTO_OP_MAC handler (server side)
 * ----------------------------------------------------------------- */
static bool cmac_compute(const uint8_t *key, const uint8_t *msg, uint16_t len,
                         uint8_t tag[CRYPTO_CMAC_TAG_LEN])
{
    /* Vetted PDL CMAC (ADR-0006 — no hand-rolled subkey/padding). One-shot form;
     * aes_state is the required scratch workspace. AES-CMAC is byte-oriented, so
     * — unlike the ECDSA path (S3-2) — key/message/tag need NO endianness
     * reversal. VERIFY at bring-up (ADR-0021 D1): the exact prototype / whether
     * an explicit Cy_Crypto_Core_Aes_Init is needed is mtb-pdl-cat1-version-
     * specific, and correctness is proven against the NIST SP 800-38B KAT. */
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
        /* unknown / wrong-type key_id -> ERROR verdict, never a tag (REQ-SECOC-010) */
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
 * Table + init
 * ----------------------------------------------------------------- */
static const crypto_handler_if_t g_handlers[] = {
    { CRYPTO_OP_HASH,         hash_handler },
    { CRYPTO_OP_VERIFY_IMAGE, verify_handler },
    { CRYPTO_OP_MAC,          mac_handler }      /* ★ M5 SecOC (ADR-0021 D7) */
};

/* Keystore (ADR-0019 D3): the ECDSA verify key AND the M5 AES-CMAC secret.
 * Unknown key_id => lookup NULL => fail (never key 0).
 * @impl ADR-0021 D6 : shared AES secret held only in the M0+ image
 * @impl REQ-SECOC-011 : app never holds key bytes; secret resides in this image */
static const crypto_key_entry_t g_keys[] = {
    { .key_id = CRYPTO_DEV_KEY_ID, .pubkey = crypto_pubkey_dev,
      .pubkey_len = sizeof crypto_pubkey_dev },  /* .type defaults to ECDSA_PUBLIC */
    { .key_id = SECOC_MAC_KEY_ID, .type = CRYPTO_KEY_AES_SECRET,
      .secret = secoc_shared_secret, .secret_len = SECOC_AES128_KEY_BYTES }
};

void cm0p_crypto_service_init(void)
{
    (void)Cy_Crypto_Core_Enable(CRYPTO);
    crypto_keystore_init(g_keys, sizeof g_keys / sizeof g_keys[0]);
    crypto_dispatch_init(g_handlers, sizeof g_handlers / sizeof g_handlers[0]);
}
