"""The agents' OAuth token is not visible to the code they run (owner's review of #52, point 2).

The Builder and the Inspector run this suite inside their agent job (`python -m pytest
host_tools`, `make test`), as subprocesses of Claude Code. With CLAUDE_CODE_SUBPROCESS_ENV_SCRUB
set on the job, the action strips the token from those subprocesses; this test is how that is
observed. It fails there if the token leaks, and passes in CI and on the owner's PC, where the
token is not set at all. It reports only "set", never the value."""
import os

SECRETS = ("CLAUDE_CODE_OAUTH_TOKEN",)


def test_the_oauth_token_is_not_visible_to_tests():
    visible = [name for name in SECRETS if os.environ.get(name)]
    assert not visible, f"set in this process: {', '.join(visible)} (value not shown)"
