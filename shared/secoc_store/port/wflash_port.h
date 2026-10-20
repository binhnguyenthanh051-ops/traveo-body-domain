/*
 * wflash_port.h — TRAVEO T2G work-flash instance of hal_flash_if_t for the
 * freshness store (ADR-0024 D4). Target-only: includes the PDL.
 *
 * The same source serves both nodes. What differs per node is DATA, passed in
 * wflash_port_cfg_t (ADR-0004 config over #ifdef): the M-DMA software-trigger
 * line, the sector size, and where the DMA-visible scratch lives.
 *
 * Coherency (Node B, CM7 with D-cache, ADR-0018 D6): the M-DMA reads the
 * descriptor and writes the read buffer, and the CM0+ (SROM) reads the program
 * source word. None of them sees the CM7's D-cache, so `shared` MUST be in
 * non-cacheable memory on a cached core (`.cy_sharedmem` -> ram_noncache on
 * Node B). Node A's CM4 has no cache, so plain RAM is fine there.
 */
#ifndef WFLASH_PORT_H
#define WFLASH_PORT_H

#include "cy_pdl.h"
#include "hal.h"

/* DMA- and CM0+-visible scratch. 32-byte aligned (Cy_Flash_Program_WorkFlash
 * wants a 32-byte aligned source). */
typedef struct {
    cy_stc_dmac_descriptor_t desc;
    uint32_t prog[8];               /* program source, one word used */
    uint32_t rd[8];                 /* DMA read destination, >= one record */
} wflash_port_shared_t;

typedef struct {
    DMAC_Type            *dmac;        /* M-DMA instance (DMAC) */
    uint32_t              channel;     /* a channel nothing else uses */
    uint32_t              sw_trigger;  /* TRIG_OUT_MUX_n_MDMA_TR_IN<channel> */
    uint32_t              sector_size; /* work-flash small sector, bytes */
    wflash_port_shared_t *shared;      /* see the coherency note above */
} wflash_port_cfg_t;

/* Enables work-flash writes and the M-DMA, then returns the HAL instance.
 * NULL on a bad config. Call once, before secoc_store_mount(). */
const hal_flash_if_t *wflash_port_init(const wflash_port_cfg_t *cfg);

/* Bring-up diagnostics: the raw PDL status of the last call (0 = success),
 * and the DMA channel's raw interrupt status after the last checked read. */
extern volatile uint32_t g_wflash_raw_status;
extern volatile uint32_t g_wflash_raw_dma_intr;

#endif /* WFLASH_PORT_H */
