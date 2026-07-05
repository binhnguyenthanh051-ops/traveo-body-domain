/*
 * flash_fake.c — host fake for hal_flash_if_t (shared/hal), backing an
 * in-memory region for the download/routine-control tests. Test harness —
 * exempt from MISRA.
 */
#include "hal.h"
#include <string.h>

#define FAKE_FLASH_SIZE           8192U
#define FAKE_FLASH_SECTOR_SIZE    4096U
#define FAKE_FLASH_ROW_SIZE       256U

static uint8_t g_flash[FAKE_FLASH_SIZE];

void fake_flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof g_flash);   /* erased-flash convention */
}

const uint8_t *fake_flash_buffer(void) { return g_flash; }
uint8_t *fake_flash_buffer_mut(void) { return g_flash; }   /* test fixture construction only */
size_t fake_flash_size(void) { return sizeof g_flash; }

static int fake_flash_read(uint32_t addr, uint8_t *dst, size_t len)
{
    if ((addr + len) > FAKE_FLASH_SIZE) { return -1; }
    memcpy(dst, &g_flash[addr], len);
    return 0;
}

static int fake_flash_write(uint32_t addr, const uint8_t *src, size_t len)
{
    if ((addr + len) > FAKE_FLASH_SIZE) { return -1; }
    memcpy(&g_flash[addr], src, len);
    return 0;
}

static int fake_flash_erase_sector(uint32_t sector_addr)
{
    if ((sector_addr + FAKE_FLASH_SECTOR_SIZE) > FAKE_FLASH_SIZE) { return -1; }
    memset(&g_flash[sector_addr], 0xFF, FAKE_FLASH_SECTOR_SIZE);
    return 0;
}

static int fake_flash_erase_range(uint32_t addr, uint32_t len)
{
    if ((addr + len) > FAKE_FLASH_SIZE) { return -1; }
    memset(&g_flash[addr], 0xFF, len);
    return 0;
}

static const hal_flash_if_t g_fake_flash = {
    .read = fake_flash_read,
    .write = fake_flash_write,
    .erase_sector = fake_flash_erase_sector,
    .erase_range = fake_flash_erase_range,
    .sector_size = FAKE_FLASH_SECTOR_SIZE,
    .program_row_size = FAKE_FLASH_ROW_SIZE
};

const hal_flash_if_t *fake_flash_hal(void) { return &g_fake_flash; }
