/*
 * port_prog.c — programming-mode entry + system reset (target).  STUB.
 *
 * Terminal actions not tied to a specific hardware block:
 *   - enter_programming_mode: where the FBL goes when it must NOT boot the app.
 *     M1 = stay resident (blink a heartbeat LED); M3 fills in UDS services.
 *   - system_reset: a Cortex-M system reset (app uses it after a programming
 *     request; ADR-0007 D1).
 */
#include "fbl_port.h"
#include "cybsp.h"      /* target-only */
#include "cy_pdl.h"     /* target-only: Cy_GPIO_*, Cy_SysLib_Delay */

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
#if defined(FBL_LED_PORT)
    /* Configure the LED as a strong-drive output directly (works whether or not
     * the BSP configured it), then blink it as the "in bootloader" heartbeat. */
    Cy_GPIO_Pin_FastInit(FBL_LED_PORT, FBL_LED_PIN,
                         CY_GPIO_DM_STRONG_IN_OFF, 0U, HSIOM_SEL_GPIO);
    for (;;)
    {
        Cy_GPIO_Inv(FBL_LED_PORT, FBL_LED_PIN);
        Cy_SysLib_Delay(250U);   /* ms — fast blink = "in bootloader" */
    }
#else
    /* No LED pin known yet (see (A)/(B) above). Stay resident. */
    for (;;)
    {
        /* idle in the bootloader */
    }
#endif
}

void fbl_port_system_reset(void)
{
    /* TODO(board): NVIC_SystemReset() (SCB AIRCR SYSRESETREQ) via the CMSIS
     * core header. Must not return. */
    for (;;)
    {
        /* placeholder until the CMSIS reset call is wired in */
    }
}
