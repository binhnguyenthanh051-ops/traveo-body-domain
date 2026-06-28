/*
 * fbl.ld — FBL linker script: the M2-3 .noinit design reference.
 *
 * NOTE: this is NOT yet the FBL's build linker. The bootloader Makefile has
 * LINKER_SCRIPT empty, so the FBL is currently built with the BSP default
 * linker. Wiring the FBL's real linker to this .noinit pin (fork the BSP CM4
 * linker as the app did for app_cm4.ld, set LINKER_SCRIPT + add
 * `-L shared/boot/linker`) is an on-board bring-up task: it changes the FBL
 * image, so M1 must be re-verified (ADR-0007 D8). Addresses are representative —
 * verify in the TRM.
 *
 * Implements the overview Sec.6 memory map for the bootloader image:
 *   - FBL occupies the first 64 KB of code flash; its vector table sits at the
 *     flash base (the ROM hands control here after verifying the FBL).
 *   - The app lives above the FBL (app.ld pins the same boundary; see ADR-0008).
 *   - The `.noinit` handshake region is pinned at a FIXED address shared with the
 *     app via shared/boot/linker/noinit.ld, and is NOT zeroed by startup so it
 *     survives warm resets (ADR-0007 D1/D8). A mismatch would make the handshake
 *     read garbage.
 *   - SRAM is shared CM0+/CM4 on this part. The FBL's own .data/.bss/stack live
 *     in the CM4 RAM region ABOVE the CM0+ tenant (0x0800_0000..0x0800_8000) —
 *     M2-3 corrected this from the earlier 0x0800_0000 origin, which overlapped
 *     the CM0+ tenant. The app/FBL share this CM4 RAM (they never run at once);
 *     only `.noinit` must be at a stable, agreed address.
 */

ENTRY(Reset_Handler)

/* Single source of truth for the shared .noinit region: _noinit_start/_size.
 * Requires the build to add `-L shared/boot/linker` to the linker flags. */
INCLUDE noinit.ld

MEMORY
{
  FLASH_FBL (rx)  : ORIGIN = 0x10000000, LENGTH = 64K
  /* CM4 RAM: above the CM0+ tenant, ending exactly at the pinned .noinit base.
   * Keep LENGTH in sync with noinit.ld (_noinit_start); the ASSERT below guards
   * it. 0x0800_8000 + 0x1_7700 = 0x0801_F700 = _noinit_start. */
  SRAM      (rwx) : ORIGIN = 0x08008000, LENGTH = 0x17700
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);   /* FBL stack top, just below .noinit */

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH_FBL   /* vector table at flash base */

  .text :
  {
    *(.text*)
    *(.rodata*)
  } > FLASH_FBL

  .data :
  {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > SRAM AT > FLASH_FBL
  _sidata = LOADADDR(.data);

  .bss :
  {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > SRAM

  /* Shared no-init handshake — pinned at the absolute shared address, NOLOAD so
   * startup never clears it (ADR-0007 D1/D8). */
  .noinit _noinit_start (NOLOAD) :
  {
    KEEP(*(.noinit))
  }
}

/* Cross-image guards (ADR-0007 D8): CM4 RAM must end exactly where .noinit
 * begins, and .noinit must land at the shared address. */
ASSERT(_estack == _noinit_start,
       "FBL SRAM must end exactly at .noinit base — sync fbl.ld with noinit.ld")
ASSERT(ADDR(.noinit) == _noinit_start,
       "FBL .noinit not pinned at _noinit_start (noinit.ld)")
