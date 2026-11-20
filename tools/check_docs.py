#!/usr/bin/env python3
## @file
# Copyright (c) 2026, Cory Bennett. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
##

"""Check inline local links against tracked files and Markdown headings."""

from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit


def heading_ids(text):
    identifiers = set()
    fenced = False
    for line in text.splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fenced = not fenced
        if fenced:
            continue
        heading = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not heading:
            continue
        title = heading[1].lower()
        slug = "".join(c for c in title if c.isalnum() or c in " _-")
        base = slug.replace(" ", "-")
        slug, suffix = base, 1
        while slug in identifiers:
            slug = f"{base}-{suffix}"
            suffix += 1
        identifiers.add(slug)
    return identifiers


def main():
    root = Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True
        ).strip()
    ).resolve()
    tracked = {
        root / name
        for name in subprocess.check_output(
            ["git", "-C", root, "ls-files", "-z"], text=True
        ).split("\0")
        if name
    }
    documents = sorted(
        path
        for path in tracked
        if path.suffix == ".md"
        and (path.parent == root or path.is_relative_to(root / "docs"))
    )
    errors = []
    for document in documents:
        text = document.read_text()
        for link in re.finditer(r"\[[^\]]*\]\(([^\s)]+)\)", text):
            destination = urlsplit(link[1])
            if destination.scheme or destination.netloc:
                continue
            path = unquote(destination.path)
            if path.startswith("/"):
                target = root / path.lstrip("/")
            elif path:
                target = document.parent / path
            else:
                target = document
            target = target.resolve()
            included = target.is_relative_to(root) and (
                target in tracked
                or (
                    target.is_dir()
                    and any(p.is_relative_to(target) for p in tracked)
                )
            )
            reason = None
            if not included or not target.exists():
                reason = "target is missing or excluded from Git"
            elif target.suffix == ".md" and destination.fragment:
                if unquote(destination.fragment) not in heading_ids(
                    target.read_text()
                ):
                    reason = "heading does not exist"
            if reason:
                line = text.count("\n", 0, link.start()) + 1
                errors.append(
                    f"{document.relative_to(root)}:{line}: {link[1]}: {reason}"
                )
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Local links and headings checked in {len(documents)} documents.")


if __name__ == "__main__":
    main()
