/*
 * fbl.ld — FBL linker script (representative — verify all addresses in the TRM).
 *
 * Implements the overview Sec.6 memory map for the bootloader image:
 *   - FBL occupies the first 64 KB of code flash; its vector table sits at the
 *     flash base (the ROM hands control here after verifying the FBL).
 *   - The app lives above the FBL (app.ld pins the same boundary; see ADR-0008).
 *   - A `.noinit` handshake region is pinned at the TOP of SRAM and is NOT
 *     zeroed by startup, so it survives warm resets (ADR-0007 D1). Its address
 *     MUST match the app's linker script exactly — define it in one shared place.
 *
 * The app/FBL share SRAM for .data/.bss/stack (they never run at once); only
 * `.noinit` must be at a stable, agreed address.
 */

ENTRY(Reset_Handler)

__noinit_size = 256;          /* bytes reserved for the shared handshake (ADR-0007) */

MEMORY
{
  FLASH_FBL (rx)  : ORIGIN = 0x10000000, LENGTH = 64K
  SRAM      (rwx) : ORIGIN = 0x08000000, LENGTH = 128K - 256
  NOINIT    (rw)  : ORIGIN = 0x08000000 + 128K - 256, LENGTH = 256
}

_estack = ORIGIN(SRAM) + LENGTH(SRAM);   /* FBL's own stack top (in shared SRAM) */

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

  /* Shared no-init handshake — pinned, NOLOAD so startup never clears it. */
  .noinit (NOLOAD) :
  {
    KEEP(*(.noinit))
  } > NOINIT
}
