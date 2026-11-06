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


BAT_SUFFIXES = {".bat", ".cmd"}


def is_bat(path: str) -> bool:
    return is_code(path) and Path(path).suffix.lower() in BAT_SUFFIXES


def bat_paren_offenders(text: str):
    """Lines of a .bat where a %VAR% whose NAME holds a paren is expanded
    unquoted inside a ( ... ) block. Yields 1-based line numbers.

    cmd expands %VAR% before it parses the block, so the ')' in
    "C:\\Program Files (x86)" closes the block early. It does so silently:
    measured on Windows 11 (2026-10-13),

        if 1==0 (
          echo %ProgramFiles(x86)%
          echo IN2
        )

    prints IN2 although the condition is false, and an unquoted
    `set P=%ProgramFiles(x86)%\\x` in a block fails the whole block.
    Quoted uses are fine and are not reported: the condition
    `if exist "%ProgramFiles(x86)%\\x" (` and `set "P=%ProgramFiles(x86)%\\x"`
    both ran correctly in the same test. Outside a block it is harmless.

    Only names that themselves contain a paren are checked (ProgramFiles(x86),
    CommonProgramFiles(x86)); a variable that merely holds such a value is not
    visible to a scan of the source.
    """
    depth = 0
    for no, line in enumerate(text.splitlines(), 1):
        head = line.lstrip().lower()
        if head.startswith("::") or head in ("rem", "@rem") or head.startswith(("rem ", "rem\t", "@rem ")):
            continue
        inq, i, n, hit = False, 0, len(line), False
        while i < n:
            ch = line[i]
            if ch == '"':
                inq = not inq
            elif ch == "^" and not inq:
                i += 1  # escaped char: not syntax
            elif ch == "%":
                nxt = line[i + 1] if i + 1 < n else ""
                if nxt in ("%", "~", "*") or nxt.isdigit():
                    i += 2  # %%a, %~dp0, %1, %*: not a %NAME% expansion
                    continue
                end = line.find("%", i + 1)
                if end < 0:
                    break
                name = line[i + 1:end]
                if ("(" in name or ")" in name) and not inq and depth > 0:
                    hit = True
                i = end + 1
                continue
            elif ch == "(" and not inq:
                depth += 1
            elif ch == ")" and not inq:
                depth = max(0, depth - 1)
            i += 1
        if hit:
            yield no


ESCAPE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".m", ".mm",
                   ".py", ".js", ".ts", ".jsx", ".tsx"}

_LITERAL = re.compile(r"""(?P<pre>\b[rRbBfFuU8]{1,2})?(?P<q>["'])(?P<body>(?:\\.|(?!(?P=q)).)*)(?P=q)""")
# The drive letter starts the literal or follows a space = ; , ( - so "%p:" then
# backslash-n in a printf format is not taken for a drive.
_DRIVE_ONE_SLASH = re.compile(r"(?:^|(?<=[\s=;,(]))[A-Za-z]:\\(?!\\)")


def path_escape_suspects(line: str, path: str) -> bool:
    r"""A string literal that holds a Windows path written with single backslashes,
    in a language where the backslash starts an escape: the path C:/temp/new
    typed with single backslashes holds a tab and a newline when the program
    runs, and nothing at commit time can see it (the file is plain ASCII). Raw
    literals (r"..", R"(..)") are left alone. WARNING ONLY: it never fails a
    commit, because a path in a message or a regex is not always a mistake.
    Only the drive form (X: then one backslash) is looked for; a relative path
    is too often something else.
    """
    if Path(path).suffix.lower() not in ESCAPE_SUFFIXES:
        return False
    head = line.lstrip()
    if head.startswith(("#", "//", "/*", "*")):
        return False
    for m in _LITERAL.finditer(line):
        if m.group("pre") and "r" in m.group("pre").lower():
            continue
        if _DRIVE_ONE_SLASH.search(m.group("body")):
            return True
    return False


def staged_text(path: str) -> str:
    return subprocess.run(["git", "show", f":{path}"], capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout


def check_staged() -> int:
    diff = subprocess.run(["git", "diff", "--cached", "--unified=0",
                           "--no-color", "--diff-filter=ACM"],
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace").stdout
    path, lineno, bad, warn = None, 0, [], []
    added = {}  # .bat path -> {line number: text} of the lines this commit adds
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
                if is_bat(path):
                    added.setdefault(path, {})[lineno] = line
                if path_escape_suspects(line, path):
                    warn.append((path, lineno, line.strip()[:70]))
            lineno += 1

    if warn:  # information only: does not change the exit code
        print("warning: a Windows path with single backslashes inside a string literal.")
        print("  When the program runs, \\t \\n \\a \\x.. in it are escapes, not path text.")
        print("  Write C:\\\\dir, C:/dir, or a raw literal (r\"..\" / R\"(..)\"). Not blocking.")
        for p, n, text in warn:
            print(f"  {p}:{n}: {text}")

    # The paren check needs the whole file (block depth) but reports only
    # added lines, so existing scripts stay legal until someone edits them.
    bat_bad = []
    for p, lines in added.items():
        for no in bat_paren_offenders(staged_text(p)):
            if no in lines:
                bat_bad.append((p, no, lines[no].strip()[:70]))

    if not bad and not bat_bad:
        return 0
    if bad:
        print("non-ASCII in added source lines. Source must stay ASCII:")
        print("  (prose belongs in .md; keep code comments in English)")
        print("  note: this also catches typographic punctuation that slips in from")
        print("        prose - em/en dashes, curly quotes, ellipsis, x, ->, +/-")
        for p, n, text, chars in bad:
            print(f"  {p}:{n}: {text}")
            print(f"      offending: {chars}")
    if bat_bad:
        print("unquoted %ProgramFiles(x86)% (or another %NAME(..)%) inside a ( ) block.")
        print("  cmd expands it first and its ')' silently closes the block: lines")
        print("  after it run even when the if is false. Quote it (set \"P=%...%\\x\")")
        print("  or move it out of the block and branch with goto.")
        for p, n, text in bat_bad:
            print(f"  {p}:{n}: {text}")
    print(f"\n{len(bad) + len(bat_bad)} line(s). Fix them, or use --no-verify when editing a")
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
    paren = []
    for f in files:
        if is_bat(f):
            try:
                text = Path(f).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            paren += [(f, no) for no in bat_paren_offenders(text)]
    print(f"\n.bat lines with an unquoted %NAME(..)% inside a ( ) block: {len(paren)}")
    for f, no in paren[:15]:
        print(f"  {f}:{no}")
    esc = []
    for f in files:
        if is_code(f) and Path(f).suffix.lower() in ESCAPE_SUFFIXES:
            try:
                text = Path(f).read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            esc += [(f, no) for no, ln in enumerate(text.splitlines(), 1) if path_escape_suspects(ln, f)]
    print(f"\nstring literals with a single-backslash Windows path (warning only): {len(esc)}")
    for f, no in esc[:15]:
        print(f"  {f}:{no}")
    print("\n(informational: existing lines are grandfathered. The hook only")
    print(" rejects lines a commit adds.)")
    return 0


# Control cases for bat_paren_offenders: (lines, expected flagged line numbers).
# The verdicts were taken by running each case in cmd.exe (2026-10-13), so a
# change that makes these disagree is a change in what the check believes
# about cmd, not only in the code.
BAT_CASES = [
    (["if 1==0 (", "  echo %ProgramFiles(x86)%", "  echo IN2", ")"], [2]),
    (["if 1==1 (", "  set P=%ProgramFiles(x86)%\\x", ")"], [2]),
    (["for %%a in (1) do (", "  echo %CommonProgramFiles(x86)%", ")"], [2]),
    (["if 1==1 (", "  set \"P=%ProgramFiles(x86)%\\x\"", ")"], []),
    (["if not exist \"%ProgramFiles(x86)%\\x\" (", "  echo IN", ")"], []),
    (["echo %ProgramFiles(x86)%", "set P=%ProgramFiles(x86)%\\x"], []),
    (["if 1==1 (echo a) else (echo b)", "echo %ProgramFiles(x86)%"], []),
    (["if 1==1 (", "  echo %~dp0 %1 %%a %PATH%", ")"], []),
    (["if 1==1 (", "  rem %ProgramFiles(x86)%", ")"], []),
]


def selftest() -> int:
    fails = 0
    for lines, want in BAT_CASES:
        got = list(bat_paren_offenders("\n".join(lines)))
        if got != want:
            fails += 1
            print(f"  FAIL want {want} got {got}: {lines}")
    print(f"bat paren cases: {len(BAT_CASES) - fails} pass / {fails} fail")
    efails = 0
    for line, path, want in ESCAPE_CASES:
        if path_escape_suspects(line, path) != want:
            efails += 1
            print(f"  FAIL want {want}: {path}: {line}")
    print(f"path escape cases: {len(ESCAPE_CASES) - efails} pass / {efails} fail")
    return 1 if fails or efails else 0


# Control cases for path_escape_suspects: (line, file, expected).
ESCAPE_CASES = [
    ('p = "C:\\temp\\new"', "a.py", True),
    ('auto p = "C:\\dev\\x.wav";', "a.cpp", True),
    ("const p = 'D:\\data';", "a.ts", True),
    ('p = "C:\\\\temp\\\\new"', "a.py", False),       # escaped: fine
    ('p = r"C:\\temp\\new"', "a.py", False),          # raw
    ('auto p = R"(C:\\temp)";', "a.cpp", False),      # C++ raw
    ('p = "C:/temp/new"', "a.py", False),
    ('# see "C:\\temp\\new"', "a.py", False),         # comment
    ('$p = "C:\\temp\\new"', "a.ps1", False),         # no escapes in PowerShell
    ('set "P=C:\\temp"', "a.bat", False),
    ('x = "a:\\b"', "a.c", True),
    ('url = "http://x"', "a.py", False),
    ('printf("for %p:\\n", f);', "a.c", False),       # printf format, not a path
    ('msg = "see C:\\temp"', "a.py", True),
]


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    if "--all" in sys.argv:
        return check_all()
    if "--staged" in sys.argv:
        return check_staged()
    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
