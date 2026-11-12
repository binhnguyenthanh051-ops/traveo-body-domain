# host_tools — PC-side tooling and integration tests

Python 3.11+. CAN tooling, a UDS client, and integration test scripts that drive the
protocol logic and assert responses (wired into CI in **EP.20**).

- `requirements.txt` — dependencies (e.g. python-can, cantools)
- tests run with `pytest` (CI calls `pytest host_tools`)
- `pipeline_setup.py` — creates or updates the agent pipeline's GitHub labels and milestones
  from `.github/pipeline/setup.json`. Dry run by default, `--apply` to change GitHub; it never
  deletes. See `docs/design/agent-pipeline.md` section 4.
