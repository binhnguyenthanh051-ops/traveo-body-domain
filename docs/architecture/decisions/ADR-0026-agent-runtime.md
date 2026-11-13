# ADR-0026: Agent runtime — GitHub Actions, two GitHub Apps, rules the platform enforces

**Status:** proposed · 2026-11-13 · build step 4 of [the agent pipeline](../../design/agent-pipeline.md)

**Summary.** The pipeline's agents run as GitHub Actions jobs with `claude-code-action`, and
act on GitHub as two GitHub Apps of their own, never as the owner. That separate identity is
what lets GitHub itself enforce "only the owner merges", "only the owner tags" and "a label
counts only when the owner set it". The agent's own job holds no token that can write to
GitHub.

## 1. Context

The design note says "never merges" is enforced by branch protection, not by a prompt
(section 7), and that agents follow only the owner's issues and comments. Both need GitHub to
tell the owner apart from an agent. The step 3 walkthrough showed the gap: every stage acted
as the owner's account.

## 2. Decisions

### D1. Runtime: GitHub Actions with `claude-code-action`, pinned to a commit

`anthropics/claude-code-action@4d75e5f` (v1.0.275), authenticated with the owner's Claude
subscription (`CLAUDE_CODE_OAUTH_TOKEN`). Triggers are workflow events: a nightly `schedule`
for the Builder (`builder.yml`), and later events for the other agents.

### D2. Identity: two GitHub Apps owned by the owner

| App | Permissions | Used by |
|---|---|---|
| Builder | Contents RW, Issues RW, Pull requests RW, Metadata R. **No Workflows** | the `publish` job of `builder.yml` |
| Reviewer | Issues RW, Pull requests RW, Contents R, Metadata R | Inspector, Planner, Challenger (steps 5–7) |

Without the Workflows permission, GitHub refuses any app push that changes
`.github/workflows/`. Tokens are minted per job by `actions/create-github-app-token`, scoped
down again per job, and expire within an hour.

### D3. The agent's job holds no GitHub write token

`builder.yml` has four jobs: `pick` (reads the queue), `build` (the agent; only a read-only
`GITHUB_TOKEN` and the OAuth token), `publish` (a scripted push and pull request, with the
Builder App token) and `report` (one status line per run). The agent's commits reach
`publish` as a git bundle artifact.

### D4. Rules GitHub enforces

| Ruleset | Rules | Bypass |
|---|---|---|
| `Protect_main_branch` (existing) | PR required, 5 required checks, no force push, no deletion, linear history | **nobody**: the checks apply to the owner too |
| `Owner_approval` | 1 approving review, stale approvals dismissed, last push approved | owner (admin role), **only through a PR merge** |
| `Tags_owner_only` | create, update and delete of any tag | owner, and GitHub Actions for `release.yml` |

Rulesets stack, so the owner's bypass of `Owner_approval` never skips `Protect_main_branch`.
Every agent job keeps `GITHUB_TOKEN` at `contents: read`, so the GitHub Actions bypass on tags
serves `release.yml` only. Applied by `host_tools/pipeline_setup.py --apply`.

### D5. A label counts only when the owner added it

An app token can add `ready` or `protected-change-approved`. So `host_tools/cr_queue.py` reads
the issue timeline: the Builder takes an issue only when the owner added `ready` last, and the
`protected-paths` check accepts the approval label only when the owner added it last.

### D6. Builder tool allowlist

```
Read, Edit, Write, Glob, Grep,
Bash(make test), Bash(make lint), Bash(python -m pytest host_tools:*),
Bash(python host_tools/trace_report.py:*), Bash(python host_tools/gen_log_events.py --check),
Bash(git status), Bash(git diff:*), Bash(git log:*), Bash(git show:*),
Bash(git add:*), Bash(git commit:*)
```

Denied: `WebFetch`, `WebSearch`, and every Bash command not listed (`git push`, `gh`, `curl`,
`pip`). `make` and `pytest` run code the agent writes, so this list stops casual misuse, not a
determined agent: D3 and D7 are the boundary.

### D7. Egress: audited now, blocked after measurement

`step-security/harden-runner@1d88384` (v2.22.4) runs first in every job, in `audit` mode. The
endpoints the first runs reach become the `allowed-endpoints` list, and a follow-up CR
switches to `egress-policy: block` with `disable-sudo-and-containers`. No list is guessed.

### D8. Fork pull requests never start an agent

Agent jobs on pull request events (from step 5) start only when
`head_repository.full_name == github.repository`, and never use `pull_request_target`.
`workflow_run`, which runs with secrets even for fork PRs, gets the same check.

## 3. Secrets on the runners, and what a leak allows

| Secret | Present in | A leak allows |
|---|---|---|
| `CLAUDE_CODE_OAUTH_TOKEN` | `build` (the agent) | model use on the owner's subscription, until revoked |
| Builder App token | `publish` only, ≤ 1 h | push branches, open PRs, edit issues and labels; no tags, no workflows, no merge to `main` |
| Reviewer App token | reviewer jobs only, ≤ 1 h | comments and labels |
| App private keys | the token-minting step only | minting new tokens: the crown jewel |
| `GITHUB_TOKEN` | every job | at most `issues: write` in `pick` and `report`; read-only elsewhere |

What remains open: a release on an **existing** tag cannot be blocked by permissions
(Contents write allows it). The release check (step 9) flags any release not made by
`release.yml`.

## 4. Alternatives

- **Claude Code cloud scheduled tasks.** Better sandbox: a network allowlist, and credentials
  kept outside the VM. Rejected for now: a session acts as the owner, so "only the owner
  merges", "only the owner's comments" and "only the owner's labels" cannot be enforced.
  Revisit if cloud sessions get an identity of their own.
- **The official Claude GitHub App.** Its permission set includes Workflows write and
  Administration read, and GitHub grants the whole set. Rejected for agents; custom apps keep
  only what each role needs.
- **One app for every agent.** Simpler, but a read-only role would hold push rights.

## 5. Consequences

- (+) The design note's "impossible, not only forbidden" holds for merges, tags, workflow
  changes and labels.
- (−) The owner approves each agent pull request (one click) before merging.
- (−) Setup the owner must do, since it creates identities and secrets: create the two apps,
  set `BUILDER_APP_ID` (variable), `BUILDER_APP_PRIVATE_KEY` and `CLAUDE_CODE_OAUTH_TOKEN`
  (secrets).
- (−) GitHub disables scheduled workflows after 60 days without repository activity.

## 6. Not verified yet

- The first `builder.yml` run: `claude-code-action` with a read-only `github_token` in
  automation mode, and the bundle hand-over between jobs.
- The ruleset actor IDs (admin role `5`, GitHub Actions `15368`) after `--apply`, read back
  from the API.
