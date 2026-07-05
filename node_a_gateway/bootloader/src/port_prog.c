/*
 * port_prog.c — programming-mode entry + system reset (target).
 *
 * Terminal actions not tied to a specific hardware block:
 *   - enter_programming_mode: where the FBL goes when it must NOT boot the
 *     app. M3 Seam 4: drives the real UDS session (fbl_diag.c), replacing
 *     the Seam 1/2 bring-up aids (LED-only heartbeat, then the isotp echo).
 *   - system_reset: a Cortex-M system reset (the app uses it after a
 *     programming request, ADR-0007 D1; the FBL uses it after ECUReset,
 *     ADR-0007 D10).
 */
#include "fbl_port.h"
#include "fbl_diag.h"   /* M3 Seam 4: the real UDS session */
#include "fbl_flash.h"  /* M3 Seam 6 bring-up test, see below */
#include "boot_types.h" /* FBL_APP_FLASH_BASE/SIZE */
#include "cybsp.h"      /* target-only */
#include "cy_pdl.h"     /* target-only: Cy_GPIO_*, Cy_SysLib_Delay, NVIC_SystemReset */
#include <string.h>

/*
 * M3 Seam 6 bring-up: a one-shot, DESTRUCTIVE standalone test of
 * fbl_flash_hal() -- erase, write, read back a pattern -- run once before
 * the flash port is ever wired through UDS, same "isolate the risky new
 * thing" instinct as every seam so far. Same gated-test-flag pattern as the
 * App's can_task.c CAN_LOOPBACK_TEST: defaults OFF; flip to 1, rebuild,
 * flash, observe the g_flash_test_* results in the debugger, then flip back
 * to 0.
 *
 * DESTRUCTIVE: this erases the last sector of the app image region (an 8 KB
 * small sector -- the small sectors are at the TOP of code flash) and writes
 * one program row into it, which will very likely invalidate whatever app is
 * currently flashed there (its digest covers this range) -- the FBL's
 * existing fail-safe (ADR-0008 D1 step 5) handles that safely (stays
 * resident, does not jump), but the app will need reflashing afterward to
 * run normally again.
 */
#ifndef FBL_FLASH_BRINGUP_TEST
#define FBL_FLASH_BRINGUP_TEST  0
#endif

#if FBL_FLASH_BRINGUP_TEST
volatile int32_t  g_flash_test_erase_status  = -2;  /* -2 = not run yet; 0 = ok */
volatile int32_t  g_flash_test_write_status  = -2;
volatile int32_t  g_flash_test_verify_status = -2;  /* 0 = readback matched */
volatile uint32_t g_flash_test_sector_addr   = 0U;  /* which sector was hit */
volatile uint32_t g_flash_test_sector_size   = 0U;  /* 8192 expected (small) */

static void run_flash_bringup_test_once(void)
{
    const hal_flash_if_t *flash = fbl_flash_hal();

    /* Erase the LAST sector of the app region, then program+verify ONE row
     * (512 B) at its base. On this mixed-geometry flash the last sector is
     * an 8 KB small sector (small sectors sit at the top of code flash), so
     * its base and size come from fbl_flash_sector_size() -- not the coarse
     * 32 KB sector_size field, which would mis-address here. Exercises both
     * granularities: a small-sector erase + a row program. */
    uint32_t top  = FBL_APP_FLASH_BASE + FBL_APP_FLASH_SIZE;
    uint32_t ssz  = fbl_flash_sector_size(top - 1U);
    uint32_t sector_addr = top - ssz;
    g_flash_test_sector_addr = sector_addr;
    g_flash_test_sector_size = ssz;

    g_flash_test_erase_status = flash->erase_sector(sector_addr);

    static uint8_t pattern[512];   /* == flash->program_row_size on this part */
    for (size_t i = 0U; i < sizeof pattern; ++i) { pattern[i] = (uint8_t)(i ^ 0xA5U); }
    g_flash_test_write_status = flash->write(sector_addr, pattern, sizeof pattern);

    static uint8_t readback[sizeof pattern];
    (void)flash->read(sector_addr, readback, sizeof readback);
    g_flash_test_verify_status = (memcmp(pattern, readback, sizeof pattern) == 0) ? 0 : -1;
}
#endif

/*
 * This Empty-App BSP has NO user LED configured in the Device Configurator, so
 * there is no CYBSP_USER_LED_PORT/PIN. Get the kit's LED GPIO one of two ways:
 *
 *   (A) Device Configurator: enable a USER LED (alias CYBSP_USER_LED), Save,
 *       rebuild — the macros below pick it up automatically. Preferred: the
 *       configurator knows the kit's correct pin.
 *
 *   (B) Kit guide / schematic: set FBL_LED_PORT / FBL_LED_PIN to the user-LED
 *       GPIO from the CYTVII-B-E-1M-SK documentation; this file inits it
 * directly.
 *
 * Until one is set, the FBL stays resident with no blink (still correct).
 */
#if defined(CYBSP_USER_LED_PORT)
  /* (A) a USER LED was configured in the Device Configurator — use it. */
  #define FBL_LED_PORT  CYBSP_USER_LED_PORT
  #define FBL_LED_PIN   CYBSP_USER_LED_PIN
#else
  /* (B) CYTVII-B-E-1M-SK kit guide: User LED 1 (Blue) = P19.0 on the DUT
   * (CYT2B7). (LED 4 = P12.2 is the alternative; LED 3 = P1.4 is on the kit's
   * PSoC/KitProg, not the DUT, so it cannot be driven from here.) */
  #define FBL_LED_PORT  GPIO_PRT19
  #define FBL_LED_PIN   0U
#endif

void fbl_port_enter_programming_mode(void)
{
    /* Only now, on the programming-mode path, lift the main-flash write
     * safety gate (ADR-0008 D1: this function is reached only when the FBL
     * has decided NOT to jump). The boot/jump path never enables it, so
     * code-flash writes stay disabled whenever control is handed to the app. */
    fbl_flash_init();

#if FBL_FLASH_BRINGUP_TEST
    run_flash_bringup_test_once();
#endif

    fbl_diag_init();

#if defined(FBL_LED_PORT)
    /* Configure the LED as a strong-drive output directly (works whether or not
     * the BSP configured it), then blink it as the "in bootloader" heartbeat. */
    Cy_GPIO_Pin_FastInit(FBL_LED_PORT, FBL_LED_PIN,
                         CY_GPIO_DM_STRONG_IN_OFF, 0U, HSIOM_SEL_GPIO);
    for (;;)
    {
        fbl_diag_tick(fbl_port_now_ms());
        Cy_GPIO_Inv(FBL_LED_PORT, FBL_LED_PIN);
        Cy_SysLib_Delay(250U);   /* ms — fast blink = "in bootloader" */
    }
#else
    /* No LED pin known yet (see (A)/(B) above). Stay resident. */
    for (;;)
    {
        fbl_diag_tick(fbl_port_now_ms());
    }
#endif
}

void fbl_port_system_reset(void)
{
    NVIC_SystemReset();   /* CMSIS core; requests SYSRESETREQ, does not return */
}
