---
description: "Run all host-side unit tests (messages, scheduler, boot) and cppcheck static analysis. Use when: run tests, run lint, make test, make lint, check tests, run checks."
agent: "agent"
tools: [run_in_terminal]
---

Run the full host-side test suite and static analysis for this repo.

Use the following command (MSYS2 bash with correct PATH):

```
C:\msys64\usr\bin\bash.exe -lc "cd 'c:/00_Projects/00_Portfolio/00_TraveoBody/traveo-body-domain' && export PATH='/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH' && make test 2>&1 && make lint 2>&1"
```

After running, report:
- Pass/fail count per module (messages, scheduler, boot)
- Any test failures with the exact assertion message and line number
- Lint findings, or confirm clean if none
- Overall status: all green, or what needs fixing
