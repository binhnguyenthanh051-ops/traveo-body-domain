/*
 * sysmgr.h — resource & lifecycle manager (ADR-0015).
 *
 * Drives an author-ordered resource table through init -> ready, enforcing
 * each resource's declared dependencies structurally (a resource's init() is
 * never called until its deps report READY -- consumers never poll another
 * resource's readiness themselves, D5). Tick-based, non-blocking, same idiom
 * as shared/boot's fbl_dwell_for_tool.
 *
 * No port of its own: now_ms is passed in by the caller, and the
 * SYSTEM_RESET fail policy escalates through a caller-supplied callback --
 * this module has no hardware dependency and no image-specific knowledge.
 */
#ifndef SYSMGR_H
#define SYSMGR_H

#include "sysmgr_types.h"
#include <stdbool.h>

typedef void (*sysmgr_reset_fn_t)(void);

/* Bind the resource table (author-ordered -- dependencies must appear before
 * their dependents) and the reset callback used by SYSMGR_FAIL_SYSTEM_RESET
 * entries. table/count are owned by the caller (composition root). Call once
 * at startup, before the first sysmgr_tick(). */
void sysmgr_init(const sysmgr_resource_t *table, size_t count,
                  sysmgr_reset_fn_t reset_fn);

/* Advance whichever resource is currently INITIALIZING (checks poll_ready()/
 * timeout), then start the next one in table order once its deps are READY.
 * Call once per FBL super-loop iteration. */
void sysmgr_tick(uint32_t now_ms);

sysmgr_res_state_t sysmgr_res_state(sysmgr_res_id_t id);

/* Convenience: true once every resource in the table is READY or DEGRADED
 * (i.e. the table has finished settling, whether or not every resource
 * succeeded). */
bool sysmgr_settled(void);

#endif /* SYSMGR_H */
