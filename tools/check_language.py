#!/usr/bin/env python3
"""Fail the build if any Dutch reaches the public tree.

This repository is public and English-only: code, comments, docs, commit
messages, entity names, log strings, YAML comments. The rule is a build gate
rather than a preference, because the tempting shortcut when porting the
Dutch-commented phase-1 sources is to paste the code and drop the comments -
and those comments carry the reasoning that makes the code reviewable.

The word list holds only tokens that are never English (see dutch_words.txt),
so a hit is a finding and not a judgement call.

Usage:
    python tools/check_language.py [path ...]      # default: repo root
Exit code 0 when clean, 1 when anything is found.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

# This checker and its word list are the two files that legitimately contain
# Dutch. Everything else in the tree must not.
SELF_EXEMPT = {HERE / "check_language.py", HERE / "dutch_words.txt"}

SKIP_DIRS = {
    ".git", ".github/workflows/__pycache__", "__pycache__", ".esphome",
    ".pio", "build", "node_modules", ".venv", ".mypy_cache", ".pytest_cache",
}

# Text we can meaningfully read. Anything else is skipped as binary.
TEXT_SUFFIXES = {
    ".py", ".c", ".h", ".cpp", ".hpp", ".yaml", ".yml", ".md", ".txt",
    ".json", ".toml", ".ini", ".cfg", ".sh", ".mk", ".csv", "",
}


def load_words() -> list[str]:
    words = []
    for line in (HERE / "dutch_words.txt").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            words.append(line)
    if not words:
        sys.exit("check_language: word list is empty")
    return sorted(set(words))


def build_pattern(words: list[str]) -> re.Pattern:
    # Word boundaries on both sides, so "regel" does not fire inside
    # "regelement" and "kat" does not fire inside "concatenate".
    return re.compile(r"\b(" + "|".join(re.escape(w) for w in words) + r")\b",
                      re.IGNORECASE)


def iter_files(roots: list[Path]):
    for root in roots:
        if root.is_file():
            yield root
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            if path.resolve() in SELF_EXEMPT:
                continue
            yield path


def main(argv: list[str]) -> int:
    roots = [Path(a).resolve() for a in argv[1:]] or [ROOT]
    pattern = build_pattern(load_words())

    hits = []
    for path in iter_files(roots):
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # binary or unreadable: nothing to check
        for lineno, line in enumerate(text.splitlines(), 1):
            for m in pattern.finditer(line):
                try:
                    shown = path.relative_to(ROOT)
                except ValueError:
                    shown = path
                hits.append((shown, lineno, m.group(0), line.strip()[:100]))

    if not hits:
        print("check_language: clean (English only)")
        return 0

    print(f"check_language: {len(hits)} Dutch token(s) found\n")
    for path, lineno, word, line in hits:
        print(f"  {path}:{lineno}: {word!r}")
        print(f"      {line}")
    print("\nThe public tree is English-only. Translate, do not delete:")
    print("the comments carry the reasoning that makes the code reviewable.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
