/*
 * sysmgr_types.h — resource descriptor and state types, no functions
 * (ADR-0015).
 *
 * A resource entry mixes data (deps, timeout, retry count) with embedded
 * callbacks, so per the naming convention (docs/coding-standard.md) it stays
 * a plain _t, not _if_t -- _if_t is reserved for structs that are only
 * function pointers.
 */
#ifndef SYSMGR_TYPES_H
#define SYSMGR_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifndef SYSMGR_MAX_DEPS
#define SYSMGR_MAX_DEPS   4U
#endif

/* A resource ID is just a small opaque key sysmgr compares for equality and
 * dependency lookups -- it carries no meaning of its own. Deliberately not a
 * fixed enum here: baking FBL-specific names (CAN, FLASH, ...) into this
 * shared, image-agnostic module would be exactly the kind of resource this
 * module exists to avoid coupling. Each image's own composition root defines
 * its own named constants (e.g. `#define RES_CAN 0` in the FBL's diag_init.c)
 * over this same underlying type. */
typedef uint8_t sysmgr_res_id_t;

typedef enum {
    SYSMGR_RES_UNINIT = 0,
    SYSMGR_RES_INITIALIZING,
    SYSMGR_RES_READY,
    SYSMGR_RES_DEGRADED,   /* SYSMGR_FAIL_DEGRADE outcome -- unavailable, no reset */
    SYSMGR_RES_FAILED      /* transient, en route to a retry or SYSTEM_RESET */
} sysmgr_res_state_t;

/* Required on every resource entry -- no implicit default (ADR-0015 D6/D7).
 *
 * A resource always retries in place, up to max_retries, regardless of this
 * field -- on_exhausted governs only what happens once those retries are
 * used up:
 *   RETRY_INPLACE: there is no terminal action -- keep retrying indefinitely
 *                  (max_retries is not a hard cap for this policy).
 *   SYSTEM_RESET:  escalate via the reset callback, exactly once (a resulting
 *                  real reset restarts everything; no further in-process
 *                  retry follows).
 *   DEGRADE:       mark DEGRADED and stop; dependents cascade to DEGRADED
 *                  too (a hard dependency requires READY, not DEGRADED).
 */
typedef enum {
    SYSMGR_FAIL_RETRY_INPLACE = 0,
    SYSMGR_FAIL_SYSTEM_RESET,
    SYSMGR_FAIL_DEGRADE
} sysmgr_fail_policy_t;

typedef struct {
    sysmgr_res_id_t id;
    sysmgr_res_id_t deps[SYSMGR_MAX_DEPS];
    uint8_t         dep_count;

    int  (*init)(void);         /* non-blocking; 0 = started ok, non-zero = failed now */
    int  (*poll_ready)(void);   /* NULL if init() is synchronous; else 0 = ready */
    void (*deinit)(void);

    uint32_t              init_timeout_ms;  /* 0 = no timeout */
    uint8_t                max_retries;
    sysmgr_fail_policy_t   on_exhausted;
} sysmgr_resource_t;

#endif /* SYSMGR_TYPES_H */
