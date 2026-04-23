# Docs Tooling

All docs automation lives here.

| Script              | Purpose                                                                | Where it runs                                       |
| ------------------- | ---------------------------------------------------------------------- | --------------------------------------------------- |
| `validate.py`       | Link / slug / frontmatter / path-vs-slug consistency                   | `docs-build` pre-commit hook + CI `make docs-check` |
| `build_index.py`    | Regenerate `docs/data.json` from the mdx tree                          | `docs-build` pre-commit hook (auto-adds to commit)  |
| `compile_blocks.py` | Compile every fenced \`\`\`c block with `int main(` against `libmop.a` | CI `make docs-check`                                |
| `utils.py`          | Shared frontmatter + path-to-slug helpers used by the above            | —                                                   |
| `hook.sh`           | Entry point for the pre-commit hook                                    | referenced by `nix/utils/precommit.nix`             |

## Running locally

```bash
make docs-check                      # both validators
python3 utils/docs/validate.py       # link + slug + frontmatter alone
python3 utils/docs/compile_blocks.py # code-block compile alone
```

`compile_blocks.py` requires `build/lib/libmop.a` — run `make` first. On Vulkan-enabled builds the linker command auto-adds `-lvulkan`.

## What `validate.py` enforces

- Every `[text](slug)` resolves to a declared slug.
- Every `.mdx` has the required frontmatter fields (`title, description, slug, author, date`).
- Every `slug:` matches its file path — `docs/reference/core/viewport.mdx` ⇔ `reference-core-viewport`. Catches drift between filesystem and URL space.
- No duplicate slugs.

## Code-block conventions

Skip a `c` block from the compile check two ways:

- **Local non-`<mop/...>` `#include`** — auto-skipped (e.g. a test-harness fragment).
- **Explicit marker** — `/* docs-skip: <reason> */` anywhere in the block. Use when the block is illustrative rather than a full example.

Normal counts: ~350 fragments skipped, 1–3 compilable blocks pass, 0 failures.

## What's NOT here (on purpose)

- **Signature drift check.** Would require a fragile regex C-parser. If we ever want real signature sync, use Doxygen + generate signature blocks, not a bespoke linter.
- **Frontmatter linter for `capability:` / `since:`.** Dead metadata without a tool that consumes it — don't add until one exists.
- **Spell / grammar / style.** Pick a standard tool (vale, prose-lint) if needed; don't roll our own.
