#!/usr/bin/env python3
"""bundle_check.py -- what the Builder's publish job checks in the agent's bundle before it pushes.

    python host_tools/bundle_check.py --repo . --bundle cr.bundle --branch cr/<n> --base <sha>

The agent's commits reach the publish job (which holds the Builder App token) as a git bundle.
The agent wrote that bundle, so publish treats it as untrusted input (Challenger finding on the
agent-runtime design, 2026-11-13). The bundle must:
  - verify (`git bundle verify`), against the commits this repo already has;
  - carry exactly one ref, refs/heads/<branch>;
  - fast-forward from <base>: the commit the build started on;
  - hold at least one and at most --max-commits commits, none of them a merge;
  - stay under --max-diff-bytes of diff, with no file over --max-file-bytes;
  - add no symlink and no submodule.
Protected paths are checked separately (protected_paths.py). Exit code: 0 ok, 1 refused.
"""
import argparse
import pathlib
import subprocess
import sys

MAX_COMMITS = 30
MAX_DIFF_BYTES = 1_000_000
MAX_FILE_BYTES = 200_000
TEMP_REF = "refs/bundle-check/head"


def _git(repo, *args, check=True, binary=False):
    done = subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                          text=not binary)
    if check and done.returncode:
        raise subprocess.CalledProcessError(done.returncode, args, done.stdout, done.stderr)
    return done


def check(repo, bundle, branch, base, *, max_commits=MAX_COMMITS,
          max_diff_bytes=MAX_DIFF_BYTES, max_file_bytes=MAX_FILE_BYTES):
    """Return the list of problems; empty when the bundle may be pushed."""
    bundle = str(pathlib.Path(bundle).resolve())
    verify = _git(repo, "bundle", "verify", bundle, check=False)
    if verify.returncode:
        return [f"the bundle does not verify: {verify.stderr.strip().splitlines()[-1:] or ''}"]

    heads = [line.split()[1] for line in
             _git(repo, "bundle", "list-heads", bundle).stdout.splitlines() if line.strip()]
    if len(heads) != 1:
        return [f"exactly one ref expected, the bundle carries {len(heads)}: {', '.join(heads)}"]
    if heads[0] != f"refs/heads/{branch}":
        return [f"the bundle carries {heads[0]}, expected refs/heads/{branch}"]

    problems = []
    _git(repo, "fetch", "--quiet", "--no-tags", bundle, f"+{heads[0]}:{TEMP_REF}")
    try:
        head = _git(repo, "rev-parse", TEMP_REF).stdout.strip()
        if _git(repo, "merge-base", "--is-ancestor", base, head, check=False).returncode:
            return [f"the branch does not fast-forward from the base {base[:7]}"]

        commits = _git(repo, "rev-list", f"{base}..{head}").stdout.split()
        if not commits:
            problems.append("no new commits on top of the base")
        elif len(commits) > max_commits:
            problems.append(f"{len(commits)} commits, more than {max_commits}")
        merges = _git(repo, "rev-list", "--merges", f"{base}..{head}").stdout.split()
        if merges:
            problems.append("merge commit(s): " + ", ".join(m[:7] for m in merges))

        diff_bytes = len(_git(repo, "diff", "--binary", base, head, binary=True).stdout)
        if diff_bytes > max_diff_bytes:
            problems.append(f"the diff is {diff_bytes} bytes, more than {max_diff_bytes}")

        for line in _git(repo, "diff", "--raw", "--no-renames", base, head).stdout.splitlines():
            meta, path = line.split("\t", 1)
            _, new_mode, _, new_sha, status = meta.split()
            if new_mode == "120000":
                problems.append(f"{path}: a symlink")
            elif new_mode == "160000":
                problems.append(f"{path}: a submodule (gitlink)")
            elif status != "D":
                size = int(_git(repo, "cat-file", "-s", new_sha).stdout)
                if size > max_file_bytes:
                    problems.append(f"{path} is {size} bytes, more than {max_file_bytes}")
    finally:
        _git(repo, "update-ref", "-d", TEMP_REF, check=False)
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=".")
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--branch", required=True, help="cr/<n>")
    parser.add_argument("--base", required=True, help="the commit the build started on")
    parser.add_argument("--max-commits", type=int, default=MAX_COMMITS)
    parser.add_argument("--max-diff-bytes", type=int, default=MAX_DIFF_BYTES)
    parser.add_argument("--max-file-bytes", type=int, default=MAX_FILE_BYTES)
    args = parser.parse_args(argv)

    problems = check(args.repo, args.bundle, args.branch, args.base,
                     max_commits=args.max_commits, max_diff_bytes=args.max_diff_bytes,
                     max_file_bytes=args.max_file_bytes)
    if problems:
        print("Bundle refused:")
        for p in problems:
            print(f"- {p}")
        return 1
    print("Bundle OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
