#!/usr/bin/env python3
"""bench_run.py -- the scripted bench run for one CR (design note sections 2-3, build step 8).

    python host_tools/bench_run.py --cr <n>        (from PowerShell, so `bash` is ModusToolbox's)

A script, not an agent: its only inputs are a CR number and the PR's head commit. It
  1. checks the CR is `needs-bench` and has an open PR, and reads that PR's head commit;
  2. puts the bench clone (a second clone that keeps only the ModusToolbox library caches) on
     exactly that commit, and copies in the local bench.env and dev signing key;
  3. starts both captures, flashes FBL, app and Node B with the zz_build scripts, resets A and B,
     and stops the captures, following the bench rule in docs/workflow.md (no erase, no SFlash,
     eFuse or lifecycle writes, and a failed flash stops the session);
  4. judges with host_tools/bench_check.py: the verdict is its exit code;
  5. posts the `bench` commit status and a PR comment, with the bench's own token (a separate gh
     login in GH_CONFIG_DIR, see docs/hardware/kits.md), and leaves the captures in
     docs/bench/<date>/cr-<n>/ of this checkout.
The Dispatcher routes the status (rows 8-9). Exit code: 0 pass, 1 fail, 2 refused or unusable.
"""
import argparse
import datetime
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
LOCAL_FILES = ("bench.env", "ec_p256_dev_private.pem")
FLASH = ("zz_build_gateway_fbl.sh", "zz_build_gateway_app.sh", "zz_build_actuator_app.sh")
SETTLE_S = 4          # between resets, as in the bench-test skill
CAPTURE_TAIL_S = 6    # after the last reset, so every boot is in the capture


def _status(gh, sha, state, why):
    gh(["api", "--method", "POST", f"repos/{{owner}}/{{repo}}/statuses/{sha}",
        "-f", f"state={state}", "-f", "context=bench", "-f", f"description={why[:139]}"])


def run_bench(cr, *, gh, run, capture, wait, clone, source, today, port_a="COM4", port_b="COM6"):
    clone, source = pathlib.Path(clone), pathlib.Path(source)
    labels = {l["name"] for l in json.loads(gh(["api", f"repos/{{owner}}/{{repo}}/issues/{cr}/labels"]))}
    if "needs-bench" not in labels:
        print(f"refused: #{cr} is not needs-bench (labels: {sorted(labels)})")
        return 2
    prs = json.loads(gh(["pr", "list", "--head", f"cr/{cr}", "--state", "open",
                         "--json", "number,headRefOid"]))
    if not prs:
        print(f"refused: #{cr} has no open pull request from cr/{cr}")
        return 2
    pr, sha = prs[0]["number"], prs[0]["headRefOid"]
    _status(gh, sha, "pending", f"bench run started for #{cr}")

    record = source / "docs" / "bench" / today / f"cr-{cr}"
    record.mkdir(parents=True, exist_ok=True)
    rc, why = 1, "bench run did not finish"
    handles = []
    try:
        if not clone.exists():
            if run(["git", "clone", "--quiet", str(source), str(clone)], cwd=source.parent):
                raise RuntimeError("could not create the bench clone")
        for cmd in (["git", "fetch", "--quiet", "origin", f"cr/{cr}"],
                    ["git", "checkout", "--quiet", "--detach", sha],
                    ["git", "clean", "-ffdq", "-e", "libs/", "-e", "mtb_shared/"]):
            if run(cmd, cwd=clone):
                raise RuntimeError(f"bench clone step failed: {' '.join(cmd)}")
        clone.mkdir(parents=True, exist_ok=True)
        for name in LOCAL_FILES:
            shutil.copyfile(source / name, clone / name)

        handles = [capture(port_a, record / "nodeA.bin"), capture(port_b, record / "nodeB.bin")]
        for script in FLASH:
            if run(["bash", f"./{script}", "program"], cwd=clone):
                raise RuntimeError(f"flash failed: {script} (session stopped)")
        for node in ("A", "B"):
            if run(["bash", "./tools/bench/reset_node.sh", node], cwd=clone):
                raise RuntimeError(f"reset failed: node {node}")
            wait(SETTLE_S)
        wait(CAPTURE_TAIL_S)
        for h in handles:
            h.stop()
        handles = []

        check = [sys.executable, str(clone / "host_tools" / "bench_check.py"),
                 "--node", f"A={record / 'nodeA.bin'}", "--node", f"B={record / 'nodeB.bin'}"]
        expect = clone / "bench" / "expect.json"
        if expect.is_file():
            check += ["--expect", str(expect)]
        rc = run(check, cwd=clone)
        why = {0: "bench pass", 1: "bench fail"}.get(rc, "bench input unusable")
        rc = 0 if rc == 0 else 1
    except (RuntimeError, OSError) as err:
        rc, why = 1, str(err)
    finally:
        for h in handles:
            h.stop()

    state = "success" if rc == 0 else "failure"
    _status(gh, sha, state, f"{why} (#{cr}, {sha[:7]})")
    gh(["pr", "comment", str(pr), "--body-file", "-"],
       stdin=f"## Bench run: **{state}**\n\n{why}. Commit `{sha[:7]}`; captures in "
             f"`docs/bench/{today}/cr-{cr}/` on the owner's PC. The verdict is "
             f"`bench_check.py`'s exit code.")
    print(f"bench: {state} ({why})")
    return rc


def run_cmd(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd).returncode


class _Capture:
    def __init__(self, port, path):
        self.proc = subprocess.Popen(
            [sys.executable, "-u", str(REPO / "host_tools" / "logview" / "logview.py"),
             "--port", port, "--raw", str(path), "--no-colour"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            self.proc.wait(timeout=10)


def run_gh(args, stdin=None):
    env = dict(os.environ, GH_CONFIG_DIR=os.environ.get(
        "BENCH_GH_CONFIG_DIR", str(pathlib.Path.home() / ".config" / "gh-bench")))
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True,
                          encoding="utf-8", input=stdin, env=env, cwd=REPO).stdout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cr", required=True, type=int)
    parser.add_argument("--clone", default=str(REPO.parent / (REPO.name + "-bench")),
                        help="the bench clone (default: a sibling folder named <repo>-bench)")
    parser.add_argument("--port-a", default="COM4")
    parser.add_argument("--port-b", default="COM6")
    args = parser.parse_args(argv)
    return run_bench(args.cr, gh=run_gh, run=run_cmd, capture=_Capture, wait=time.sleep,
                     clone=args.clone, source=REPO, today=datetime.date.today().isoformat(),
                     port_a=args.port_a, port_b=args.port_b)


if __name__ == "__main__":
    sys.exit(main())
