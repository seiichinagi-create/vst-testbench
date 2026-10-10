#!/usr/bin/env python3
"""Run Steinberg's VST3 validator on every VST3 made by SeiNagi and write a one-line-per-plugin summary.

    python run_own_plugins.py [--extensive] [--only NAME]

The plugin list comes from the VST TestBench's plugin cache (its control server must be running), so it is the same
set of binaries the bench and the rig use. Each plugin runs in its own process with a time limit; a crash or a
timeout is a result, not an error of this script.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

VALIDATOR = r"C:\dev\vst3sdk\build\bin\Release\validator.exe"
OUT = r"C:\dev\vst3sdk\validator_results"


def plugin_files():
    sys.path.insert(0, r"C:\dev\vst-testbench\tools")
    from tb import TestBench
    tb = TestBench()
    seen = {}
    for p in tb.call("list_plugins")["plugins"]:
        if "seinagi" in p.get("manufacturer", "").lower().replace(" ", ""):
            if not os.path.exists(p["file"]):
                print(f"  (skipped: the plugin cache lists {p['file']} but it no longer exists)")
                continue
            seen.setdefault(p["file"], p["name"])
    tb.sock.close()
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--extensive", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    files = plugin_files()
    rows = []
    for path, name in sorted(files.items(), key=lambda kv: kv[1].lower()):
        if a.only and a.only.lower() not in name.lower():
            continue
        cmd = [VALIDATOR] + (["-e"] if a.extensive else []) + [path]
        t0 = time.time()
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=a.timeout)
            text, code, status = r.stdout + r.stderr, r.returncode, None
        except subprocess.TimeoutExpired as e:
            text = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            code, status = None, f"TIMEOUT after {a.timeout} s"
        safe = re.sub(r"[^A-Za-z0-9_.-]+", "_", name)
        open(os.path.join(OUT, safe + (".extensive" if a.extensive else "") + ".txt"), "w", encoding="utf-8").write(text)
        m = re.search(r"Result:\s*(\d+) tests passed,\s*(\d+) tests failed", text)
        failed = re.findall(r"^\[Failed\].*$|^.*\bFAILED\b.*$", text, flags=re.M)
        if status is None and m:
            status = f"{m.group(1)} passed, {m.group(2)} failed"
        elif status is None:
            status = f"NO RESULT (exit code {code})"
        rows.append({"name": name, "file": path, "status": status, "exit": code, "seconds": round(time.time() - t0, 1),
                     "failed_lines": failed[:6]})
        print(f"{name:34s} {status}   ({rows[-1]['seconds']} s)", flush=True)
    with open(os.path.join(OUT, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)
    bad = [r for r in rows if not re.fullmatch(r"\d+ passed, 0 failed", r["status"])]
    print(f"\n{len(rows)} plugins, {len(bad)} with a failure, a crash or a timeout")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
