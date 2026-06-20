/*
 * sched_types.h — scheduler type definitions and compile-time configuration.
 *
 * Pure types, no functions, no hardware dependencies.
 * Included by both the scheduler core and the port interface.
 * See ADR-0009 for design rationale.
 */
#ifndef SCHED_TYPES_H
#define SCHED_TYPES_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

/* --------------------------------------------------------------------
 * Compile-time configuration — override via -D flags in the build.
 * ------------------------------------------------------------------ */

#ifndef SCHED_MAX_TASKS
#define SCHED_MAX_TASKS         8U
#endif

#ifndef SCHED_MAX_PRIORITIES
#define SCHED_MAX_PRIORITIES    32U
#endif

/* --------------------------------------------------------------------
 * Task states
 * ------------------------------------------------------------------ */

typedef enum {
    SCHED_STATE_SUSPENDED = 0,  /* not in the ready set; initial state after create */
    SCHED_STATE_READY,          /* eligible to run                                  */
    SCHED_STATE_RUNNING,        /* currently executing (exactly one task at a time)  */
    SCHED_STATE_BLOCKED         /* waiting on sched_delay(); delay_ticks counts down */
} sched_task_state_t;

/* --------------------------------------------------------------------
 * Task entry-point signature
 * ------------------------------------------------------------------ */

typedef void (*sched_task_fn_t)(void *arg);

/* --------------------------------------------------------------------
 * Task control block (TCB)
 *
 * sp MUST be the first field — the PendSV handler indexes into the
 * TCB at offset 0 to load/store the stack pointer.
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t           *sp;           /* saved stack pointer (offset 0)   */
    uint8_t             priority;     /* 0 = highest                     */
    sched_task_state_t  state;
    uint32_t            delay_ticks;  /* ticks remaining; 0 = not delayed */
    uint32_t           *stack_base;   /* bottom of statically allocated stack */
    uint32_t            stack_size;   /* in 32-bit words                 */
    sched_task_fn_t     entry;        /* task entry point                */
    void               *arg;         /* argument passed to entry        */
    uint8_t             id;           /* index in the task table         */
} sched_tcb_t;

/* --------------------------------------------------------------------
 * Return / error codes
 * ------------------------------------------------------------------ */

typedef enum {
    SCHED_OK = 0,
    SCHED_ERR_FULL,             /* task table is full                    */
    SCHED_ERR_NULL,             /* NULL pointer argument                */
    SCHED_ERR_PRIORITY,         /* priority >= SCHED_MAX_PRIORITIES     */
    SCHED_ERR_STACK             /* stack too small or NULL               */
} sched_err_t;

#endif /* SCHED_TYPES_H */
