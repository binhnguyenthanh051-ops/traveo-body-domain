/*
 * fbl_main.c — FBL entry: wire the host-tested boot core (shared/boot) to the
 * target port. Deliberately minimal (ADR-0004): no scheduler, no RTOS.
 *
 * The decision logic lives in fbl_run_boot() (shared/boot, host-tested). This
 * file only performs the terminal action the core returns.
 */
#include "boot.h"
#include "fbl_port.h"
#include "fbl_crypto.h"  /* M4: the crypto/IPC target binding (fbl_crypto_port) */
#include "fbl_tcb_probe.h" /* M4 Seam 5: the CM4-side TCB-isolation fault demo (ADR-0020 D6) */
#include "fbl_can.h"    /* M3 Seam 1: CAN must be alive before the knock window polls it */
#include "fbl_time.h"   /* M3 Seam 2 prerequisite: real ms tick for N_Cr / the knock dwell */
#include "cybsp.h"      /* target-only: clocks + BSP pins (incl. the user LED) */
#include "cycfg.h"      /* target-only: cycfg_config_init (Device Configurator: CAN clock/pins) */
#include "cy_pdl.h"     /* target-only: __enable_irq (CMSIS core intrinsic) */
#if FBL_DIGEST_ALGO == FBL_DIGEST_SHA256
#include "crypto_service.h"  /* M4 Seam 4: crypto_service_init (bind the port) */
#endif

/* Crypto bring-up checks (Seam 2 hash + Seam 3 verify) are retained behind
 * FBL_CRYPTO_BRINGUP (default 0 in fbl_crypto.h; M4 is proven). Enabling the
 * Seam-3 verify also needs src/test_signed_image.h from make_test_image.py. */
#if FBL_CRYPTO_BRINGUP
#include "test_signed_image.h"
#endif

int main(void)
{
    /* Bring up clocks and BSP pins so timing + the LED heartbeat work. */
    (void)cybsp_init();

    /* M4 Seam-0 regression fix (CAN): the BSP applies the Device Configurator's
     * peripheral setup — CANFD clock, pins, and peripheral config (design.modus)
     * — inside cybsp_init() via cycfg_config_init(), but ONLY on the core the
     * gate in cybsp.c selects. Through M1-M3 (COMBINED project, prebuilt CM0+)
     * that was the CM4; after the Seam-0 split, CY_USING_PREBUILT_CM0P_IMAGE is
     * no longer defined for proj_cm4, so the gate hands it to the CM0+ instead
     * and the CM4's cybsp_init() SKIPS it. port_can.c doesn't configure the CAN
     * clock/pins itself (it only calls Cy_CANFD_Init), so CAN went dark. Re-run
     * the config here on the CM4 explicitly — it owns CAN, it configures CAN,
     * as in M1-M3. Idempotent with any CM0+-side config and runs after the CM0+
     * released the CM4, so it is the final word on those pins/clocks. */
    cycfg_config_init();

    /* The vendor startup leaves PRIMASK set (interrupts globally masked)
     * after SystemInit()/cybsp_init(), expecting whatever runs next to turn
     * them back on -- an RTOS scheduler start does this for the App for
     * free (ADR-0010). The FBL is bare-metal (ADR-0004: no RTOS), so nothing
     * else will ever do this; without it, SysTick counts down correctly in
     * hardware but its interrupt is never taken, so fbl_port_now_ms() never
     * advances (silicon-verified during M3 Seam 3 bring-up: PRIMASK read
     * back as 1, s_tick_ms stuck at 0 -- the knock dwell's timeout branch
     * could then only ever be escaped by a knock, never by elapsed time). */
    __enable_irq();

    /* Real timing before anything that depends on fbl_port_now_ms() being
     * wall-clock accurate (the knock dwell just below, and ISO-TP's N_Cr
     * timeout once Seam 2 is wired in). */
    fbl_time_init();

    /* CAN must be up before fbl_run_boot(), not only once programming mode is
     * entered: the knock window (ADR-0008 D2) needs fbl_port_tool_contact()
     * to see real frames during the dwell, which happens inside
     * fbl_run_boot() itself. If CAN fails to come up, tool_contact() simply
     * never reports contact -- indistinguishable from "nobody knocked" to
     * the boot decision (ADR-0015 D6: no escalation needed here either). */
    fbl_can_init();

    /* M4 crypto bring-up checks — retained behind FBL_CRYPTO_BRINGUP (default
     * off, M4 proven); not part of the boot decision. */
#if FBL_CRYPTO_BRINGUP
    volatile bool crypto_ok = fbl_crypto_bringup_hash();   /* Seam 2: HW SHA-256 round trip */
    (void)crypto_ok;
#endif

    /* M4 Seam 5 (ADR-0020 D6): prove the CM0+ walls from the CM4 side. The CM0+
     * raised them before releasing this core, so with FBL_M4_SEAM5_PROBE the two
     * illegal accesses fault into fbl_tcb_probe's HardFault_Handler (inspect
     * g_tcb_probe over OpenOCD). No-op otherwise. Bench-only — remove with the
     * rest of the scaffolding at seam sign-off. */
    fbl_tcb_probe();

#if FBL_CRYPTO_BRINGUP
    /* Seam 3: verify three images embedded in this FBL's flash. Set a
     * breakpoint on seam3_pass and inspect the verdicts (VALID=1, INVALID=0,
     * ERROR=2), or drive the LED from seam3_pass. */
    volatile crypto_verdict_t v_good =
        fbl_crypto_bringup_verify((uint32_t)(uintptr_t)test_image_good,
                                  TEST_IMAGE_LEN, TEST_IMAGE_KEY_ID);
    volatile crypto_verdict_t v_tampered =
        fbl_crypto_bringup_verify((uint32_t)(uintptr_t)test_image_tampered,
                                  TEST_IMAGE_LEN, TEST_IMAGE_KEY_ID);
    volatile crypto_verdict_t v_wrongkey =
        fbl_crypto_bringup_verify((uint32_t)(uintptr_t)test_image_wrongkey,
                                  TEST_IMAGE_LEN, TEST_IMAGE_KEY_ID);
    volatile bool seam3_pass = (v_good == CRYPTO_VERDICT_VALID) &&
                               (v_tampered == CRYPTO_VERDICT_INVALID) &&
                               (v_wrongkey == CRYPTO_VERDICT_INVALID);
    (void)v_good; (void)v_tampered; (void)v_wrongkey; (void)seam3_pass;
#endif

    /* M4 Seam 4 (SHA-256 mode only): bind the crypto-service port BEFORE
     * fbl_run_boot(), so the boot decision's fbl_app_image_valid() can delegate
     * authenticity to the M0+ (ADR-0016). Not compiled in CRC32 mode, so the
     * M1-M3 boot path is byte-for-byte unchanged until FBL_DIGEST_ALGO flips. */
#if FBL_DIGEST_ALGO == FBL_DIGEST_SHA256
    crypto_service_init(fbl_crypto_port());
#endif

    fbl_boot_action_t action = fbl_run_boot();

    if (action == FBL_ACTION_JUMP_APP)
    {
        /* De-init to the handover contract (ADR-0008 D4 / review B3), then jump.
         * jump_to_app reads MSP/reset from the app vector table and never
         * returns. */
        fbl_port_deinit_for_jump();
        fbl_port_jump_to_app((uint32_t)fbl_port_app_image_base());
    }

    /* Programming mode (M1: minimal; M3: UDS programming services). */
    fbl_port_enter_programming_mode();

    for (;;)
    {
        /* never returns to the startup code */
    }
}
