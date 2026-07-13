/*
 * port_flash.c — the FBL's flash driver, bound to the app-image region
 * (target). M3 Seam 6.
 *
 * Verified against the PDL, corrected twice on the board (each draft caught
 * a wrong assumption the compiler or the silicon then falsified):
 *
 *  1. This part is CY_IP_MXFLASHC_VERSION_ECT (CPUSS_FLASHC_ECT == 1 in
 *     tviibe1m_config.h). The ECT flash API is Cy_Flash_ProgramRow +
 *     Cy_Flash_EraseSector (both blocking, controller resolved from the
 *     address); Cy_Flash_WriteRow/EraseRow do NOT exist for ECT. (The first
 *     draft assumed the non-ECT branch -- the compile error on
 *     Cy_Flash_EraseRow caught it.)
 *
 *  2. PROGRAM granularity is one CY_FLASH_SIZEOF_ROW = CPUSS_FLASHC_PA_SIZE
 *     (128) * 4 = 512-byte "program area" row.
 *
 *  3. ERASE granularity is a whole SECTOR, and code flash is MIXED geometry
 *     (TRM / bench-confirmed): 30 large 32 KB sectors from 0x1000_0000, then
 *     16 small 8 KB sectors up to 0x1011_0000. The small sectors are at the
 *     TOP of code flash -- i.e. the TOP of the app region -- not a
 *     don't-care region at the start as a first draft assumed. So within the
 *     app image region (0x1004_0000 .. 0x1011_0000):
 *        0x1004_0000 .. 0x100F_0000 : 22 large 32 KB sectors
 *        0x100F_0000 .. 0x1011_0000 : 16 small  8 KB sectors
 *
 * Consequences of (3): a single fixed sector_size can't describe this flash.
 * erase_sector() erases the one real hardware sector containing its address
 * (32 KB or 8 KB, per fbl_flash_sector_size()); erase_range() walks real
 * sector boundaries internally so a caller need only align to the coarse
 * (32 KB) granularity reported in hal_flash_if_t.sector_size -- a 32 KB
 * boundary is always also an 8 KB boundary, so it is a valid sector boundary
 * everywhere in the app region.
 *
 * hal_flash_if_t.write() accepts an arbitrary length (ADR-0012 D6); this
 * target implementation only accepts whole, row-aligned writes and chunks
 * internally by the 512-byte program row -- any read-modify-write for a
 * partial row is the download bookkeeping's job (shared/diag), not the
 * port's (ADR-0012 D4). Erase and program are thus at different (and, for
 * erase, non-uniform) granularities, which the download logic respects by
 * erasing the whole region via erase_range() and programming row by row.
 */
#include "fbl_flash.h"
#include "boot_types.h"    /* FBL_APP_FLASH_BASE, FBL_APP_FLASH_SIZE */
#include "cy_pdl.h"
#include <string.h>

#define FLASH_PROGRAM_ROW_SIZE    CY_FLASH_SIZEOF_ROW    /* 512 on this part */
#define FLASH_SMALL_SECTOR_SIZE   (8U * 1024U)
#define FLASH_LARGE_SECTOR_SIZE   (32U * 1024U)

/* Where the small (8 KB) sectors begin: 30 large * 32 KB = 960 KB above the
 * code-flash base (TRM / bench-confirmed). Everything from here up is 8 KB
 * sectors; everything below is 32 KB. */
#define FLASH_SMALL_REGION_START  (0x10000000U + (30U * FLASH_LARGE_SECTOR_SIZE))  /* 0x100F_0000 */

void fbl_flash_init(void)
{
    /* Lift the main-flash write-safety gate (see fbl_flash.h). ECT-only
     * convenience macro -> Cy_Flashc_MainWriteEnable_Base((FLASHC_Type*)FLASHC).
     * The app image is in MAIN (code) flash, not WORK flash, so this is the
     * one to enable. */
    Cy_Flashc_MainWriteEnable();
}

uint32_t fbl_flash_sector_size(uint32_t addr)
{
    return (addr >= FLASH_SMALL_REGION_START) ? FLASH_SMALL_SECTOR_SIZE
                                              : FLASH_LARGE_SECTOR_SIZE;
}

/* Bring-up diagnostics: the RAW cy_en_flashdrv_status_t from the last erase /
 * program call (0 = CY_FLASH_DRV_SUCCESS). The low byte is the specific
 * error offset from CY_FLASH_ID_ERROR -- e.g. 0x00 INV_PROT, 0x03
 * ROW_PROTECTED, 0x05 IPC_BUSY, 0x0C FLASH_SAFETY_ENABLED. Read in the
 * debugger when hal_* returns -1. Cheap to leave in. */
volatile uint32_t g_flash_raw_erase_status   = 0xFFFFFFFFU;
volatile uint32_t g_flash_raw_program_status = 0xFFFFFFFFU;

/* Defense in depth: every entry point re-checks this, not just the
 * composition root -- writing/erasing outside the app image region (the
 * FBL's own code at 0x1002_0000-0x1004_0000, or anything else) is not a
 * recoverable mistake on real hardware. */
static bool in_app_region(uint32_t addr, uint32_t len)
{
    if (len == 0U) { return false; }
    if (addr < FBL_APP_FLASH_BASE) { return false; }
    if (len > FBL_APP_FLASH_SIZE) { return false; }
    return (addr - FBL_APP_FLASH_BASE) <= (FBL_APP_FLASH_SIZE - len);
}

/* A blocking flash program/erase busies the code-flash macro; fetching an
 * instruction from that macro while it is busy is a read-while-write
 * violation -> HardFault (iBusErr). SysTick firing mid-operation and vectoring
 * to its flash-resident handler triggered exactly this during Seam 7 bring-up
 * (the fault frame showed iBusErr + sysTickAct). Run each blocking flash call
 * with interrupts masked so no ISR fetches from flash while the macro is busy;
 * Cy_SysLib_Enter/ExitCriticalSection saves and restores the prior PRIMASK.
 * The blocking op is short (a row program / sector erase) and the PC is
 * waiting for our response during it, so no CAN RX is missed. */
static cy_en_flashdrv_status_t flash_program_row_masked(uint32_t addr, const uint32_t *data)
{
    uint32_t saved = Cy_SysLib_EnterCriticalSection();
    cy_en_flashdrv_status_t st = Cy_Flash_ProgramRow(addr, data);
    Cy_SysLib_ExitCriticalSection(saved);
    return st;
}

static cy_en_flashdrv_status_t flash_erase_sector_masked(uint32_t addr)
{
    uint32_t saved = Cy_SysLib_EnterCriticalSection();
    cy_en_flashdrv_status_t st = Cy_Flash_EraseSector(addr);
    Cy_SysLib_ExitCriticalSection(saved);
    return st;
}

static int hal_read(uint32_t addr, uint8_t *dst, size_t len)
{
    if (!in_app_region(addr, (uint32_t)len)) { return -1; }
    (void)memcpy(dst, (const void *)addr, len);   /* code flash is memory-mapped */
    return 0;
}

static int hal_write(uint32_t addr, const uint8_t *src, size_t len)
{
    if (!in_app_region(addr, (uint32_t)len)) { return -1; }
    if ((addr % FLASH_PROGRAM_ROW_SIZE) != 0U) { return -1; }
    if ((len % FLASH_PROGRAM_ROW_SIZE) != 0U) { return -1; }

    for (uint32_t off = 0U; off < len; off += FLASH_PROGRAM_ROW_SIZE)
    {
        /* Cy_Flash_ProgramRow wants uint32_t*-aligned data in SRAM; src's own
         * alignment/placement isn't guaranteed, so stage each row through a
         * properly aligned local (stack = SRAM) buffer rather than casting
         * src directly. */
        uint32_t row[FLASH_PROGRAM_ROW_SIZE / 4U];
        (void)memcpy(row, &src[off], FLASH_PROGRAM_ROW_SIZE);
        g_flash_raw_program_status = (uint32_t)flash_program_row_masked(addr + off, row);
        if (g_flash_raw_program_status != CY_FLASH_DRV_SUCCESS) { return -1; }
    }

    Cy_SysLib_ClearFlashCacheAndBuffer();   /* else a readback may see stale cache */
    return 0;
}

/* Erase the single real hardware sector containing sector_addr -- 32 KB or
 * 8 KB depending on where it is (mixed geometry). sector_addr must be
 * aligned to that real sector's size. */
static int hal_erase_sector(uint32_t sector_addr)
{
    uint32_t ssz = fbl_flash_sector_size(sector_addr);
    if (!in_app_region(sector_addr, ssz)) { return -1; }
    if ((sector_addr % ssz) != 0U) { return -1; }

    g_flash_raw_erase_status = (uint32_t)flash_erase_sector_masked(sector_addr);
    Cy_SysLib_ClearFlashCacheAndBuffer();
    return (g_flash_raw_erase_status == CY_FLASH_DRV_SUCCESS) ? 0 : -1;
}

/* Erase [addr, addr+len). addr/len align to the coarse (32 KB) granularity --
 * always a valid sector boundary in both regions -- but the walk erases each
 * REAL sector (stepping by its actual 32 KB / 8 KB size), so a range spanning
 * the large->small transition erases the right sectors on each side. */
static int hal_erase_range(uint32_t addr, uint32_t len)
{
    if (!in_app_region(addr, len)) { return -1; }
    if ((addr % FLASH_LARGE_SECTOR_SIZE) != 0U) { return -1; }
    if ((len % FLASH_LARGE_SECTOR_SIZE) != 0U) { return -1; }

    uint32_t pos = addr;
    uint32_t end = addr + len;
    while (pos < end)
    {
        uint32_t ssz = fbl_flash_sector_size(pos);
        if ((end - pos) < ssz) { return -1; }   /* range not sector-aligned */
        if (flash_erase_sector_masked(pos) != CY_FLASH_DRV_SUCCESS) { return -1; }
        pos += ssz;
    }

    Cy_SysLib_ClearFlashCacheAndBuffer();
    return 0;
}

static const hal_flash_if_t g_fbl_flash_hal = {
    .read = hal_read,
    .write = hal_write,
    .erase_sector = hal_erase_sector,
    .erase_range = hal_erase_range,
    /* Coarse erase-alignment granularity for callers (the largest sector,
     * so a boundary aligned to it is valid everywhere). erase_range() erases
     * real sectors regardless of this value. */
    .sector_size = FLASH_LARGE_SECTOR_SIZE,
    .program_row_size = FLASH_PROGRAM_ROW_SIZE   /* 512 B */
};

const hal_flash_if_t *fbl_flash_hal(void) { return &g_fbl_flash_hal; }
