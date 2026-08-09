# -*- coding: utf-8 -*-
"""check_ascii.py - refuse non-ASCII in source files, at commit time.

  python .githooks/check_ascii.py --staged   # what the pre-commit hook runs
  python .githooks/check_ascii.py --all      # report on the whole tree

Why this exists
---------------
Source passes through several layers that each have their own default encoding,
and they disagree *silently*:

  - PowerShell 5.1 reads a BOM-less .ps1 as the ANSI code page (932 here),
    PowerShell 7 reads it as UTF-8. A script that parses in one is a syntax
    error in the other.
  - MSVC reads a BOM-less source as the ANSI code page unless /utf-8 is passed.
    Comment bytes then break the parse (C4819 -> C2059).
  - juce::String(const char*) treats bytes as ASCII, so >127 becomes mojibake
    that the compiler never warns about.

None of these raises an error where the mistake was made. Keeping source
ASCII-only removes the whole class: there is nothing left to disagree about.

What is checked
---------------
Source only. Prose (.md) is untouched, and so is anything under a directory in
SKIP_DIRS - build output, vendored copies and third party trees are not ours to
police. Existing lines are grandfathered: --staged only looks at lines the
commit ADDS, so old files stay legal until someone edits them.

This file is distributed from sn-lookandfeel. Do not edit it in place; fix the
copy there and hand it out again.
"""
import re
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CODE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".m", ".mm",
                 ".py", ".ps1", ".psm1", ".bat", ".cmd", ".sh", ".cmake",
                 ".js", ".ts", ".jsx", ".tsx", ".css", ".scad"}
CODE_NAMES = {"CMakeLists.txt"}

# Not ours to police: build output, vendored copies, third party, environments.
SKIP_DIRS = {
    ".git", "__pycache__", ".venv", "venv", "node_modules",
    "build", "builds", "Builds", "build-cu128", "cmake-build-debug",
    "cmake-build-release", "out", "bin", "obj", "x64", "Debug", "Release",
    "JuceLibraryCode", "JUCE", "juce", "extern", "external", "third_party",
    "thirdparty", "vendor", "_deps", "dist", "site-packages",
    # data and results: large, machine generated, not source
    "data", "models", "results", "ref", "captures",
}


def is_code(path: str) -> bool:
    p = Path(path)
    if any(part in SKIP_DIRS for part in p.parts):
        return False
    return p.suffix.lower() in CODE_SUFFIXES or p.name in CODE_NAMES


def offenders(text: str):
    """Every character outside printable ASCII. Tabs and newlines are fine."""
    for i, ch in enumerate(text):
        if ch in "\t\r\n":
            continue
        if ord(ch) < 32 or ord(ch) > 126:
            yield i + 1, ch


def check_staged() -> int:
    diff = subprocess.run(["git", "diff", "--cached", "--unified=0",
                           "--no-color", "--diff-filter=ACM"],
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace").stdout
    path, lineno, bad = None, 0, []
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
    for raw in diff.splitlines():
        if raw.startswith("+++ b/"):
            path = raw[6:]
            continue
        m = hunk.match(raw)
        if m:
            lineno = int(m.group(1))
            continue
        if raw.startswith("+") and not raw.startswith("+++"):
            line = raw[1:]
            if path and is_code(path):
                found = list(offenders(line))
                if found:
                    bad.append((path, lineno, line.strip()[:70],
                                "".join(sorted({c for _, c in found}))[:20]))
            lineno += 1
    if not bad:
        return 0
    print("non-ASCII in added source lines. Source must stay ASCII:")
    print("  (prose belongs in .md; keep code comments in English)")
    print("  note: this also catches typographic punctuation that slips in from")
    print("        prose - em/en dashes, curly quotes, ellipsis, x, ->, +/-")
    for p, n, text, chars in bad:
        print(f"  {p}:{n}: {text}")
        print(f"      offending: {chars}")
    print(f"\n{len(bad)} line(s). Fix them, or use --no-verify when editing a")
    print("file that predates this rule and has to keep its wording.")
    return 1


def check_all() -> int:
    files = subprocess.run(["git", "ls-files"], capture_output=True, text=True,
                           encoding="utf-8", errors="replace").stdout.split()
    total, worst = 0, []
    for f in files:
        if not is_code(f):
            continue
        try:
            text = Path(f).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        n = sum(1 for ln in text.splitlines() if any(True for _ in offenders(ln)))
        if n:
            total += n
            worst.append((n, f))
    worst.sort(reverse=True)
    print(f"source files with non-ASCII lines: {len(worst)} / {total} lines total")
    for n, f in worst[:15]:
        print(f"  {n:5d}  {f}")
    print("\n(informational: existing lines are grandfathered. The hook only")
    print(" rejects lines a commit adds.)")
    return 0


def main() -> int:
    if "--all" in sys.argv:
        return check_all()
    if "--staged" in sys.argv:
        return check_staged()
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
