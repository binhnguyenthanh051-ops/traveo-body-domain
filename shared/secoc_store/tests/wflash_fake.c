/*
 * wflash_fake.c — host fake of TRAVEO T2G work flash. See the header for the
 * modelled behaviour. Test harness — exempt from MISRA.
 */
#include "wflash_fake.h"

#define WF_SIZE   (WF_SECTOR_SIZE * WF_SECTORS)
#define WF_WORDS  (WF_SIZE / 4U)

typedef enum { W_ERASED, W_PROGRAMMED, W_TORN } wstate_t;
typedef enum { STEP_OK, STEP_INTERRUPTED, STEP_OFF } step_t;

static wstate_t       g_state[WF_WORDS];
static uint32_t       g_value[WF_WORDS];
static wf_torn_mode_t g_torn_mode;
static int            g_budget;            /* steps left before the interruption; <0 = no limit */
static int            g_powered;
static unsigned       g_steps;
static unsigned       g_erases[WF_SECTORS];
static unsigned       g_cpu_reads;
static unsigned       g_reprogram;

static int in_range(uint32_t addr, size_t len)
{
    return (addr >= WF_BASE) && ((addr - WF_BASE) + len <= WF_SIZE) && ((addr & 3U) == 0U) &&
           ((len & 3U) == 0U);
}

static unsigned word_index(uint32_t addr) { return (addr - WF_BASE) / 4U; }

static step_t step(void)
{
    if (!g_powered) {
        return STEP_OFF;
    }
    g_steps++;
    if (g_budget == 0) {
        g_powered = 0;              /* power dies DURING this step */
        return STEP_INTERRUPTED;
    }
    if (g_budget > 0) {
        g_budget--;
    }
    return STEP_OK;
}

static int fake_erase_sector(uint32_t addr)
{
    if (!in_range(addr, WF_SECTOR_SIZE) || (((addr - WF_BASE) % WF_SECTOR_SIZE) != 0U)) {
        return -1;
    }
    step_t s = step();
    if (s == STEP_OFF) {
        return -1;
    }
    unsigned first = word_index(addr);
    for (unsigned i = 0U; i < WF_SECTOR_SIZE / 4U; ++i) {
        /* TRM §9.3.2: after a power loss mid-erase the state is unknown. */
        g_state[first + i] = (s == STEP_INTERRUPTED) ? W_TORN : W_ERASED;
        g_value[first + i] = 0xFFFFFFFFU;
    }
    g_erases[(addr - WF_BASE) / WF_SECTOR_SIZE]++;
    return (s == STEP_INTERRUPTED) ? -1 : 0;
}

static int fake_write(uint32_t addr, const uint8_t *src, size_t len)
{
    if (!in_range(addr, len) || (src == NULL)) {
        return -1;
    }
    for (size_t off = 0U; off < len; off += 4U) {
        unsigned w = word_index(addr + (uint32_t)off);
        if (g_state[w] != W_ERASED) {
            g_reprogram++;          /* not allowed on this flash without an erase */
            return -1;
        }
        step_t s = step();
        if (s == STEP_OFF) {
            return -1;
        }
        if (s == STEP_INTERRUPTED) {
            g_state[w] = W_TORN;
            return -1;
        }
        g_value[w] = (uint32_t)src[off] | ((uint32_t)src[off + 1U] << 8) |
                     ((uint32_t)src[off + 2U] << 16) | ((uint32_t)src[off + 3U] << 24);
        g_state[w] = W_PROGRAMMED;
    }
    return 0;
}

static int fake_blank_check(uint32_t addr, size_t len)
{
    if (!g_powered || !in_range(addr, len)) {
        return -1;
    }
    for (size_t off = 0U; off < len; off += 4U) {
        if (g_state[word_index(addr + (uint32_t)off)] != W_ERASED) {
            return 0;
        }
    }
    return 1;
}

static int fake_read_checked(uint32_t addr, uint8_t *dst, size_t len)
{
    if (!g_powered || !in_range(addr, len) || (dst == NULL)) {
        return -1;
    }
    for (size_t off = 0U; off < len; off += 4U) {
        unsigned w = word_index(addr + (uint32_t)off);
        uint32_t v = g_value[w];
        if (g_state[w] == W_ERASED) {
            return -1;              /* reading erased work flash = ECC error */
        }
        if (g_state[w] == W_TORN) {
            if (g_torn_mode == WF_TORN_ECC) {
                return -1;
            }
            v = 0x5A5A0000U | (uint32_t)w;   /* wrong bits, ECC happens to agree */
        }
        dst[off] = (uint8_t)v;
        dst[off + 1U] = (uint8_t)(v >> 8);
        dst[off + 2U] = (uint8_t)(v >> 16);
        dst[off + 3U] = (uint8_t)(v >> 24);
    }
    return 0;
}

static int fake_cpu_read(uint32_t addr, uint8_t *dst, size_t len)
{
    (void)addr; (void)dst; (void)len;
    g_cpu_reads++;                  /* a CPU read of a torn word would fault on target */
    return -1;
}

const hal_flash_if_t g_wflash_fake = {
    .read = fake_cpu_read,
    .write = fake_write,
    .erase_sector = fake_erase_sector,
    .erase_range = NULL,
    .sector_size = WF_SECTOR_SIZE,
    .program_row_size = 4U,
    .blank_check = fake_blank_check,
    .read_checked = fake_read_checked,
};

void wflash_fake_reset(void)
{
    for (unsigned i = 0U; i < WF_WORDS; ++i) {
        g_state[i] = W_ERASED;
        g_value[i] = 0xFFFFFFFFU;
    }
    for (unsigned i = 0U; i < WF_SECTORS; ++i) {
        g_erases[i] = 0U;
    }
    g_torn_mode = WF_TORN_ECC;
    g_budget = -1;
    g_powered = 1;
    g_steps = 0U;
    g_cpu_reads = 0U;
    g_reprogram = 0U;
}

void wflash_fake_set_torn_mode(wf_torn_mode_t mode) { g_torn_mode = mode; }
void wflash_fake_fail_after(int steps) { g_budget = steps; }

void wflash_fake_power_cycle(void)
{
    g_powered = 1;
    g_budget = -1;
}

void wflash_fake_decay_word(uint32_t addr)
{
    if (in_range(addr, 4U)) {
        g_state[word_index(addr)] = W_TORN;
    }
}

unsigned wflash_fake_steps(void) { return g_steps; }

unsigned wflash_fake_erase_count(uint32_t sector_addr)
{
    return in_range(sector_addr, WF_SECTOR_SIZE) ? g_erases[(sector_addr - WF_BASE) / WF_SECTOR_SIZE]
                                                 : 0U;
}

unsigned wflash_fake_cpu_reads(void) { return g_cpu_reads; }
unsigned wflash_fake_reprogram_violations(void) { return g_reprogram; }
