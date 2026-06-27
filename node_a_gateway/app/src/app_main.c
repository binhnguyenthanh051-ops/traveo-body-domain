/*
 * app_main.c — minimal M1 application (the FBL's jump target).
 *
 * Proves the FBL handover: blinks USER LED 4 (P12.2) — a DIFFERENT LED and a
 * slower rate than the FBL's LED 1 — so "FBL boots -> verifies -> jumps" is
 * visible (LED 1 fast blink hands over to LED 4 slow blink). Grows into the
 * FreeRTOS application in M2.
 */
#include "cybsp.h"
#include "cy_pdl.h"

/* CYTVII-B-E-1M-SK kit: User LED 4 (Blue) = P12.2 on the DUT (CYT2B7). */
#define APP_LED_PORT  GPIO_PRT12
#define APP_LED_PIN   2U

int main(void)
{
    (void)cybsp_init();
    Cy_GPIO_Pin_FastInit(APP_LED_PORT, APP_LED_PIN,
                         CY_GPIO_DM_STRONG_IN_OFF, 0U, HSIOM_SEL_GPIO);
    for (;;)
    {
        Cy_GPIO_Inv(APP_LED_PORT, APP_LED_PIN);
        Cy_SysLib_Delay(500U);   /* ms — slow blink = "app running" (vs FBL's fast) */
    }
}
