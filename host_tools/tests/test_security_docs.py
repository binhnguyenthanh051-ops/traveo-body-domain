"""Guards for two lines from the owner's review of #52 (2026-11-14).

1. ADR-0026 states the risk that remains inside the egress allowlist: allowed hosts accept
   an attacker's own credentials, so the real control is owner-only input pinned at ready.
5. The Challenger asks, for every allowlist, permission or filter, what an attacker can
   still do inside it."""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(rel):
    return " ".join(ROOT.joinpath(rel).read_text(encoding="utf-8").split())


def test_adr_0026_names_the_risk_inside_the_allowlist():
    adr = read("docs/architecture/decisions/ADR-0026-agent-runtime.md")
    section = adr[adr.index("## 3. Secrets on the runners"):adr.index("## 4. Alternatives")]
    assert "attacker's own token" in section
    assert "pinned at the owner's `ready`" in section


def test_the_challenger_asks_what_an_attacker_can_still_do():
    prompt = read(".claude/agents/challenger.md")
    assert ("For every allowlist, permission or filter, ask what an attacker can still do "
            "inside it.") in prompt
