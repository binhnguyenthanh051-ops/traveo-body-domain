# Security check — the runbook's negative tests, 2026-11-14

**Run:** `security-check.yml`, run [43572295509](https://github.com/binhnguyenthanh051-ops/traveo-body-domain/actions/runs/43572295509)
on `main`, started by the owner; the `release` environment approved by the owner.
**Why:** owner's review of #52, point 4: ADR-0026 is accepted only after these ran.
**Verdict:** **PASS**, 8 of 8 checks. Each verdict is the job's exit code; GitHub's own
refusal message is quoted for each refusal.

| Job | Identity | Action | Expected | Result | GitHub's answer |
|---|---|---|---|---|---|
| `app-refused` | Builder app token (contents, issues, PRs: write) | push an ordinary branch (control) | allowed | allowed | — |
| | | push to `main` | refused | refused | `push declined due to repository rule violations` |
| | | create a tag | refused | refused | `push declined due to repository rule violations` |
| | | push a workflow change | refused | refused | `refusing to allow a GitHub App to create or update workflow .github/workflows/security-check.yml without workflows permission` |
| `egress-refused` | a job under harden-runner `block` (github.com only) | reach github.com (control) | allowed | allowed | — |
| | | reach example.com | refused | refused | `curl: (7) Failed to connect to example.com port 443 after 1 ms` |
| `deploy-key-tag` | release deploy key (write) | create a tag | allowed | allowed | — |
| | | delete that tag | allowed | allowed | — |
| | | push to `main` | refused | refused | `push declined due to repository rule violations` |

Afterwards: no `security-check*` tag and no `security-check/*` branch on the remote
(`git ls-remote`), and `main` unchanged by the run.

**What this shows:** `Protect_main_branch` holds against both the Builder app and the deploy
key; `Tags_owner_only` refuses the app and lets the deploy key through, as designed; the
Builder app cannot touch workflows; egress `block` refuses an unlisted host.

**Not covered:** a pull request from a fork (ADR-0026 D8), which needs a second GitHub
account (future, agreed with the owner); the Reviewer app (it holds Contents: read only, so
the Builder app is the stronger case).
