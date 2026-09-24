# Claude mistakes log

My running notes on where Claude Code slipped — lost context, made a wrong assumption,
overstepped a project convention, or fumbled the tooling. Kept for my reference (and, per
`docs/workflow.md`, "rejected AI suggestions and why are good blog material" — so is this).

Not every correction belongs here — only genuine mistakes, not normal design iteration.

## How to read an entry

- **What happened** — the observable slip.
- **Category** — convention-miss / wrong-assumption / context-loss / slip.
- **Root cause** — why it happened (the honest version, not "the AI is imperfect").
- **Prevention** — what should stop it next time (usually a memory or a doc to read first).

---

## Entries

### 2026-07-25 — Ran the lint/test pipeline, which is Copilot/CI's job
- **What happened:** during the M5 SecOC failing-test step, Claude built and ran the Unity
  binaries (and invoked `make`) to confirm the red/green state. `docs/workflow.md` assigns
  running Unity test cases to GitHub Copilot / CI; Claude owns contracts, ADRs, requirements,
  and *review*.
- **Category:** convention-miss.
- **Root cause:** never opened `docs/workflow.md`, even though `CLAUDE.md` explicitly points to
  it ("See `docs/workflow.md` for the AI tool split"). Skipped a referenced doc, then acted on
  an assumed division of labor.
- **Prevention:** memory `workflow-tool-division` added; treat `docs/workflow.md` as binding at
  the start of repo work. Deliver test scaffolds as drafts and stop before executing them.

### 2026-07-25 — Doubled path segment (`00_Projects\00_Projects\...`) — RECURRED
- **What happened:** a tool call used `C:\00_Projects\00_Projects\00_Portfolio\...` (the
  `00_Projects` segment duplicated). First time (a `Write`, the freshness-store fake) it silently
  created a stray tree that had to be deleted; it **happened again the same session** on an `Edit`
  (can_task.c) — that one failed loudly ("File does not exist") and was retried correctly, no
  stray created.
- **Category:** slip (path handling) — recurring, so a pattern, not a one-off.
- **Root cause:** hand-retyping the long absolute Windows prefix
  (`C:\00_Projects\00_Portfolio\00_TraveoBody\traveo-body-domain`) instead of copying it verbatim.
- **Prevention:** never hand-type that prefix — copy it exactly from the working-directory line
  (or use repo-relative paths). Note the asymmetry: `Edit` fails loudly on a bad path (safe),
  `Write` creates silently (dangerous) — so scrutinise the prefix specifically on `Write`.

<!-- Add new entries above this line, newest first. -->
