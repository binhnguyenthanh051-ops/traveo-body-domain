/*
 * boot_port_fake.c — host fake for the FBL port (fbl_port.h).
 *
 * Backs every fbl_port_* function with settable state and inspection helpers,
 * so the boot core can be driven entirely from the test. Test harness — exempt
 * from MISRA (see docs/coding-standard.md).
 */
#include "fbl_port.h"
#include <string.h>

#define FAKE_BACKUP_REGS  8U
#define FAKE_TICK_MS      50U     /* fbl_port_now_ms advances by this each call */

/* ---- settable / observable state ---- */
static fbl_reset_cause_t g_cause;
static fbl_lifecycle_t   g_lifecycle;
static fbl_handshake_t   g_noinit;
static uint32_t          g_backup[FAKE_BACKUP_REGS];
static uint32_t          g_now_ms;
static uint32_t          g_contact_at_ms;        /* UINT32_MAX = never */
static const uint8_t    *g_app_base;
static uint32_t          g_app_len;
static int               g_prime_called;
static int               g_clear_cause_called;
static int               g_jump_called;
static uint32_t          g_jump_msp;
static uint32_t          g_jump_reset;
static int               g_programming_called;
static int               g_reset_called;

/* ---- test API ---- */
void fake_reset(void)
{
    g_cause = FBL_RST_POWER_ON;
    g_lifecycle = FBL_LC_NORMAL;
    memset(&g_noinit, 0, sizeof g_noinit);
    memset(g_backup, 0, sizeof g_backup);
    g_now_ms = 0U;
    g_contact_at_ms = 0xFFFFFFFFU;
    g_app_base = NULL;
    g_app_len = 0U;
    g_prime_called = 0;
    g_clear_cause_called = 0;
    g_jump_called = 0;
    g_jump_msp = 0U;
    g_jump_reset = 0U;
    g_programming_called = 0;
    g_reset_called = 0;
}

void              fake_set_cause(fbl_reset_cause_t c)     { g_cause = c; }
void              fake_set_lifecycle(fbl_lifecycle_t lc)  { g_lifecycle = lc; }
fbl_handshake_t  *fake_noinit(void)                       { return &g_noinit; }
void              fake_set_backup(uint8_t i, uint32_t v)  { if (i < FAKE_BACKUP_REGS) g_backup[i] = v; }
uint32_t          fake_get_backup(uint8_t i)              { return (i < FAKE_BACKUP_REGS) ? g_backup[i] : 0U; }
void              fake_set_contact_at(uint32_t ms)        { g_contact_at_ms = ms; }
void              fake_set_app(const uint8_t *b, uint32_t n) { g_app_base = b; g_app_len = n; }
int               fake_prime_called(void)                 { return g_prime_called; }
int               fake_jump_called(void)                  { return g_jump_called; }
uint32_t          fake_jump_msp(void)                     { return g_jump_msp; }
uint32_t          fake_jump_reset(void)                   { return g_jump_reset; }
int               fake_programming_called(void)           { return g_programming_called; }

/* ---- fbl_port implementation ---- */
fbl_reset_cause_t fbl_port_reset_cause(void)              { return g_cause; }
void              fbl_port_clear_reset_cause(void)        { ++g_clear_cause_called; }
fbl_lifecycle_t   fbl_port_lifecycle(void)               { return g_lifecycle; }
fbl_handshake_t  *fbl_port_noinit(void)                  { return &g_noinit; }
void              fbl_port_prime_noinit(void)            { ++g_prime_called; memset(&g_noinit, 0, sizeof g_noinit); }
uint32_t          fbl_port_backup_read(uint8_t idx)      { return (idx < FAKE_BACKUP_REGS) ? g_backup[idx] : 0U; }
void              fbl_port_backup_write(uint8_t idx, uint32_t v) { if (idx < FAKE_BACKUP_REGS) g_backup[idx] = v; }

uint32_t fbl_port_now_ms(void)
{
    uint32_t now = g_now_ms;
    g_now_ms += FAKE_TICK_MS;        /* advance so dwell loops terminate */
    return now;
}

bool fbl_port_tool_contact(void)
{
    return (g_now_ms >= g_contact_at_ms);
}

const uint8_t *fbl_port_app_image_base(void)             { return g_app_base; }
uint32_t       fbl_port_app_region_len(void)             { return g_app_len; }

void fbl_port_deinit_for_jump(void)                      { /* host no-op */ }

void fbl_port_jump_to_app(uint32_t msp, uint32_t reset)
{
    ++g_jump_called;
    g_jump_msp = msp;
    g_jump_reset = reset;
}

void fbl_port_enter_programming_mode(void)               { ++g_programming_called; }
void fbl_port_system_reset(void)                         { ++g_reset_called; }
