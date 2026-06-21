# Git rules

Lightweight conventions for this repo. It is solo-developed, but it *is* the portfolio —
a hiring manager may read the history and branch hygiene as a work sample, so keep it clean.
Not heavy process; just enough to look (and be) professional.

## Branches

- **`main` is always green** — builds, CI passing, never knowingly broken.
- Work happens on **short-lived branches**, merged into `main` via Pull Request (yes, even
  self-merged — the PR runs CI before merge and the diff/description becomes a mini design note).
- Naming: `type/short-slug`, mapping to the roadmap where possible.

  | Prefix | For | Example |
  |--------|-----|---------|
  | `feat/` | new functionality | `feat/m1-bootloader-handshake`, `feat/trackS-scheduler-core` |
  | `fix/`  | bug / review-finding fix | `fix/adr0009-exc-return-f1` |
  | `docs/` | docs, ADRs, blog drafts | `docs/architecture-overview` |
  | `refactor/` | restructuring, no behaviour change | `refactor/shared-can-config` |
  | `test/` | tests only | `test/scheduler-readyset` |
  | `chore/` / `ci/` | tooling, build, CI | `ci/add-cppcheck-job` |

- Delete branches after merge. Don't let stale branches pile up.

## Commits — Conventional Commits

Format: `type(scope): summary` — imperative mood, summary ≤ ~72 chars.

- Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `ci`, `perf`, `build`.
- `scope` is optional but useful: the module or milestone (`scheduler`, `bootloader`, `m1`).
- Reference review findings where relevant (`(F1)`, `(F2)`).
- Body (optional): the *why*, not the *what* — the diff already shows the what.

Examples:
```
feat(scheduler): add idle task to avoid ctz(0) UB (F2)
fix(scheduler): correct initial EXC_RETURN to standard frame (F1)
docs: capture ADR-0009 scheduler review
ci: require host build + static analysis before merge
```

## Tags — one per milestone

Annotated tags mark publishable milestones (M0–M6) and pair with blog posts.
```
git tag -a m1-bootloader -m "FBL boots, verifies, jumps to app"
git push origin m1-bootloader
```
A blog post can then say "code as of this article: tag `m1-bootloader`".

## Pull request hygiene

- One logical change per PR; keep them small and reviewable.
- PR description: what changed, why, and how it was verified (`make test` / `make lint` green,
  on-target tested, etc.). This is reusable blog material.
- Let CI pass before merging. Squash or rebase-merge to keep `main` history linear and readable.

## Don't rewrite published history

- Never force-push over commits that are already pushed — especially anything tagged or linked
  from a blog post. Readers following along will get broken references.
- Local-only branches: rebase/clean up freely *before* the first push.

## Secrets — hard rule

- **Never commit keys, certificates, signing material, or device secrets.** `.gitignore`
  blocks the common patterns, but the rule is on you, not the tool.
- If a real secret is ever pushed, treat it as **compromised** — rotate/revoke it. Deleting the
  commit is not enough; it lives in history and any clone. For a repo that *discusses* secure
  boot, a leaked key in history is the worst possible look.

## Branch protection (configured on GitHub, see README/below)

`main` requires the CI status checks to pass before merge, so a red build can't land.
