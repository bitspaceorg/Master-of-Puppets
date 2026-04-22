#!/usr/bin/env python3
"""Verify every slug referenced in docs/ resolves to a declared slug.

Exit non-zero on any broken link.  Safe to run from any cwd.

Usage:  python3 tools/docs/check_links.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs"

SLUG_DECL_RE = re.compile(r'^slug:\s*"([^"]+)"\s*$', re.MULTILINE)
# Match markdown link targets that look like a doc slug (no scheme, no dot).
LINK_RE = re.compile(r"\]\(([a-z][a-z0-9-]*)\)")


def collect_declared() -> set[str]:
    slugs: set[str] = set()
    for mdx in DOCS.rglob("*.mdx"):
        text = mdx.read_text(encoding="utf-8")
        m = SLUG_DECL_RE.search(text)
        if m:
            slugs.add(m.group(1))
    return slugs


def collect_referenced() -> dict[str, list[pathlib.Path]]:
    refs: dict[str, list[pathlib.Path]] = {}
    for mdx in DOCS.rglob("*.mdx"):
        text = mdx.read_text(encoding="utf-8")
        for m in LINK_RE.finditer(text):
            refs.setdefault(m.group(1), []).append(mdx.relative_to(ROOT))
    return refs


def main() -> int:
    declared = collect_declared()
    referenced = collect_referenced()

    broken = {s: files for s, files in referenced.items() if s not in declared}
    orphans = declared - set(referenced.keys())

    if broken:
        print("BROKEN links — slug referenced but never declared:", file=sys.stderr)
        for slug, files in sorted(broken.items()):
            print(f"  {slug}", file=sys.stderr)
            for f in sorted(set(files)):
                print(f"    in {f}", file=sys.stderr)

    if orphans:
        print("ORPHANED slugs — declared but never linked:", file=sys.stderr)
        for slug in sorted(orphans):
            print(f"  {slug}", file=sys.stderr)

    print(
        f"docs-check-links: {len(declared)} declared, "
        f"{len(referenced)} referenced, "
        f"{len(broken)} broken, {len(orphans)} orphans"
    )
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
