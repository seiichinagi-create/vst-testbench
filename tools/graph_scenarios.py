#!/usr/bin/env python3
"""The live graph's routing in a fixed set of states, as sorted connection lines (the `graph_dump` command).

  python tools/graph_scenarios.py dump out.json            # run the scenarios against a running TestBench
  python tools/graph_scenarios.py compare a.json b.json    # equal routing in every scenario?

For checking a change to how the live graph is built (docs/TRACKS.md, P1): the audio engine is JUCE's own graph, so the
same connections mean the same processing; the scenarios cover each source mode with and without an FX, an instrument
with its MIDI, and PRE-RENDER (the FX is baked into the cache and leaves the chain).
"""
import json
import os
import struct
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench  # noqa: E402

FX = "Legacy Distortion"
INST = "PAGANIHANDS"


def make_wav(path):
    n = 48000
    frames = b"".join(struct.pack("<hh", int(8000 * ((i % 100) / 50 - 1)), int(8000 * ((i % 70) / 35 - 1))) for i in range(n))
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(frames)


def ok(r, what):
    if not r.get("ok", False):
        raise SystemExit(f"{what} failed: {r}")
    return r


def dump(tb):
    r = ok(tb.call("graph_dump"), "graph_dump")
    return r["connections"]


def scenarios(tb):
    out = {}
    wav = os.path.join(tempfile.gettempdir(), "graph_scenario.wav")
    make_wav(wav)

    def settle():
        tb.wait_idle()

    def reset():
        tb.call("prerender", on=False)
        tb.call("remove_plugin", role="fx")
        tb.call("remove_plugin", role="inst")
        settle()
        tb.call("set_source", mode="live")
        settle()

    reset()
    out["live, nothing loaded"] = dump(tb)

    ok(tb.call("load_plugin", role="fx", name=FX), "load fx"); settle()
    out["live + FX"] = dump(tb)

    ok(tb.call("load_plugin", role="inst", name=INST), "load inst"); settle()
    out["live + FX + instrument loaded"] = dump(tb)

    ok(tb.call("set_source", mode="inst"), "set_source inst"); settle()
    out["instrument + FX"] = dump(tb)

    ok(tb.call("remove_plugin", role="fx"), "remove fx"); settle()
    out["instrument, no FX"] = dump(tb)

    ok(tb.call("load_plugin", role="fx", name=FX), "load fx"); settle()
    ok(tb.call("load_audio", path=wav), "load_audio"); settle()
    ok(tb.call("set_source", mode="file"), "set_source file"); settle()
    out["file + FX + instrument loaded"] = dump(tb)

    ok(tb.call("remove_plugin", role="inst"), "remove inst"); settle()
    out["file + FX"] = dump(tb)

    ok(tb.call("remove_plugin", role="fx"), "remove fx"); settle()
    out["file, no FX"] = dump(tb)

    ok(tb.call("load_plugin", role="fx", name=FX), "load fx"); settle()
    tb.call("prerender", on=True); settle()
    out["file + FX, PRE-RENDER on"] = dump(tb)
    tb.call("prerender", on=False); settle()

    ok(tb.call("set_source", mode="live"), "set_source live"); settle()
    out["live + FX again"] = dump(tb)
    reset()
    return out


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "dump":
        tb = TestBench()
        res = scenarios(tb)
        with open(sys.argv[2], "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1, ensure_ascii=False)
        for k, v in res.items():
            print(f"  {k}: {len(v)} connections")
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "compare":
        a = json.load(open(sys.argv[2], encoding="utf-8"))
        b = json.load(open(sys.argv[3], encoding="utf-8"))
        bad = 0
        for k in a:
            if a[k] != b.get(k):
                bad += 1
                print(f"  DIFFERENT: {k}")
                print("    only before:", sorted(set(a[k]) - set(b.get(k, []))))
                print("    only after: ", sorted(set(b.get(k, [])) - set(a[k])))
            else:
                print(f"  same: {k} ({len(a[k])} connections)")
        print("PASS" if bad == 0 and set(a) == set(b) else "FAIL")
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
