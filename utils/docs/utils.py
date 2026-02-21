import os
import subprocess
import frontmatter

REQUIRED_FIELDS = ["title", "description", "slug", "author", "date"]


def slug_from_path(file_path):
    """Derive expected slug from file path.

    docs/reference/core/graph.mdx -> reference-core-graph
    docs/index.mdx -> index
    docs/guide.mdx -> guide
    """
    rel = file_path.removeprefix("docs/").removesuffix(".mdx")
    return rel.replace("/", "-")


def parse_file(file_path, base_url):
    with open(file_path, "r", encoding="utf-8") as f:
        post = frontmatter.load(f)

    for field in REQUIRED_FIELDS:
        if field not in post.metadata or post.metadata.get(field) in [None, ""]:
            raise ValueError(f"Missing required field '{field}' in {file_path}")

    slug = post.metadata["slug"]
    expected_slug = slug_from_path(file_path)
    if slug != expected_slug:
        raise ValueError(
            f"Slug mismatch in {file_path}: got '{slug}', expected '{expected_slug}'"
        )

    doc = {
        "title": post.metadata["title"],
        "description": post.metadata["description"],
        "url": f"{base_url}/{file_path}",
        "slug": slug,
        "author": post.metadata["author"],
        "date": post.metadata["date"],
        "tags": post.metadata.get("tags", []),
    }

    source = post.metadata.get("source")
    if source:
        doc["source"] = source

    return doc


def collect_docs_recursive(dir_path, base_url, slugs, authors, tags):
    """Recursively collect docs from a directory.

    For each .mdx file, if a matching directory exists, its children are
    collected recursively and attached under the 'children' key.
    """
    docs = []

    if not os.path.isdir(dir_path):
        return docs

    entries = os.listdir(dir_path)
    mdx_files = sorted({e[:-4] for e in entries if e.endswith(".mdx")})
    dirs = {e for e in entries if os.path.isdir(os.path.join(dir_path, e))}

    for name in mdx_files:
        file_path = os.path.join(dir_path, f"{name}.mdx")
        doc = parse_file(file_path, base_url)

        if doc["slug"] in slugs:
            raise ValueError(f"Duplicate slug: {doc['slug']}")
        slugs.add(doc["slug"])

        authors.add(doc["author"])
        tags.update(doc["tags"])

        if name in dirs:
            child_dir = os.path.join(dir_path, name)
            children = collect_docs_recursive(child_dir, base_url, slugs, authors, tags)
            if children:
                doc["children"] = children

        docs.append(doc)

    return docs


def current_branch():
    ci_ref = os.environ.get("CI_COMMIT_REF_NAME")
    if ci_ref:
        return ci_ref
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            text=True,
        ).strip()
    except Exception:
        return "main"


def collect_docs():
    slugs = set()
    authors = set()
    tags = set()
    branch = current_branch()
    base_url = f"https://github.com/bitspaceorg/master-of-puppets/raw/{branch}"

    docs = collect_docs_recursive("docs", base_url, slugs, authors, tags)

    return docs, sorted(authors), sorted(tags)
