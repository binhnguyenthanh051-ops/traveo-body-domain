# scheduler — custom preemptive scheduler

Introduced in **EP.06–EP.10**. The scheduler *core* (task table, ready-queue, tick
bookkeeping, priority selection) is host-testable under CI; the context-switch assembly
and SysTick wiring are target-only and sit behind the HAL.

- `include/` — public scheduler API
- `src/`     — core logic (host-buildable) + target-only context switch (guarded)
- `tests/`   — host unit tests for the core

Status: skeleton only. First real code lands with EP.07.
