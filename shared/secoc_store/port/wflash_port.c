/*
 * wflash_port.c — TRAVEO T2G work-flash instance of hal_flash_if_t for the
 * freshness store (ADR-0024 D3/D4). Target-only. See the header.
 *
 * PDL calls (eCT flash, CY_IP_MXFLASHC_VERSION_ECT, as in ADR-0012 S1):
 *   erase_sector  Cy_Flash_EraseSector          (SROM via the CM0+, blocking)
 *   write         Cy_Flash_Program_WorkFlash    one 32-bit word per call, with
 *                                               the SROM's own blank check, so
 *                                               a non-erased word is refused
 *   blank_check   Cy_Flash_BlankCheck           SUCCESS = blank,
 *                                               FLASH_NOT_ERASED = not blank
 *   read_checked  one M-DMA 1D transfer         an uncorrectable ECC error is
 *                                               a SRC_BUS_ERROR in the channel
 *                                               status, not a CPU fault (TRM
 *                                               §9.3.1 measure 1)
 *
 * Interrupts stay ENABLED around erase/program. M3's code-flash port masks
 * them because an ISR fetching from the busy code-flash macro is a
 * read-while-write fault; here the busy macro is WORK flash, which no code
 * runs from. TRM-verify (ADR-0024 V2): no fetch or read hits work flash
 * during the op.
 *
 * @impl ADR-0024 D3 : blank check + DMA read, so the CPU never reads a store slot
 * @impl ADR-0024 D4 : target hal_flash_if_t with blank_check and read_checked
 */
#include "wflash_port.h"

#define WF_WORD                 4U
#define WF_DMA_POLL_LIMIT       100000U     /* a 20 B transfer takes far less */
#define WF_DMA_ERRORS           (CY_DMAC_INTR_SRC_BUS_ERROR | CY_DMAC_INTR_DST_BUS_ERROR | \
                                 CY_DMAC_INTR_SRC_MISAL | CY_DMAC_INTR_DST_MISAL |         \
                                 CY_DMAC_INTR_CURR_PTR_NULL | CY_DMAC_INTR_DESCR_BUS_ERROR)

volatile uint32_t g_wflash_raw_status   = 0xFFFFFFFFU;
volatile uint32_t g_wflash_raw_dma_intr = 0U;

static wflash_port_cfg_t s_cfg;

static int status_to_rc(cy_en_flashdrv_status_t st)
{
    g_wflash_raw_status = (uint32_t)st;
    return (st == CY_FLASH_DRV_SUCCESS) ? 0 : -1;
}

static int wf_erase_sector(uint32_t sector_addr)
{
    return status_to_rc(Cy_Flash_EraseSector(sector_addr));
}

static int wf_write(uint32_t addr, const uint8_t *src, size_t len)
{
    if ((src == NULL) || ((addr % WF_WORD) != 0U) || ((len % WF_WORD) != 0U))
    {
        return -1;
    }
    for (size_t off = 0U; off < len; off += WF_WORD)
    {
        /* One word at a time, in address order: the store relies on the mark
         * word being programmed after the body (ADR-0024 D2). */
        s_cfg.shared->prog[0] = (uint32_t)src[off] | ((uint32_t)src[off + 1U] << 8U) |
                                ((uint32_t)src[off + 2U] << 16U) | ((uint32_t)src[off + 3U] << 24U);
        const cy_stc_flash_programrow_config_t pc = {
            .destAddr = (const uint32_t *)(addr + (uint32_t)off),
            .dataAddr = &s_cfg.shared->prog[0],
            .blocking = CY_FLASH_PROGRAMROW_BLOCKING,
            .skipBC   = CY_FLASH_PROGRAMROW_BLANK_CHECK,
            .dataSize = CY_FLASH_PROGRAMROW_DATA_SIZE_32BIT,
            .dataLoc  = CY_FLASH_PROGRAMROW_DATA_LOCATION_SRAM,
            .intrMask = CY_FLASH_PROGRAMROW_NOT_SET_INTR_MASK,
        };
        if (status_to_rc(Cy_Flash_Program_WorkFlash(&pc)) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int wf_blank_check(uint32_t addr, size_t len)
{
    if (((addr % WF_WORD) != 0U) || (len == 0U) || ((len % WF_WORD) != 0U))
    {
        return -1;
    }
    const cy_stc_flash_blankcheck_config_t bc = {
        .addrToBeChecked       = (const uint32_t *)addr,
        .numOfWordsToBeChecked = (uint32_t)(len / WF_WORD),
    };
    cy_en_flashdrv_status_t st = Cy_Flash_BlankCheck(&bc, CY_FLASH_DRIVER_BLOCKING);
    g_wflash_raw_status = (uint32_t)st;
    if (st == CY_FLASH_DRV_SUCCESS)
    {
        return 1;
    }
    if (st == CY_FLASH_DRV_FLASH_NOT_ERASED)
    {
        return 0;
    }
    return -1;
}

static int wf_read_checked(uint32_t addr, uint8_t *dst, size_t len)
{
    if ((dst == NULL) || ((addr % WF_WORD) != 0U) || (len == 0U) || ((len % WF_WORD) != 0U) ||
        (len > sizeof s_cfg.shared->rd))
    {
        return -1;
    }
    DMAC_Type *dmac = s_cfg.dmac;
    const uint32_t ch = s_cfg.channel;

    const cy_stc_dmac_descriptor_config_t dc = {
        .retrigger       = CY_DMAC_RETRIG_IM,
        .interruptType   = CY_DMAC_DESCR,
        .triggerOutType  = CY_DMAC_DESCR,
        .channelState    = CY_DMAC_CHANNEL_DISABLED,
        .triggerInType   = CY_DMAC_DESCR,
        .dataPrefetch    = false,
        .dataSize        = CY_DMAC_WORD,
        .srcTransferSize = CY_DMAC_TRANSFER_SIZE_WORD,
        .dstTransferSize = CY_DMAC_TRANSFER_SIZE_WORD,
        .descriptorType  = CY_DMAC_1D_TRANSFER,
        .srcAddress      = (void *)addr,
        .dstAddress      = (void *)&s_cfg.shared->rd[0],
        .srcXincrement   = 1,
        .dstXincrement   = 1,
        .xCount          = (uint32_t)(len / WF_WORD),
        .srcYincrement   = 0,
        .dstYincrement   = 0,
        .yCount          = 1U,
        .nextDescriptor  = NULL,
    };
    if (Cy_DMAC_Descriptor_Init(&s_cfg.shared->desc, &dc) != CY_DMAC_SUCCESS)
    {
        return -1;
    }
    const cy_stc_dmac_channel_config_t cc = {
        .descriptor = &s_cfg.shared->desc,
        .priority   = 3U,
        .enable     = false,
        .bufferable = false,
    };
    if (Cy_DMAC_Channel_Init(dmac, ch, &cc) != CY_DMAC_SUCCESS)
    {
        return -1;
    }
    Cy_DMAC_Channel_ClearInterrupt(dmac, ch, CY_DMAC_INTR_MASK);
    Cy_DMAC_Channel_Enable(dmac, ch);
    if (Cy_TrigMux_SwTrigger(s_cfg.sw_trigger, CY_TRIGGER_TWO_CYCLES) != CY_TRIGMUX_SUCCESS)
    {
        Cy_DMAC_Channel_Disable(dmac, ch);
        return -1;
    }

    uint32_t intr = 0U;
    for (uint32_t i = 0U; i < WF_DMA_POLL_LIMIT; ++i)
    {
        intr = Cy_DMAC_Channel_GetInterruptStatus(dmac, ch);
        if ((intr & (CY_DMAC_INTR_COMPLETION | WF_DMA_ERRORS)) != 0U)
        {
            break;
        }
    }
    Cy_DMAC_Channel_Disable(dmac, ch);
    Cy_DMAC_Channel_ClearInterrupt(dmac, ch, CY_DMAC_INTR_MASK);
    g_wflash_raw_dma_intr = intr;

    /* An uncorrectable ECC error in the source lands here as SRC_BUS_ERROR. A
     * timeout (no completion) is treated the same: the slot is unreadable. */
    if (((intr & WF_DMA_ERRORS) != 0U) || ((intr & CY_DMAC_INTR_COMPLETION) == 0U))
    {
        return -1;
    }
    for (size_t i = 0U; i < len; ++i)
    {
        dst[i] = (uint8_t)(s_cfg.shared->rd[i / WF_WORD] >> (8U * (uint32_t)(i % WF_WORD)));
    }
    return 0;
}

/* Not const: sector_size comes from the node's config at init. */
static hal_flash_if_t s_hal = {
    .read             = NULL,       /* deliberately absent: the store never CPU-reads (D3) */
    .write            = wf_write,
    .erase_sector     = wf_erase_sector,
    .erase_range      = NULL,
    .sector_size      = 0U,
    .program_row_size = WF_WORD,
    .blank_check      = wf_blank_check,
    .read_checked     = wf_read_checked,
};

const hal_flash_if_t *wflash_port_init(const wflash_port_cfg_t *cfg)
{
    if ((cfg == NULL) || (cfg->dmac == NULL) || (cfg->shared == NULL) || (cfg->sector_size == 0U))
    {
        return NULL;
    }
    s_cfg = *cfg;
    s_hal.sector_size = cfg->sector_size;

    Cy_Flashc_WorkWriteEnable();    /* work-flash write-safety gate, like M3's MainWriteEnable */
    Cy_DMAC_Enable(cfg->dmac);
    return &s_hal;
}
