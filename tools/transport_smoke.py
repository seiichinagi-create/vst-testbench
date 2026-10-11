#!/usr/bin/env python3
"""Live host transport: `transport` start / stop / seek / status, midi_play starts it and midi_stop stops it.
Position advances with the audio callback (real time), so the check is a tolerance on wall-clock time."""
import sys, time
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from tb import TestBench

tb = TestBench()
fails = 0


def check(name, ok):
    global fails
    print(("ok   " if ok else "FAIL ") + name)
    fails += 0 if ok else 1


tb.call("transport", action="stop")
r = tb.call("transport", action="status")
check("stopped at 0", not r["playing"] and r["seconds"] == 0.0)
r = tb.call("transport", action="start", bpm=90, **{"from": 1.0})
check("start from 1.0 s, bpm 90", r["playing"] and abs(r["bpm"] - 90) < 1e-9 and 0.99 <= r["seconds"] <= 1.1)
time.sleep(1.0)
r = tb.call("transport", action="status")
check("advances in real time (1.0 s)", 1.9 <= r["seconds"] <= 2.2)
r = tb.call("transport", action="stop", rewind=False)
t = r["seconds"]
time.sleep(0.3)
check("stop without rewind holds the position", not r["playing"] and abs(tb.call("transport", action="status")["seconds"] - t) < 1e-6)
r = tb.call("transport", action="seek", seconds=5.0)
check("seek", abs(r["seconds"] - 5.0) < 1e-3)
r = tb.call("transport", action="stop")
check("stop rewinds", r["seconds"] == 0.0)
check("bad action refused", "error" in tb.call("transport", action="bogus"))
print("FAILED %d" % fails)
sys.exit(1 if fails else 0)
