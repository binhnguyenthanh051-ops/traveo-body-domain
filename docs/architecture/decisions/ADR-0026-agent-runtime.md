# ADR-0026: Agent runtime — GitHub Actions, two GitHub Apps, rules the platform enforces

**Status:** accepted · 2026-11-14 (proposed 2026-11-13) · build step 4 of [the agent pipeline](../../design/agent-pipeline.md)

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
subscription (`CLAUDE_CODE_OAUTH_TOKEN`). No agent has a trigger of its own: the Dispatcher
(`dispatcher.yml`, design note section 3, added 2026-11-13) owns every trigger and calls each
agent's workflow as a job.

### D2. Identity: two GitHub Apps owned by the owner

| App | Permissions | Used by |
|---|---|---|
| Builder | Contents RW, Issues RW, Pull requests RW, Metadata R. **No Workflows** | the `publish` job of `builder.yml` |
| Reviewer | Issues RW, Pull requests RW, Contents R, Metadata R | Inspector, Planner, Challenger (steps 5–7). Posts **comments only, never an approval**: an app approval would satisfy `Owner_approval`, and the Builder app could then merge |

Without the Workflows permission, GitHub refuses any app push that changes
`.github/workflows/`. Tokens are minted per job by `actions/create-github-app-token`, scoped
down again per job, and expire within an hour.

### D3. The agent's job holds no GitHub write token

`builder.yml` has two jobs: `build` (the agent; only a read-only `GITHUB_TOKEN` and the OAuth
token) and `publish` (a scripted push, and the pull request on a first build, with the Builder
App token). The agent's commits reach `publish` as a git bundle artifact. Picking the CR,
moving labels and the status line belong to the Dispatcher.

### D4. Rules GitHub enforces

| Ruleset | Rules | Bypass |
|---|---|---|
| `Protect_main_branch` (existing) | PR required, 5 required checks, no force push, no deletion, linear history | **nobody**: the checks apply to the owner too |
| `Owner_approval` | 1 approving review, stale approvals dismissed, last push approved | owner (admin role), **only through a PR merge** |
| `Tags_owner_only` | create, update and delete of any tag | owner, and deploy keys |

Rulesets stack, so the owner's bypass of `Owner_approval` never skips `Protect_main_branch`.
Applied by `host_tools/pipeline_setup.py --apply`.

**Release tags (revised 2026-11-13).** The first plan let GitHub Actions bypass the tag rule
for `release.yml`. GitHub refused it: the GitHub Actions app can be a bypass actor only on an
organization repo (HTTP 422). So `release.yml` pushes the tag over SSH with a **deploy key**,
stored only in a `release` environment that needs the owner's approval for each run and starts
only from `main`. No agent job names that environment, so none gets the key. The DeployKey
bypass covers every deploy key with write access, so `pipeline_setup.py` fails when more than
one exists. Alternative: the owner pushes the tag by hand and `release.yml` only creates the
Release. It is simpler, but then the tag exists before the checks pass.

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

*Done 2026-11-14.* The lists were measured from StepSecurity insights of audit runs on
2026-11-13 and live in `.github/pipeline/egress.json`, one per job, each host with its reason.
The Inspector had no insights; its list is derived from the same steps in the Builder. The
Builder's build job needs `sudo apt-get` for cppcheck, so instead of
`disable-sudo-and-containers` it does the same thing itself (truncates
`/etc/sudoers.d/runner`, closes the Docker and containerd sockets) right after its tools step,
before any repository or agent code runs, and fails if sudo or Docker still work.

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
| `RELEASE_DEPLOY_KEY` | the `release` environment only, after the owner approves the run | pushing any tag or branch the rulesets allow a deploy key: every tag, but not `main` |
| `GITHUB_TOKEN` | every job | at most `issues: write` in `pick` and `report`; read-only elsewhere |

What remains open: a release on an **existing** tag cannot be blocked by permissions
(Contents write allows it). The release check (step 9) flags any release not made by
`release.yml`.

What remains open inside the egress allowlist (D7): the allowed hosts (`github.com`,
`api.github.com`, `pypi.org`, and the others in `.github/pipeline/egress.json`) accept requests
with **any** credentials. An agent steered by injected text could send data there with an
attacker's own token (a gist, an issue in another repository, a package upload), and the
allowlist would let it through. The allowlist only narrows the channel. The real control is
the input: the agents act only on text the owner wrote or approved, pinned at the owner's
`ready` (D5 and `cr_queue.py`: no edit by anyone else after it).

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
  set `BUILDER_APP_ID`, `REVIEWER_APP_ID` and `REVIEWER_APP_SLUG` (variables),
  `BUILDER_APP_PRIVATE_KEY`, `REVIEWER_APP_PRIVATE_KEY` and `CLAUDE_CODE_OAUTH_TOKEN`
  (secrets); create one deploy key with write access and store its private half as the
  `release` environment secret `RELEASE_DEPLOY_KEY`. Rotation and revocation of each, and the
  incident steps: `docs/security/agent-pipeline-runbook.md`.
- (−) GitHub disables scheduled workflows after 60 days without repository activity. The
  missing nightly line on #42 is the signal; the runbook (section 3) has the check and the fix.
- (−) The agents share the owner's subscription limit with the owner's own sessions. A hit
  limit fails the agent job and routes it to `waiting-for-me`; runbook section 4.
- How each "impossible" claim is observed (merge, tag, workflow change, label, egress, fork):
  runbook section 5.

## 6. Not verified yet

- ~~The first `builder.yml` run~~: verified on #43 (2026-11-13) and #50 (2026-11-14, under
  egress `block`): `claude-code-action` with a read-only `github_token`, and the bundle
  hand-over between jobs.
- ~~The negative tests of runbook section 5~~: passed 2026-11-14, 8 of 8
  (`security-check.yml`, run 43572295509, `docs/bench/2026-11-14/security-check.md`): the
  Builder app is refused `main`, a tag and a workflow change; egress `block` refuses an
  unlisted host; the release deploy key creates and deletes a tag and is refused `main`.
- `Owner_approval` was applied and read back on 2026-11-13 (admin role `5`, PR-only bypass).
  `Tags_owner_only` (create, update, delete of every tag; bypass: DeployKey and admin) was read
  back active on 2026-11-14, and the deploy-key tag push ran once in the same check.
- Still open: a pull request from a fork (D8), which needs a second GitHub account (future).
- The owner accepts this ADR after the negative tests and one deploy-key tag push have run
  (owner's review of #52, 2026-11-14). Both have now run.
