"""Tests for the hash-pinned host_tools lock file (owner's review of #52, point 3).

The agent jobs and CI install host_tools' Python packages from PyPI, which is on the egress
allowlist. A version range resolves to whatever is newest at run time, so a compromised
release would reach the runners unseen. With a lock file and `pip --require-hashes`, every
package, including each dependency of a dependency, has an exact version and hash, and pip
refuses anything else.

`requirements.txt` stays the human-edited input (and what the bench PC installs);
`requirements.lock` is generated from it for the runners (Linux, Python 3.11)."""
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
REQS = ROOT / "host_tools" / "requirements.txt"
LOCK = ROOT / "host_tools" / "requirements.lock"
WORKFLOWS = ROOT / ".github" / "workflows"
INSTALL = "pip install --require-hashes -r host_tools/requirements.lock"


def norm(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def lock_entries():
    """{name: (version, [hashes])} from the lock file (pip's continuation-line format)."""
    text = LOCK.read_text(encoding="utf-8").replace("\\\n", " ")
    entries = {}
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"([A-Za-z0-9_.-]+)==([^\s;]+)", line)
        assert m, f"not pinned with ==: {line[:60]}"
        entries[norm(m.group(1))] = (m.group(2), re.findall(r"--hash=sha256:([0-9a-f]{64})", line))
    return entries


def test_every_locked_package_has_an_exact_version_and_a_hash():
    entries = lock_entries()
    assert entries
    for name, (_, hashes) in entries.items():
        assert hashes, f"{name}: no --hash"


def test_the_lock_covers_requirements_txt():
    wanted = {norm(re.match(r"[A-Za-z0-9_.-]+", l).group(0))
              for l in REQS.read_text(encoding="utf-8").splitlines()
              if l.strip() and not l.lstrip().startswith("#")}
    assert wanted <= set(lock_entries())


def test_the_lock_is_resolved_for_the_linux_runners():
    # pytest needs colorama on Windows only (the one difference between the two resolutions
    # on 2026-11-14): a lock resolved for the owner's Windows PC would carry it.
    assert "colorama" not in lock_entries()


def test_the_lock_says_how_to_regenerate_it():
    head = LOCK.read_text(encoding="utf-8")[:600]
    assert "--python-platform linux" in head and "--generate-hashes" in head


def test_every_workflow_installs_only_from_the_lock():
    installs = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        for line in path.read_text(encoding="utf-8").splitlines():
            if re.search(r"\bpip install\b", line) and not line.lstrip().startswith("#"):
                installs.append((path.name, line.strip()))
    assert installs, "no pip install found"
    for name, line in installs:
        assert INSTALL in line and line.endswith("requirements.lock"), f"{name}: {line}"
