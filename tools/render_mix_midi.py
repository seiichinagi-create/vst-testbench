#!/usr/bin/env python3
"""render_mix on an instrument track (docs/TRACKS.md, P4): the loaded MIDI file through INST 1, rendered by the rig, must match
what the bench's own MIDI bounce (the older offline path) writes for the same instrument and file.

  python tools/render_mix_midi.py [--inst PAGANIHANDS] [--midi assets/test_bounce.mid]
"""
import argparse, glob, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench
from live_vs_rig import read_wav, align_residual_db


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst", default="PAGANIHANDS")
    ap.add_argument("--midi", default=os.path.join(os.path.dirname(__file__), "..", "assets", "test_bounce.mid"))
    ap.add_argument("--limit", type=float, default=-90.0)
    a = ap.parse_args()
    tb = TestBench()
    for r in ("fx", "inst", "inst2", "inst3"):
        tb.call("remove_plugin", role=r)
    tb.wait_idle()
    assert tb.call("load_plugin", role="inst", name=a.inst).get("ok"); tb.wait_idle()
    midi = os.path.abspath(a.midi)
    r = tb.call("load_midi", path=midi)
    assert r.get("ok"), r
    tb.wait_idle()                       # the bounce runs and its wav is loaded into the file player
    st = tb.call("status")
    bounce = st.get("file", {}).get("playable_wav")
    print("file player:", bounce)
    assert bounce and os.path.exists(bounce), "cannot find the bounced wav"
    # the instrument sounds in VSTi mode; AUDIO (the bounce) is muted by the mode preset
    assert tb.call("set_source", mode="inst").get("ok"); tb.wait_idle()
    out = os.path.join(os.environ.get("TEMP", "."), "render_mix_midi.wav")
    r = tb.call("render_mix", out=out, compensate=True, tail=2.0)
    assert r.get("ok"), r
    tb.wait_idle()
    rig = tb.call("status").get("rig", {})
    print("rig:", {k: rig.get(k) for k in ("ok", "samples", "peak_db", "chain_latency")})
    b, br = read_wav(bounce)
    x, xr = read_wav(out)
    print(f"bounce {len(b)/br:.2f} s, render_mix {len(x)/xr:.2f} s")
    n = min(len(b), len(x))
    peak = max(abs(b[:n]).max(), 1e-9)
    # same start (both offline, compensated): compare directly, then with the best alignment
    direct = 20 * __import__("numpy").log10(max(__import__("numpy").sqrt(((b[:n] - x[:n]) ** 2).mean()), 1e-12) / max(__import__("numpy").sqrt((b[:n] ** 2).mean()), 1e-12))
    lag, res = align_residual_db(b, x)
    print(f"direct residual {direct:.1f} dB; aligned (lag {lag}) {res:.1f} dB re the bounce (limit {a.limit})")
    ok = min(direct, res) <= a.limit
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
