#!/usr/bin/env python3
"""PRE-RENDER bakes the mixer strips (docs/TRACKS.md): the cache is AUDIO strip -> FX -> master strip, so what plays from it must
equal what the live graph plays with the same strips.

  python tools/prerender_strips.py [--fx "Legacy Distortion"] [--param Drive=0.7]

Live recording (PRE-RENDER off) against the recording of the cache (PRE-RENDER on), strips set (AUDIO -4 dB / balance 0.3,
master -2 dB), over the body of the file (the cache ends with the file: the FX ring-out after the last sample is not in it).
Control: the strips at their defaults give a different recording (must stay above -60 dB).
"""
import argparse, os, sys, tempfile, time
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench
from live_vs_rig import read_wav, write_wav, test_signal, align_residual_db


def record(tb, path, seconds):
    tb.call("record_start", path=path)
    time.sleep(0.4)
    tb.call("play", **{"from": 0.0})
    time.sleep(seconds + 1.0)
    tb.call("stop")
    time.sleep(0.3)
    tb.call("record_stop")
    return read_wav(path)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fx", default="Legacy Distortion")
    ap.add_argument("--param", default="Drive=0.7")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--limit", type=float, default=-100.0)
    a = ap.parse_args()
    pname, pval = a.param.split("="); pval = float(pval)
    tb = TestBench()
    tmp = tempfile.gettempdir()
    for r in ("fx", "inst", "inst2", "inst3"):
        tb.call("remove_plugin", role=r)
    tb.wait_idle()
    tb.call("set_source", mode="file"); tb.wait_idle()
    for _ in range(50):
        audio = tb.call("status").get("audio")
        if audio:
            break
        time.sleep(0.2)
    rate = int(audio["sample_rate"])
    src = os.path.join(tmp, "prerender_strips_src.wav")
    write_wav(src, test_signal(rate, a.seconds), rate)
    assert tb.call("load_plugin", role="fx", name=a.fx).get("ok"); tb.wait_idle()
    assert tb.call("set_param", role="fx", name=pname, value=pval).get("ok")
    assert tb.call("load_audio", path=src).get("ok"); tb.wait_idle()
    tb.call("set_source", mode="file"); tb.wait_idle()

    tb.call("track_set", track="audio", gain_db=-4.0, balance=0.3)
    tb.call("track_set", track="master", gain_db=-2.0)
    live = record(tb, os.path.join(tmp, "prs_live.wav"), a.seconds)

    assert tb.call("prerender", on=True).get("ok"); tb.wait_idle()
    time.sleep(0.5); tb.wait_idle()
    baked = record(tb, os.path.join(tmp, "prs_baked.wav"), a.seconds)

    tb.call("track_set", track="audio", gain_db=0.0, balance=0.0)
    tb.call("track_set", track="master", gain_db=0.0)
    time.sleep(1.0); tb.wait_idle(); time.sleep(0.5); tb.wait_idle()      # the cache re-renders with the strips at their defaults
    plain = record(tb, os.path.join(tmp, "prs_plain.wav"), a.seconds)
    tb.call("prerender", on=False); tb.wait_idle()

    # the body of the file: the cache ends with the file, so the FX ring-out after its last sample is in the live path only
    n_body = int((a.seconds - 0.3) * rate)

    def body(x):
        i = int(np.argmax(np.abs(x[:, 0]) > 1e-4))     # where the sound starts in this recording
        return x[i:i + n_body + 300]

    lag, res = align_residual_db(body(live), body(baked), search=200)
    lag2, res2 = align_residual_db(body(live), body(plain), search=200)
    print(f"live vs cache, strips set:   lag {lag}, residual {res:.1f} dB (limit {a.limit})")
    print(f"control, cache with strips at defaults: residual {res2:.1f} dB (must stay above -60)")
    ok = res <= a.limit and res2 > -60.0
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
