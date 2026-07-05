/*
 * uds_routine_control.c — 0x31 RoutineControl: erase + CRC verify (ADR-0012
 * D6/D8).
 *
 * check-programmed-image reuses fbl_app_image_valid() unchanged (ADR-0008
 * D3) -- no new verify interface. Request layout is this project's own
 * simplification, not the full UDS address/length-format-identifier scheme:
 * [routineControlType, routineId(2)] + erase-only [addr(4), len(2)].
 */
#include "uds_routine_control.h"
#include "boot.h"

static const hal_flash_if_t *g_flash;
static const uint8_t *g_app_base;
static uint32_t g_app_region_len;

void uds_routine_control_init(const hal_flash_if_t *flash,
                               const uint8_t *app_base, uint32_t app_region_len)
{
    g_flash = flash;
    g_app_base = app_base;
    g_app_region_len = app_region_len;
}

static uds_result_t do_erase(const uint8_t *req, size_t req_len,
                              uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 9U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }

    uint32_t addr = ((uint32_t)req[3] << 24) | ((uint32_t)req[4] << 16) |
                    ((uint32_t)req[5] << 8)  |  (uint32_t)req[6];
    uint32_t len  = ((uint32_t)req[7] << 8) | (uint32_t)req[8];

    int rc;
    if (g_flash->erase_range != NULL)
    {
        rc = g_flash->erase_range(addr, len);
    }
    else
    {
        rc = 0;
        uint32_t sector = g_flash->sector_size;
        for (uint32_t off = 0U; off < len; off += sector)
        {
            if (g_flash->erase_sector(addr + off) != 0) { rc = -1; break; }
        }
    }
    if (rc != 0) { return UDS_NRC_GENERAL_PROGRAMMING_FAILURE; }

    if (resp_cap < 3U) { return UDS_NRC_GENERAL_REJECT; }
    resp[0] = req[0];
    resp[1] = req[1];
    resp[2] = req[2];
    *resp_len = 3U;
    return UDS_NRC_NONE;
}

static uds_result_t do_check_image(const uint8_t *req, uint8_t *resp,
                                    size_t resp_cap, size_t *resp_len)
{
    if (!fbl_app_image_valid(g_app_base, g_app_region_len))
    {
        return UDS_NRC_GENERAL_PROGRAMMING_FAILURE;
    }
    if (resp_cap < 4U) { return UDS_NRC_GENERAL_REJECT; }
    resp[0] = req[0];
    resp[1] = req[1];
    resp[2] = req[2];
    resp[3] = 0x00U;   /* status: pass */
    *resp_len = 4U;
    return UDS_NRC_NONE;
}

static uds_result_t handle(const uint8_t *req, size_t req_len,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (req_len < 3U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }
    if (req[0] != 0x01U) { return UDS_NRC_REQUEST_OUT_OF_RANGE; }   /* only "start" in M3 */

    uint16_t routine_id = ((uint16_t)req[1] << 8) | req[2];

    if (routine_id == UDS_ROUTINE_ERASE_MEMORY)
    {
        return do_erase(req, req_len, resp, resp_cap, resp_len);
    }
    if (routine_id == UDS_ROUTINE_CHECK_PROGRAMMED_IMAGE)
    {
        return do_check_image(req, resp, resp_cap, resp_len);
    }
    return UDS_NRC_REQUEST_OUT_OF_RANGE;
}

static const uds_handler_if_t g_if = {
    .sid = UDS_SID_ROUTINE_CONTROL,
    .handle = handle
};

const uds_handler_if_t *uds_routine_control_handler(void) { return &g_if; }
