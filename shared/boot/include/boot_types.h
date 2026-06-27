/*
 * boot_types.h — types and compile-time configuration for the FBL boot core.
 *
 * Pure types, no functions, no vendor headers. Shared between the bootloader
 * and (for the handshake/header definitions) the application. Design rationale:
 * ADR-0007 (boot-state persistence) and ADR-0008 (boot decision + jump).
 */
#ifndef BOOT_TYPES_H
#define BOOT_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------
 * Compile-time configuration — override via -D in the build.
 * ------------------------------------------------------------------ */

/* Boot-loop fallback threshold N: counter > N => stay in FBL (ADR-0007 D4). */
#ifndef FBL_BOOTLOOP_THRESHOLD
#define FBL_BOOTLOOP_THRESHOLD   3U
#endif

/* CAN knock window, milliseconds (ADR-0008 D2). */
#ifndef FBL_KNOCK_WINDOW_MS
#define FBL_KNOCK_WINDOW_MS      2000U
#endif

/* Offset of the descriptive app header past the app vector table (ADR-0008 D3).
 * Actual value tracks the app's vector-table size on target — verify in TRM. */
#ifndef FBL_APP_HEADER_OFFSET
#define FBL_APP_HEADER_OFFSET    0x100U
#endif

/* Validity magics. */
#define FBL_HANDSHAKE_MAGIC      0xB007C0DEU   /* .noinit shared region */
#define FBL_BACKUP_MAGIC         0xB00710ADU   /* BREG FBL-private block */
#define FBL_APP_HEADER_MAGIC     0xA9900D01U   /* app image descriptive header */

/* Backup-domain register map (ADR-0007 D6). FBL block is [0..1]; the App
 * partition is reserved from FBL_BREG_APP_BASE_IDX and is not used in M1. */
#define FBL_BREG_MAGIC_IDX       0U            /* FBL_BACKUP_MAGIC when valid */
#define FBL_BREG_COUNTER_IDX     1U            /* boot-loop counter */
#define FBL_BREG_APP_BASE_IDX    2U            /* reserved: App wake-context */

/* Memory map for vector sanity. Confirmed against the BSP linker (dual-core):
 * code flash 0x1000_0000 holds the CM0+ prebuilt (first 128 KB), then the CM4
 * FBL at 0x1002_0000; the CM4 app lives above the FBL at 0x1004_0000.
 * (overview Sec.6.) */
#define FBL_SRAM_BASE            0x08000000U
#define FBL_SRAM_SIZE            (128U * 1024U)
#define FBL_APP_FLASH_BASE       0x10040000U      /* CM4 app vector table */
#define FBL_APP_FLASH_SIZE       (832U * 1024U)   /* 0x1004_0000 .. 0x1011_0000 */

/* --------------------------------------------------------------------
 * Image digest selection (ADR-0008 D3) — M1: CRC32; M4: flip to SHA-256.
 * ------------------------------------------------------------------ */

#define FBL_DIGEST_CRC32   1
#define FBL_DIGEST_SHA256  2

#ifndef FBL_DIGEST_ALGO
#define FBL_DIGEST_ALGO    FBL_DIGEST_CRC32
#endif

#if   FBL_DIGEST_ALGO == FBL_DIGEST_CRC32
#define FBL_DIGEST_SIZE    4U
#elif FBL_DIGEST_ALGO == FBL_DIGEST_SHA256
#define FBL_DIGEST_SIZE    32U
#else
#error "FBL_DIGEST_ALGO must be FBL_DIGEST_CRC32 or FBL_DIGEST_SHA256"
#endif

/* --------------------------------------------------------------------
 * Reset cause (raw, from the port) and its classification
 * ------------------------------------------------------------------ */

typedef enum {
    FBL_RST_POWER_ON = 0,   /* POR/BOD — or cause-register == 0 */
    FBL_RST_SOFTWARE,       /* SYSRESETREQ */
    FBL_RST_WATCHDOG,
    FBL_RST_DEBUG,
    FBL_RST_HIBERNATE,      /* wake from hibernate */
    FBL_RST_OTHER           /* unknown / multiple — treated as cold */
} fbl_reset_cause_t;

/* Whether the .noinit region must be primed (lost/untrusted) or can be read
 * (retained). Prime-biased: ambiguous causes prime (ADR-0007 D3). */
typedef enum {
    FBL_NOINIT_READ = 0,    /* retained + ECC-valid — read and validate */
    FBL_NOINIT_PRIME        /* lost/undefined — write to set ECC, init default */
} fbl_noinit_action_t;

/* --------------------------------------------------------------------
 * Device lifecycle (ADR-0008 D2) — exact stage values verify in TRM.
 * Knock window opens only when lifecycle < FBL_LC_SECURE.
 * ------------------------------------------------------------------ */

typedef enum {
    FBL_LC_NORMAL = 0,
    FBL_LC_TRANSITION_TO_SECURE,
    FBL_LC_SECURE,
    FBL_LC_RMA
} fbl_lifecycle_t;

/* --------------------------------------------------------------------
 * The .noinit shared App<->FBL handshake (ADR-0007 D1)
 * ------------------------------------------------------------------ */

typedef enum {
    FBL_BOOT_APP = 0,
    FBL_PROGRAMMING_REQUESTED = 1
} fbl_boot_mode_t;

typedef struct {
    uint32_t magic;        /* FBL_HANDSHAKE_MAGIC when initialised */
    uint16_t version;
    uint8_t  mode;         /* fbl_boot_mode_t */
    uint8_t  reserved;
    uint32_t crc32;        /* over the preceding bytes */
} fbl_handshake_t;

/* --------------------------------------------------------------------
 * Descriptive app image header (ADR-0008 D3). Lives at
 * app_base + FBL_APP_HEADER_OFFSET; the integrity trailer (digest / hash +
 * signature) lives at app_base + image_len and is NOT part of the header.
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;        /* FBL_APP_HEADER_MAGIC */
    uint16_t hdr_version;
    uint16_t hdr_size;
    uint32_t image_len;    /* digest covers [app_base, app_base + image_len) */
} fbl_app_header_t;

/* --------------------------------------------------------------------
 * Final boot decision (ADR-0008 D1)
 * ------------------------------------------------------------------ */

typedef enum {
    FBL_ACTION_JUMP_APP = 0,
    FBL_ACTION_PROGRAMMING_MODE
} fbl_boot_action_t;

#endif /* BOOT_TYPES_H */
