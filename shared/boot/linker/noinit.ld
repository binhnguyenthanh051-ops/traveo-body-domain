/*
 * noinit.ld — single source of truth for the shared .noinit handshake region
 * (ADR-0007 D8, M2-3).
 *
 * INCLUDEd by BOTH the FBL linker (node_a_gateway/bootloader/linker/fbl.ld) and
 * the app linker so the two images can never disagree on the address — a
 * mismatch makes the App<->FBL handshake read garbage. The build must add this
 * directory to the linker search path, e.g. `-L shared/boot/linker`, so the
 * INCLUDE resolves.
 *
 * Placement (SRAM is 128 KB @ 0x0800_0000, shared CM0+/CM4):
 *   0x0800_0000  CM0+ RAM tenant (32 KB)      -- both images AVOID
 *   0x0800_8000  CM4 RAM (.data/.bss/stack)
 *   0x0801_F700  .noinit handshake (256 B)    -- HERE: above the tenant,
 *   0x0801_F800  reserved "system use" (2 KB)    below the BSP-reserved 2 KB
 *   0x0802_0000  top of SRAM
 *
 * Keep _noinit_start in sync with any MEMORY literals that bound it; both
 * linkers ASSERT that their CM4 RAM ends exactly at _noinit_start.
 */

_noinit_start = 0x0801F700;
_noinit_size  = 0x100;        /* 256 B; handshake is 16 B, rest is headroom */
