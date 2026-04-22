# Docs Checks

Two narrow tools that run on every push. Both are intentionally small; feature creep here trades catching drift for people ignoring noisy output.

| Script              | What it verifies                                                                                                                                                       |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `check_links.py`    | Every `[text](slug)` in `docs/**/*.mdx` resolves to a `slug:` declared in some other `.mdx`. Exits non-zero on broken links.                                           |
| `compile_blocks.py` | Every fenced \`\`\`c block that looks like a complete program (contains `int main(`) compiles against `build/lib/libmop.a`. Fragments without `int main(` are skipped. |

## Running locally

```bash
make docs-check           # both
python3 tools/docs/check_links.py
python3 tools/docs/compile_blocks.py
```

`compile_blocks.py` requires `build/lib/libmop.a` to exist — run `make` first. On Vulkan-enabled builds the script auto-adds `-lvulkan` to the link line.

## Conventions

### Skipping a code block

Two ways — both explicit, both rare:

- **Local non-`<mop/...>` `#include`** — automatically skipped (the block pulls in a header the script can't resolve, e.g. a test harness).
- **Explicit marker** — `/* docs-skip: <reason> */` anywhere in the block. Use when the block is illustrative rather than a full example.

### Expected counts

- ~350 fragments skipped — most docs blocks are call-pattern examples, not full programs. Normal.
- 1-3 compilable blocks pass — the minimal render loop in `docs/skill.mdx`, plus any reference pages with full `int main` examples.
- 0 failures — if this is non-zero, something drifted.

## What's NOT here (on purpose)

- **Signature drift check.** Tried; would have been a fragile regex C-parser. If we ever want real signature sync, use Doxygen + generate the signature blocks, not a bespoke linter.
- **Frontmatter linter.** Until we have a tool that consumes `capability:` / `since:` / etc., they're dead metadata.
- **Spell / grammar / style.** Pick a standard tool (vale, prose-lint) when we want that; don't roll our own.
