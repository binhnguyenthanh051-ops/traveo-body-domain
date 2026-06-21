# scheduler — custom preemptive scheduler (STANDALONE DEEP-DIVE)

**Not linked into any product image.** The application runs on FreeRTOS (ADR-0005). This
module is a from-scratch preemptive scheduler built as an educational/portfolio deep-dive
(EP.06–11) and as the basis for the FreeRTOS comparison (EP.14).

The scheduler *core* (task table, ready-set, priority selection, tick/delay bookkeeping) is
host-testable under CI; the context-switch (PendSV/SysTick, PSP handling, FPU context) is
target-only behind a `shared/hal` port interface, with a host fake for tests.

- `include/` — public scheduler API (`sched.h`, `sched_types.h`, `sched_port.h`)
- `src/`     — core logic (`sched.c`, host-buildable) + target-only port
              (`sched_port_arm.c`, guarded by `SCHED_PORT_ARM`, PendSV/SysTick TODO)
- `tests/`   — host unit tests (Unity) + host fake port (`sched_port_fake.c`)

Design: ADR-0009 (reviewed in `docs/review/ADR-0009-review.md`).
See `docs/briefs/S1-S2-scheduler-brief.md`.

**Status (EP.06–07):** core implemented and host-tested green — static task
creation, fixed-priority preemptive selection (priority bitmap + `__builtin_ctz`),
built-in idle task, tick-driven delay/unblock, equal-priority round-robin,
BASEPRI critical sections. The Cortex-M context switch (PendSV/SysTick, PSP/FPU)
is stubbed in `sched_port_arm.c` pending board bring-up; the core is fully
testable without hardware.
