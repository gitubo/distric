#!/usr/bin/env python3
"""
fix_log_nulls.py — Add the required NULL sentinel to bare LOG_* macro calls.

Usage:
    python3 fix_log_nulls.py [--dry-run] [root_dir]

    --dry-run   Print what would change without modifying any files.
    root_dir    Directory to search recursively (default: libs).

A "bare" call is one where the last argument before the closing ) is a string
literal, meaning the NULL sentinel that terminates log_write()'s variadic
argument list is missing:

    LOG_INFO(logger, "component", "message");           ← BROKEN: missing NULL
    LOG_INFO(logger, "component", "message", NULL);     ← correct
    LOG_INFO(logger, "component", "msg", "k", "v");     ← BROKEN: missing NULL
    LOG_INFO(logger, "component", "msg", "k", "v", NULL); ← correct

The fix inserts ', NULL' immediately before the closing ) in every matched call.

Regex strategy:
    Match LOG_* calls whose last argument (before the closing paren) is a
    quoted string literal — not NULL, not a bare identifier, not a number.
    This correctly leaves well-formed calls (ending in NULL) untouched.

Limitation:
    Calls where the final argument is a non-literal expression (e.g. a char*
    variable) are not detected by this script. Those are rare; fix them manually
    if the compiler reports them.
"""

import re
import sys
import os
from pathlib import Path

# ---------------------------------------------------------------------------
# Pattern
# ---------------------------------------------------------------------------
# LOG_{DEBUG,INFO,WARN,ERROR,FATAL}(
#     <anything without closing paren>
#     "<last string argument>"   ← last arg is a string literal
#     <optional whitespace>
# )
#
# Group 1: everything up to and including the final closing quote
# Group 2: optional trailing whitespace
# Group 3: the closing )
#
# We rely on the fact that correctly-terminated calls end with NULL), not "..."),
# so they don't match this pattern.
# ---------------------------------------------------------------------------

BARE_LOG_RE = re.compile(
    r'(LOG_(?:DEBUG|INFO|WARN|ERROR|FATAL)\s*\([^)]*"[^"]*")(\s*)(\))',
    re.MULTILINE,
)


def fix_text(text: str) -> tuple[str, list[int]]:
    """Return (fixed_text, list_of_line_numbers_changed)."""
    lines_changed = []
    offset_to_line = build_offset_index(text)

    def replacer(m: re.Match) -> str:
        lines_changed.append(offset_to_line(m.start()))
        return m.group(1) + ', NULL' + m.group(2) + m.group(3)

    new_text = BARE_LOG_RE.sub(replacer, text)
    return new_text, lines_changed


def build_offset_index(text: str):
    """Return a function that maps a character offset to its 1-based line number."""
    newline_offsets = [i for i, c in enumerate(text) if c == '\n']

    def offset_to_line(offset: int) -> int:
        lo, hi = 0, len(newline_offsets)
        while lo < hi:
            mid = (lo + hi) // 2
            if newline_offsets[mid] < offset:
                lo = mid + 1
            else:
                hi = mid
        return lo + 1  # 1-based

    return offset_to_line


def process_file(path: Path, dry_run: bool) -> int:
    try:
        text = path.read_text(encoding='utf-8', errors='replace')
    except OSError as e:
        print(f"  SKIP {path}: {e}", file=sys.stderr)
        return 0

    new_text, lines_changed = fix_text(text)
    if not lines_changed:
        return 0

    tag = '[DRY RUN] ' if dry_run else ''
    for ln in lines_changed:
        print(f"  {tag}{path}:{ln}")

    if not dry_run:
        path.write_text(new_text, encoding='utf-8')

    return len(lines_changed)


def main() -> None:
    args = sys.argv[1:]
    dry_run = '--dry-run' in args
    if dry_run:
        args.remove('--dry-run')

    root = Path(args[0]) if args else Path('libs')
    if not root.exists():
        print(f"error: directory '{root}' not found.", file=sys.stderr)
        sys.exit(1)

    if dry_run:
        print(f"DRY RUN — scanning '{root}' (no files will be modified)\n")
    else:
        print(f"Scanning '{root}' and patching files...\n")

    total_fixes = 0
    total_files = 0

    for ext in ('*.c', '*.h'):
        for path in sorted(root.rglob(ext)):
            n = process_file(path, dry_run)
            if n:
                total_files += 1
                total_fixes += n

    action = "Would fix" if dry_run else "Fixed"
    print(f"\n{action} {total_fixes} call(s) across {total_files} file(s).")

    if dry_run and total_fixes:
        print("\nRe-run without --dry-run to apply changes.")


if __name__ == '__main__':
    main()