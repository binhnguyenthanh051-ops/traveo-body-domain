/*
 * app_port_fake.c — host fake for the app port (app_port.h).
 *
 * Backs the .noinit region with a static buffer and records the reset call,
 * capturing the handshake mode AT reset time so a test can prove the write
 * happened before the reset (the ordering the target __DSB guarantees). Test
 * harness — exempt from MISRA (see docs/coding-standard.md).
 */
#include "app_port.h"
#include <string.h>

static fbl_handshake_t g_region;
static int             g_reset_called;
static uint8_t         g_mode_at_reset;

/* ---- test API ---- */
void fake_app_reset(void)
{
    memset(&g_region, 0, sizeof g_region);
    g_reset_called  = 0;
    g_mode_at_reset = 0xFFu;
}
fbl_handshake_t *fake_app_region(void)       { return &g_region; }
int              fake_app_reset_called(void) { return g_reset_called; }
int              fake_app_mode_at_reset(void){ return (int)g_mode_at_reset; }

/* ---- app_port implementation ---- */
fbl_handshake_t *app_port_noinit(void)       { return &g_region; }

void app_port_system_reset(void)
{
    g_mode_at_reset = g_region.mode;   /* snapshot what the app wrote */
    ++g_reset_called;
}
