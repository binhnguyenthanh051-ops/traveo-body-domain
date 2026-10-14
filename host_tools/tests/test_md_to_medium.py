"""Unit tests for md_to_medium (no network)."""
import base64
import importlib.util
import pathlib
import urllib.parse

_PATH = pathlib.Path(__file__).resolve().parents[1] / "blog" / "md_to_medium.py"
_spec = importlib.util.spec_from_file_location("md_to_medium", _PATH)
mm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mm)


def test_table_becomes_a_list_with_bold_first_column():
    md = "| Item | Value | Why |\n|---|---|---|\n| MAC | 8 B | fits |\n"
    out = mm.convert(md)
    assert "<table" not in out
    assert "<li><strong>MAC</strong> — Value: 8 B; Why: fits</li>" in out


def test_numbered_table_merges_number_and_name():
    md = "| # | Attack | Result |\n|---|---|---|\n| 3 | Replay | `REJECT_FRESHNESS` |\n"
    assert "<strong>3 · Replay</strong> — Result: <code>REJECT_FRESHNESS</code>" in mm.convert(md)


def test_mermaid_becomes_a_mermaid_ink_image_with_the_code_round_tripping():
    code = "flowchart LR\n  A --> B"
    out = mm.convert("```mermaid\n" + code + "\n```\n")
    url = out.split('src="')[1].split('"')[0]
    b64 = urllib.parse.urlparse(url).path.split("/img/")[1]
    assert base64.urlsafe_b64decode(b64).decode() == code


def test_editor_only_parts_are_dropped_and_todos_highlighted():
    md = ("> **Draft status:** wip\n\nText TODO(you): add a line.\n\n<!-- x -->\n"
          "## Figures list\n1. a\n")
    out = mm.convert(md)
    assert "Draft status" not in out and "Figures list" not in out and "<!--" not in out
    assert "<mark>TODO(you): add a line.</mark>" in out


def test_heading_annotations_are_stripped():
    assert "<h2>5. User manual</h2>" in mm.convert("## 5. User manual *(outline)*\n")


def test_img_base_points_figures_and_images_at_the_repo():
    md = "![hero](img/m5/hero.png)\n\n```mermaid\nflowchart LR\n  A --> B\n```\n"
    out = mm.convert(md, "https://raw.example/docs/blogs", "img/m5")
    assert '<img src="https://raw.example/docs/blogs/img/m5/hero.png" alt="hero">' in out
    assert '<img src="https://raw.example/docs/blogs/img/m5/fig-01.png"' in out
    assert "mermaid.ink" not in out

