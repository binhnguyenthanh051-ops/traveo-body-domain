/*
 * boot.c — FBL boot core (host-testable decision logic).
 *
 * Hardware-independent (ADR-0001): all hardware access is via fbl_port.h, no
 * vendor headers. Implements ADR-0007 (reset-cause classification, .noinit
 * handshake, BREG boot-loop counter) and ADR-0008 (image validity, knock
 * dwell, the boot-decision tree).
 */
#include "boot.h"
#include "fbl_port.h"
#include <string.h>

/* --------------------------------------------------------------------
 * CRC-32 (IEEE, reflected poly 0xEDB88320) — used for both the handshake
 * integrity field and the M1 image digest.
 * ------------------------------------------------------------------ */
static uint32_t boot_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < len; ++i)
    {
        crc ^= (uint32_t)data[i];
        for (uint32_t b = 0U; b < 8U; ++b)
        {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

/* --------------------------------------------------------------------
 * Reset-cause classification (ADR-0007 D2/D3) — prime-biased.
 * ------------------------------------------------------------------ */
fbl_noinit_action_t fbl_classify_noinit(fbl_reset_cause_t cause)
{
    fbl_noinit_action_t action;
    switch (cause)
    {
        case FBL_RST_SOFTWARE:
        case FBL_RST_WATCHDOG:
        case FBL_RST_DEBUG:
            action = FBL_NOINIT_READ;            /* retained + ECC-valid */
            break;
        case FBL_RST_POWER_ON:
        case FBL_RST_HIBERNATE:
        case FBL_RST_OTHER:
        default:
            action = FBL_NOINIT_PRIME;           /* lost/untrusted → prime */
            break;
    }
    return action;
}

/* --------------------------------------------------------------------
 * .noinit handshake (ADR-0007 D1)
 * ------------------------------------------------------------------ */
uint32_t fbl_handshake_crc(const fbl_handshake_t *h)
{
    uint8_t buf[offsetof(fbl_handshake_t, crc32)];   /* all fields but crc32 */
    (void)memcpy(buf, h, sizeof buf);
    return boot_crc32(buf, sizeof buf);
}

bool fbl_handshake_valid(const fbl_handshake_t *h)
{
    if (h->magic != FBL_HANDSHAKE_MAGIC)
    {
        return false;
    }
    return (h->crc32 == fbl_handshake_crc(h));
}

void fbl_handshake_init_default(fbl_handshake_t *h)
{
    h->magic    = FBL_HANDSHAKE_MAGIC;
    h->version  = 1U;
    h->mode     = (uint8_t)FBL_BOOT_APP;
    h->reserved = 0U;
    h->crc32    = fbl_handshake_crc(h);
}

/* --------------------------------------------------------------------
 * BREG boot-loop counter rule (ADR-0007 D4)
 * ------------------------------------------------------------------ */
uint32_t fbl_counter_update(fbl_reset_cause_t cause,
                            bool              programming_requested,
                            uint32_t          current)
{
    uint32_t result;
    switch (cause)
    {
        case FBL_RST_POWER_ON:
        case FBL_RST_OTHER:                       /* treat as power-on path */
            result = 0U;
            break;
        case FBL_RST_SOFTWARE:
            /* a deliberate reflash is clean intent, not a crash */
            result = programming_requested ? 0U : (current + 1U);
            break;
        case FBL_RST_WATCHDOG:
            result = current + 1U;
            break;
        case FBL_RST_DEBUG:
        case FBL_RST_HIBERNATE:
        default:
            result = current;                     /* unchanged */
            break;
    }
    return result;
}

/* --------------------------------------------------------------------
 * Image digest + validity (ADR-0008 D3)
 * ------------------------------------------------------------------ */
size_t fbl_digest(const uint8_t *data, size_t len, uint8_t *out)
{
#if FBL_DIGEST_ALGO == FBL_DIGEST_CRC32
    uint32_t crc = boot_crc32(data, len);
    out[0] = (uint8_t)(crc & 0xFFU);
    out[1] = (uint8_t)((crc >> 8) & 0xFFU);
    out[2] = (uint8_t)((crc >> 16) & 0xFFU);
    out[3] = (uint8_t)((crc >> 24) & 0xFFU);
#else
    /* M4: SHA-256 via shared/crypto — same covered range and verify flow. */
    (void)data;
    (void)len;
    (void)memset(out, 0, FBL_DIGEST_SIZE);
#endif
    return FBL_DIGEST_SIZE;
}

bool fbl_vector_sane(uint32_t msp, uint32_t reset)
{
    bool msp_ok = (msp > FBL_SRAM_BASE) &&
                  (msp <= (FBL_SRAM_BASE + FBL_SRAM_SIZE)) &&
                  ((msp & 0x7U) == 0U);
    bool reset_ok = (reset >= FBL_APP_FLASH_BASE) &&
                    (reset < (FBL_APP_FLASH_BASE + FBL_APP_FLASH_SIZE)) &&
                    ((reset & 0x1U) == 1U);
    return msp_ok && reset_ok;
}

bool fbl_app_image_valid(const uint8_t *base, uint32_t region_len)
{
    if (base == NULL)
    {
        return false;
    }

    /* Region must hold header + at least the integrity trailer. */
    uint32_t min_len = FBL_APP_HEADER_OFFSET +
                       (uint32_t)sizeof(fbl_app_header_t) + FBL_DIGEST_SIZE;
    if (region_len < min_len)
    {
        return false;
    }

    fbl_app_header_t hdr;
    (void)memcpy(&hdr, &base[FBL_APP_HEADER_OFFSET], sizeof hdr);
    if (hdr.magic != FBL_APP_HEADER_MAGIC)
    {
        return false;
    }

    /* The covered body must contain the header and leave room for the trailer. */
    uint32_t image_len = hdr.image_len;
    if (image_len < (FBL_APP_HEADER_OFFSET + (uint32_t)sizeof(fbl_app_header_t)))
    {
        return false;
    }
    if (image_len > (region_len - FBL_DIGEST_SIZE))
    {
        return false;
    }

    /* Digest over [base, base+image_len) vs the excluded trailer (B4). */
    uint8_t digest[FBL_DIGEST_SIZE];
    (void)fbl_digest(base, image_len, digest);
    if (memcmp(digest, &base[image_len], FBL_DIGEST_SIZE) != 0)
    {
        return false;
    }

    /* Vector-table sanity (B8). */
    uint32_t msp;
    uint32_t reset;
    (void)memcpy(&msp,   &base[0], sizeof msp);
    (void)memcpy(&reset, &base[4], sizeof reset);
    return fbl_vector_sane(msp, reset);
}

/* --------------------------------------------------------------------
 * Knock dwell (ADR-0008 D2)
 * ------------------------------------------------------------------ */
bool fbl_dwell_for_tool(uint32_t window_ms)
{
    uint32_t start = fbl_port_now_ms();
    bool contacted = false;
    bool expired = false;
    while ((!contacted) && (!expired))
    {
        contacted = fbl_port_tool_contact();
        expired = ((fbl_port_now_ms() - start) >= window_ms);
    }
    return contacted;
}

/* --------------------------------------------------------------------
 * Boot-decision tree (ADR-0008 D1)
 * ------------------------------------------------------------------ */
static bool fbl_knock_allowed(fbl_lifecycle_t lc)
{
    /* Only pre-SECURE stages open the dev backdoor (ADR-0008 D2). */
    return (lc == FBL_LC_NORMAL) || (lc == FBL_LC_TRANSITION_TO_SECURE);
}

fbl_boot_action_t fbl_run_boot(void)
{
    fbl_reset_cause_t cause = fbl_port_reset_cause();
    fbl_port_clear_reset_cause();

    /* --- reconcile the .noinit handshake (ADR-0007 D3/D5) --- */
    fbl_handshake_t *hs = fbl_port_noinit();
    if (fbl_classify_noinit(cause) == FBL_NOINIT_PRIME)
    {
        fbl_port_prime_noinit();
        fbl_handshake_init_default(hs);
    }
    else if (!fbl_handshake_valid(hs))
    {
        fbl_handshake_init_default(hs);          /* corrupt on read-cause → default */
    }
    else
    {
        /* valid — use as-is */
    }
    bool programming_requested =
        (hs->mode == (uint8_t)FBL_PROGRAMMING_REQUESTED);

    /* --- BREG boot-loop counter, single-sourced here (ADR-0007 D4) --- */
    uint32_t counter = 0U;
    if (fbl_port_backup_read(FBL_BREG_MAGIC_IDX) == FBL_BACKUP_MAGIC)
    {
        counter = fbl_port_backup_read(FBL_BREG_COUNTER_IDX);
    }
    counter = fbl_counter_update(cause, programming_requested, counter);
    fbl_port_backup_write(FBL_BREG_MAGIC_IDX, FBL_BACKUP_MAGIC);
    fbl_port_backup_write(FBL_BREG_COUNTER_IDX, counter);

    /* --- decision tree (ADR-0008 D1) --- */
    fbl_boot_action_t action = FBL_ACTION_PROGRAMMING_MODE;   /* safe default */

    if (programming_requested)                               /* 1 */
    {
        action = FBL_ACTION_PROGRAMMING_MODE;
    }
    else if (counter > FBL_BOOTLOOP_THRESHOLD)               /* 2: boot-loop fallback */
    {
        action = FBL_ACTION_PROGRAMMING_MODE;
    }
    else
    {
        /* 3: knock window — only on a power-on reset below SECURE. The dwell is
         * kept out of the && above so it is not a side effect in a condition. */
        bool knock = false;
        if ((cause == FBL_RST_POWER_ON) && fbl_knock_allowed(fbl_port_lifecycle()))
        {
            knock = fbl_dwell_for_tool(FBL_KNOCK_WINDOW_MS);
        }

        if (knock)
        {
            action = FBL_ACTION_PROGRAMMING_MODE;
        }
        else if (fbl_app_image_valid(fbl_port_app_image_base(),
                                     fbl_port_app_region_len()))   /* 4 */
        {
            action = FBL_ACTION_JUMP_APP;
        }
        else                                                 /* 5: fail-safe */
        {
            action = FBL_ACTION_PROGRAMMING_MODE;
        }
    }

    return action;
}
