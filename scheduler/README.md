# scheduler — custom preemptive scheduler (STANDALONE DEEP-DIVE)

**Not linked into any product image.** The application runs on FreeRTOS (ADR-0005). This
module is a from-scratch preemptive scheduler built as an educational/portfolio deep-dive
(EP.06–11) and as the basis for the FreeRTOS comparison (EP.14).

The scheduler *core* (task table, ready-set, priority selection, tick/delay bookkeeping) is
host-testable under CI; the context-switch (PendSV/SysTick, PSP handling, FPU context) is
target-only behind a `shared/hal` port interface, with a host fake for tests.

- `include/` — public scheduler API
- `src/`     — core logic (host-buildable) + target-only port (guarded)
- `tests/`   — host unit tests (Unity)

See `docs/briefs/EP06-07-scheduler-brief.md`. Status: skeleton only.
