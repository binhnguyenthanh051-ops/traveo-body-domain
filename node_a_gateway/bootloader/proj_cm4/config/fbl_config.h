/*
 * fbl_config.h — bootloader-variant configuration (ADR-0004: config data, not
 * #ifdef). Force-included before the shared/boot headers (build flag
 * `-include fbl_config.h`) so these values override the `#ifndef` defaults in
 * boot_types.h for the FBL image.
 */
#ifndef FBL_CONFIG_H
#define FBL_CONFIG_H

/* Image integrity for M1 is CRC32; M4 switches this one line to SHA-256. */
#define FBL_DIGEST_ALGO         FBL_DIGEST_SHA256

/* Boot policy tunables (ADR-0008). */
#define FBL_KNOCK_WINDOW_MS     2000U      /* dev cold-boot knock window */
#define FBL_BOOTLOOP_THRESHOLD  3U         /* counter > N => stay in FBL */

/* FBL-only (not used by the portable core): the diagnostic CAN ID the knock
 * frame (and later, real UDS requests) arrives on; port_can.c polls for it.
 * Updated for M3 to the Node A diagnostic request ID reserved in ADR-0002's
 * M3 extension (0x700-0x7FF band) -- was a placeholder 0x7E0 under M1, when
 * no diagnostic band existed yet. */
#define FBL_KNOCK_CAN_ID        0x7A0U

/* M3 Seam 2: the Node A diagnostic response ID (ADR-0002 M3 extension) --
 * every frame isotp.c transmits (SF/FF/CF/FC) goes out under this ID. */
#define FBL_DIAG_RESPONSE_ID    0x7A8U

#endif /* FBL_CONFIG_H */
