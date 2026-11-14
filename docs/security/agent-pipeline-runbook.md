# Agent pipeline — security runbook

What to do with the pipeline's credentials, in an incident, and when the schedule stops.
Design: `docs/design/agent-pipeline.md` section 13, runtime: ADR-0026. Written for the
Challenger's points 6, 14 and 15 on #52. `host_tools/tests/test_runbook.py` checks that every
secret a workflow reads has a row in section 1.

## 1. Rotate

One row per credential. Rotate on a schedule (yearly), when someone may have seen it, and
after any incident (section 2). Always: make the new one, store it, prove a run works, then
delete the old one.

| Credential | Where it lives | Who holds it at run time | Rotate | Revoke now |
|---|---|---|---|---|
| `CLAUDE_CODE_OAUTH_TOKEN` | repo secret | the agent step of each agent job | `claude setup-token` on your PC → `gh secret set CLAUDE_CODE_OAUTH_TOKEN` (paste) → note the expiry date it shows | in your Claude account settings (exact place not verified yet); a new token does not end the old one by itself |
| `BUILDER_APP_PRIVATE_KEY` | repo secret | `builder.yml` `publish` only | github.com/settings/apps/builder-github-app → Private keys → Generate → `gh secret set BUILDER_APP_PRIVATE_KEY < key.pem` → delete the `.pem` | same page: delete the old key (tokens already minted live at most 1 hour) |
| `REVIEWER_APP_PRIVATE_KEY` | repo secret | the `post`/`publish` jobs of Inspector, Planner, Challenger | github.com/settings/apps/reviewer-github-app → as above, secret `REVIEWER_APP_PRIVATE_KEY` | as above |
| `RELEASE_DEPLOY_KEY` | `release` environment secret (not created yet) | `release.yml`, after your approval | `ssh-keygen -t ed25519 -f release_key -N ""` → repo Settings → Deploy keys → add `release_key.pub` with write → `gh secret set RELEASE_DEPLOY_KEY --env release < release_key` → delete both files | repo Settings → Deploy keys → delete |
| bench token (`traveo-bench`) | `%USERPROFILE%\.config\traveo-bench\token` on your PC | `bench_run.py`, as `GH_TOKEN` | github.com/settings/personal-access-tokens → `traveo-bench` → Regenerate → overwrite the file, strip whitespace (`docs/hardware/kits.md`) | same page → Delete |

`gh secret set` reads the value from stdin, so it never reaches the shell history. Never run a
second `gh auth login` for these: it overwrites your main token in the Windows keyring.

## 2. Incident

Signs: a pull request, branch, comment or label you did not expect from an app; a "Blocked"
call in a job's StepSecurity insights that is not `motd.ubuntu.com`; a run you cannot explain.

| Step | Do | Why first |
|---|---|---|
| 1 | `gh variable set PIPELINE_PAUSED --body true` | the Dispatcher routes nothing from now on |
| 2 | `gh run list --status in_progress` → `gh run cancel <id>` for each | a running job finishes otherwise |
| 3 | Suspend both apps: github.com/settings/installations → each app → Configure → Suspend | cuts every app token at once, including ones already minted |
| 4 | Look: the apps' recent PRs, `cr/*` branches, comments and label events; the runs' StepSecurity insights | find what was touched |
| 5 | Clean up: close PRs, delete branches, revert labels | — |
| 6 | Rotate what the incident could have reached (section 1). The OAuth token sits in every agent job: rotate it whenever an agent job is involved | — |
| 7 | Unsuspend the apps, `gh variable delete PIPELINE_PAUSED`, one tick by hand (`gh workflow run dispatcher.yml`), check #42 | prove it runs again |
| 8 | Write it down in `docs/review/` and add a negative test if one was missing | — |

## 3. The nightly schedule

GitHub turns off scheduled workflows in a public repository after **60 days without
activity** in it. Then the 01:00 tick stops, and so do its status lines on #42.

- Signal: #42 has no line from the previous night (every run writes one, also "no ready CR").
- Check: `gh workflow list --all` shows `dispatcher` as `disabled_inactivity`.
- Fix: `gh workflow enable dispatcher.yml`, then one tick by hand.
- Risk window: a long break with no commits (the job-search months, Dec 2026–Feb 2027). A
  glance at #42 at the Sunday retro catches it within a week.

## 4. The subscription token

- **Terms:** claude-code-action's setup guide names this path itself: "Pro and Max users can
  generate this by running `claude setup-token` locally", stored as `CLAUDE_CODE_OAUTH_TOKEN`.
  Re-check the current terms if the plan changes.
- **Rate limit:** the agent jobs and your own Claude sessions share one plan limit (observed
  2026-11-14: weekly at 97 % after a week of building the pipeline). What bounds the agents:
  `--max-turns` 80 (Builder), 60 (Inspector), 40 (Planner, Challenger); one CR per tick;
  no `ready` CR means no agent run. Near the limit: don't set `ready`, or pause.
- **When the limit is hit:** the agent job fails, and the Dispatcher routes it to
  `waiting-for-me` (rows 10, 10b, 17), so it shows as a referee call, never as silence. A
  failed Challenger changes no label; the status line says "Challenger failed" (row 20).
- **Alternatives:** an API key (separate billing, its own limit), or workload identity
  federation (GitHub OIDC, no stored credential at all; needs an Anthropic API organisation).

## 5. How each guarantee is observed

ADR-0026 says merges, tags, workflow changes and labels are "impossible, not only forbidden".

| Guarantee | Enforced by | Observed by |
|---|---|---|
| Only I merge | ruleset `Protect_main_branch` (PR + 6 checks, no bypass) and `Owner_approval` (1 approval) | `python host_tools/pipeline_setup.py` (dry run) reports any drift from `.github/pipeline/setup.json`; negative test: the Builder app pushes to `main` → refused (not run yet) |
| Only I (and the release key) tag | ruleset `Tags_owner_only` | same dry run; negative test: an app creates a tag → refused (not run yet) |
| No agent changes a workflow | the Builder app has no Workflows permission; the `protected-paths` check | the app's permission page; `test_protected_paths.py`; `publish` refuses protected paths (`bundle_check.py`, tests) |
| Only my labels route | the Dispatcher reads who acted; `cr_queue.py` reads who set `ready` last | `test_dispatcher.py` (someone else's label), `test_cr_queue.py`; live: the status line names the actor it ignored |
| No egress except the list | harden-runner `block`, `.github/pipeline/egress.json` | `test_egress.py`; each run's StepSecurity insights ("Blocked"); live 2026-11-14: `motd.ubuntu.com` refused in the Builder job |
| Fork PRs start no agent (D8) | `same_repo` check in `dispatcher.py` | `test_dispatcher.py`; live: needs a PR from a second account's fork (not run yet) |

The negative tests marked "not run yet" belong to the security check at setup and at every
release (design note section 13); record each run in `docs/bench/<date>/`.
