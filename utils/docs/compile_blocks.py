#!/usr/bin/env python3
"""Extract every fenced ```c block from docs/ and try to compile the ones
that look like complete programs.

Rules for inclusion:
- Code fence language is `c`.
- Block contains `int main(` — this is the "this is meant to stand alone"
  contract. Fragments without `int main(` are skipped.

Each block is written to a temp file and compiled against the prebuilt
static library at build/lib/libmop.a.  SDL programs are detected by the
presence of `<SDL.h>` and given the SDL2 link flags from `pkg-config`
when available — if not, the block is skipped with a warning.

Exit non-zero if any compilable block fails to compile.

Usage:  python3 utils/docs/compile_blocks.py
        [--cc <compiler>] [--verbose]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs"
LIB = ROOT / "build" / "lib" / "libmop.a"
INCLUDE = ROOT / "include"

FENCE_RE = re.compile(
    r"```c\s*\n(.*?)\n```",
    re.DOTALL,
)


def find_blocks() -> list[tuple[pathlib.Path, int, str]]:
    """Return (source_file, starting_line, block_text) for every ```c block."""
    out: list[tuple[pathlib.Path, int, str]] = []
    for mdx in DOCS.rglob("*.mdx"):
        text = mdx.read_text(encoding="utf-8")
        for m in FENCE_RE.finditer(text):
            start_line = text[: m.start()].count("\n") + 1
            out.append((mdx.relative_to(ROOT), start_line, m.group(1)))
    return out


def is_compilable(block: str) -> bool:
    return "int main(" in block


def pkg_flags(pkg: str) -> tuple[list[str], list[str]] | None:
    if shutil.which("pkg-config") is None:
        return None
    try:
        cf = subprocess.check_output(
            ["pkg-config", "--cflags", pkg], text=True,
            stderr=subprocess.DEVNULL,
        ).split()
        lf = subprocess.check_output(
            ["pkg-config", "--libs", pkg], text=True,
            stderr=subprocess.DEVNULL,
        ).split()
    except subprocess.CalledProcessError:
        return None
    return cf, lf


def sdl_flags() -> tuple[list[str], list[str]] | None:
    return pkg_flags("sdl2")


def libmop_needs_vulkan() -> bool:
    """Heuristic: if libmop.a contains an undefined vkCreateInstance symbol,
    it was built with Vulkan enabled and we must link the loader."""
    try:
        out = subprocess.check_output(
            ["nm", "-u", str(LIB)], text=True,
            stderr=subprocess.DEVNULL,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False
    return any("vkCreateInstance" in line for line in out.splitlines())


SKIP_RE = re.compile(r"/\*\s*docs-skip\s*:\s*([^*]+?)\s*\*/")


def compile_block(
    block: str,
    origin: str,
    cc: str,
    verbose: bool,
) -> tuple[str, str]:
    """Compile one block.  Returns (status, message) where status is
    one of 'ok', 'skip', 'fail'."""
    # Explicit opt-out marker for illustrative snippets.
    m = SKIP_RE.search(block)
    if m:
        return ("skip", f"explicit docs-skip: {m.group(1)}")

    # Local #include that isn't <mop/...> or <system/...> — we can't resolve it.
    for line in block.splitlines():
        s = line.strip()
        if s.startswith('#include "') and 'mop/' not in s:
            return ("skip", f"local include not resolvable: {s}")

    needs_sdl = "<SDL.h>" in block or "<SDL/SDL.h>" in block
    sdl = sdl_flags() if needs_sdl else None
    if needs_sdl and sdl is None:
        return ("skip", "SDL block — pkg-config sdl2 not found in env")
    vk = pkg_flags("vulkan") if libmop_needs_vulkan() else None

    with tempfile.TemporaryDirectory() as td:
        src = pathlib.Path(td) / "snippet.c"
        out = pathlib.Path(td) / "snippet"
        src.write_text(block, encoding="utf-8")

        cmd = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wno-unused-variable",
            "-Wno-unused-parameter",
            "-Wno-unused-but-set-variable",
            "-Wno-unused-function",
            f"-I{INCLUDE}",
        ]
        if sdl is not None:
            cmd += sdl[0]
        cmd += [
            str(src),
            f"-L{LIB.parent}",
            "-lmop",
            "-lm",
            "-lpthread",
        ]
        if sys.platform == "darwin":
            cmd += ["-lc++"]
        else:
            cmd += ["-lstdc++"]
        if sdl is not None:
            cmd += sdl[1]
        if vk is not None:
            cmd += vk[1]
        elif libmop_needs_vulkan():
            cmd += ["-lvulkan"]
        cmd += ["-o", str(out)]

        if verbose:
            print("  $", " ".join(cmd))

        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            msg = proc.stderr.strip() or proc.stdout.strip()
            return ("fail", f"{origin}:\n{msg}")
        return ("ok", "")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)

    if not LIB.exists():
        print(
            f"docs-check-code: {LIB.relative_to(ROOT)} missing — "
            f"build the library first (`make`).",
            file=sys.stderr,
        )
        return 2

    blocks = find_blocks()
    compilable = [(p, ln, b) for p, ln, b in blocks if is_compilable(b)]
    skipped = len(blocks) - len(compilable)

    failures: list[str] = []
    skipped_compilable = 0
    passed = 0
    for path, line, block in compilable:
        origin = f"{path}:{line}"
        status, msg = compile_block(block, origin, args.cc, args.verbose)
        if status == "ok":
            print(f"  OK   {origin}")
            passed += 1
        elif status == "skip":
            print(f"  SKIP {origin} ({msg})")
            skipped_compilable += 1
        else:
            print(f"  FAIL {origin}")
            failures.append(msg)

    print(
        f"docs-check-code: {passed} passed, "
        f"{skipped_compilable} skipped (compilable but env-gated), "
        f"{skipped} skipped (fragments), "
        f"{len(failures)} failures"
    )
    if failures:
        print("", file=sys.stderr)
        for f in failures:
            print(f, file=sys.stderr)
            print("---", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
