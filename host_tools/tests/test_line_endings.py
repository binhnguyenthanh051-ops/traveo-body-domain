"""Repository hygiene: no stray carriage returns in host_tools Python sources (issue #43)."""
import re
from pathlib import Path

HOST_TOOLS = Path(__file__).resolve().parents[1]


def test_no_lone_carriage_return_in_python_sources():
    bad = []
    for path in sorted(HOST_TOOLS.rglob("*.py")):
        data = path.read_bytes()
        if re.search(rb"\r(?!\n)|\r(?=\r)", data):
            bad.append(str(path.relative_to(HOST_TOOLS)))
    assert bad == []
