/*
 * secoc_store.c — two-slot work-flash record behind the SecOC persistence port
 * (ADR-0024). See the header for the contract.
 *
 * Host-testable: flash only through hal_flash_if_t, no vendor headers. MISRA
 * C:2012 (target-buildable); guard clauses deviate from 15.5 single-exit
 * deliberately, matching the house style in shared/secoc.
 *
 * @impl ADR-0024 D1 : two slots, one record per node with both domains
 * @impl ADR-0024 D2 : erase older -> program body -> program mark -> read back -> return
 * @impl ADR-0024 D3 : blank-check the mark, fault-safe read of the record, CRC
 * @impl ADR-0021 D8 : durable-before-return backing for the freshness-store port
 */
#include "secoc_store.h"

#define SLOT_COUNT      2U
#define BODY_LEN        16U     /* words 0..3: seq, epochs, written, CRC */
#define CRC_LEN         12U     /* CRC-32 covers words 0..2 */
#define MARK_OFFSET     16U     /* word 4: the commit mark */
#define MARK_LEN        4U
#define NO_SLOT         0xFFU
#define WRITTEN_MASK    ((1U << SECOC_STORE_DOMAINS) - 1U)
#define CRC32_POLY      0xEDB88320U     /* IEEE 802.3, reflected */

typedef struct {
    uint32_t seq;                               /* +1 per save; higher = newer */
    uint16_t epoch[SECOC_STORE_DOMAINS];
    uint32_t written;                           /* bit n: domain n committed at least once */
} record_t;

typedef enum {
    SLOT_EMPTY   = 0,   /* mark blank: no save ever completed in this slot */
    SLOT_TORN    = 1,   /* mark or body unreadable / mid-program (power loss) */
    SLOT_CORRUPT = 2,   /* readable, but the CRC or reserved bits are wrong */
    SLOT_VALID   = 3
} slot_state_t;

static struct {
    secoc_store_cfg_t    cfg;
    secoc_store_status_t status;
    uint8_t              newest;    /* slot holding the mirror's record, NO_SLOT if factory */
    record_t             mirror;    /* RAM copy of the newest valid record */
} s_store = { .status = SECOC_STORE_BROKEN, .newest = NO_SLOT };   /* unmounted = broken */

/* ------------------------------------------------------------------ codec -- */

static void put_u32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFFU);
    b[1] = (uint8_t)((v >> 8U) & 0xFFU);
    b[2] = (uint8_t)((v >> 16U) & 0xFFU);
    b[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8U) |
           ((uint32_t)b[2] << 16U) | ((uint32_t)b[3] << 24U);
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < n; ++i)
    {
        crc ^= (uint32_t)p[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (crc >> 1U) ^ CRC32_POLY;
            }
            else
            {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/* Explicit byte offsets, little-endian, never a struct overlay. */
static void encode(const record_t *r, uint8_t out[SECOC_STORE_RECORD_LEN])
{
    put_u32(&out[0], r->seq);
    put_u32(&out[4], (uint32_t)r->epoch[0] | ((uint32_t)r->epoch[1] << 16U));
    put_u32(&out[8], r->written);
    put_u32(&out[12], crc32(out, CRC_LEN));
    put_u32(&out[MARK_OFFSET], SECOC_STORE_COMMIT_MARK);
}

static bool same_record(const record_t *a, const record_t *b)
{
    return (a->seq == b->seq) && (a->epoch[0] == b->epoch[0]) &&
           (a->epoch[1] == b->epoch[1]) && (a->written == b->written);
}

/* ------------------------------------------------------------- slot read -- */

/* @impl ADR-0024 D3 : the CPU never reads a slot; erased/torn words can't fault it */
static slot_state_t read_slot(uint32_t addr, record_t *r)
{
    const hal_flash_if_t *f = s_store.cfg.flash;

    int blank = f->blank_check(addr + MARK_OFFSET, (size_t)MARK_LEN);
    if (blank == 1)
    {
        return SLOT_EMPTY;
    }
    if (blank != 0)
    {
        return SLOT_TORN;       /* blank check itself failed: state unknown, fail closed */
    }

    uint8_t buf[SECOC_STORE_RECORD_LEN];
    if (f->read_checked(addr, buf, (size_t)SECOC_STORE_RECORD_LEN) < 0)
    {
        return SLOT_TORN;       /* uncorrectable ECC somewhere in the record */
    }
    if (get_u32(&buf[MARK_OFFSET]) != SECOC_STORE_COMMIT_MARK)
    {
        return SLOT_TORN;       /* the mark itself was mid-program */
    }
    if (get_u32(&buf[12]) != crc32(buf, CRC_LEN))
    {
        return SLOT_CORRUPT;
    }

    uint32_t epochs = get_u32(&buf[4]);
    r->seq = get_u32(&buf[0]);
    r->epoch[0] = (uint16_t)(epochs & 0xFFFFU);
    r->epoch[1] = (uint16_t)(epochs >> 16U);
    r->written = get_u32(&buf[8]);
    if ((r->written & ~WRITTEN_MASK) != 0U)
    {
        return SLOT_CORRUPT;    /* reserved bits set: not a record this code wrote */
    }
    return SLOT_VALID;
}

/* ------------------------------------------------------------------ mount -- */

static bool cfg_ok(const secoc_store_cfg_t *cfg)
{
    if ((cfg == NULL) || (cfg->flash == NULL))
    {
        return false;
    }
    const hal_flash_if_t *f = cfg->flash;
    if ((f->erase_sector == NULL) || (f->write == NULL) ||
        (f->blank_check == NULL) || (f->read_checked == NULL))
    {
        return false;
    }
    if ((f->sector_size < SECOC_STORE_RECORD_LEN) || (cfg->slot_addr[0] == cfg->slot_addr[1]))
    {
        return false;
    }
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i)
    {
        if ((cfg->slot_addr[i] % f->sector_size) != 0U)
        {
            return false;       /* each slot must be exactly one sector */
        }
    }
    return true;
}

/* @impl REQ-SECOC-006 : no valid record and not blank => BROKEN (fail closed), never cold */
secoc_store_status_t secoc_store_mount(const secoc_store_cfg_t *cfg)
{
    s_store.status = SECOC_STORE_BROKEN;
    s_store.newest = NO_SLOT;
    s_store.mirror = (record_t){ 0U, { 0U, 0U }, 0U };

    if (!cfg_ok(cfg))
    {
        return SECOC_STORE_BROKEN;
    }
    s_store.cfg = *cfg;

    record_t rec[SLOT_COUNT] = { { 0U, { 0U, 0U }, 0U }, { 0U, { 0U, 0U }, 0U } };
    slot_state_t st[SLOT_COUNT];
    for (uint32_t i = 0U; i < SLOT_COUNT; ++i)
    {
        st[i] = read_slot(cfg->slot_addr[i], &rec[i]);
    }

    /* Higher seq wins. seq is u32 and +1 per save: it can't wrap within the
     * work flash's endurance, so a plain comparison is enough. */
    if ((st[0] == SLOT_VALID) && (st[1] == SLOT_VALID))
    {
        s_store.newest = (rec[1].seq > rec[0].seq) ? 1U : 0U;
    }
    else if (st[0] == SLOT_VALID)
    {
        s_store.newest = 0U;
    }
    else if (st[1] == SLOT_VALID)
    {
        s_store.newest = 1U;
    }
    else
    {
        /* no valid record */
    }

    if (s_store.newest != NO_SLOT)
    {
        s_store.mirror = rec[s_store.newest];
        s_store.status = SECOC_STORE_OK;
    }
    else if ((st[0] == SLOT_EMPTY) && (st[1] == SLOT_EMPTY))
    {
        s_store.status = SECOC_STORE_FACTORY;
    }
    else
    {
        s_store.status = SECOC_STORE_BROKEN;
    }
    return s_store.status;
}

/* ------------------------------------------------------------------- port -- */

static bool usable(void)
{
    return s_store.status != SECOC_STORE_BROKEN;
}

static bool store_load(uint8_t domain, uint16_t *epoch)
{
    if (!usable() || (domain >= SECOC_STORE_DOMAINS) || (epoch == NULL))
    {
        return false;
    }
    if ((s_store.mirror.written & (1U << domain)) == 0U)
    {
        return false;           /* cold: this domain was never committed */
    }
    *epoch = s_store.mirror.epoch[domain];
    return true;
}

/* @impl REQ-SECOC-006 : commit is durable before it returns; a failed save leaves the old record */
static bool store_commit(uint8_t domain, uint16_t epoch)
{
    if (!usable() || (domain >= SECOC_STORE_DOMAINS))
    {
        return false;
    }
    const hal_flash_if_t *f = s_store.cfg.flash;

    record_t next = s_store.mirror;
    next.seq = s_store.mirror.seq + 1U;
    next.epoch[domain] = epoch;
    next.written |= (1U << domain);

    /* Never touch the newest valid record: write the other slot. */
    uint8_t target = (s_store.newest == 0U) ? 1U : 0U;
    uint32_t addr = s_store.cfg.slot_addr[target];
    uint8_t buf[SECOC_STORE_RECORD_LEN];
    encode(&next, buf);

    if (f->erase_sector(addr) != 0)
    {
        return false;
    }
    if (f->write(addr, buf, (size_t)BODY_LEN) != 0)
    {
        return false;
    }
    if (f->write(addr + MARK_OFFSET, &buf[MARK_OFFSET], (size_t)MARK_LEN) != 0)
    {
        return false;           /* mark last: until it lands, the slot isn't valid */
    }

    record_t back = { 0U, { 0U, 0U }, 0U };
    if ((read_slot(addr, &back) != SLOT_VALID) || !same_record(&back, &next))
    {
        return false;
    }

    /* Only now is the new value durable, so only now does RAM follow. */
    s_store.mirror = next;
    s_store.newest = target;
    s_store.status = SECOC_STORE_OK;
    return true;
}

const secoc_freshness_store_if_t g_secoc_store = {
    .load   = store_load,
    .commit = store_commit,
};
