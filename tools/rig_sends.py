#!/usr/bin/env python3
"""Send buses in the offline rig (docs/TRACKS.md): a track's post-fader send feeds a bus, the bus (FX + return strip) is summed into
the master with the tracks, and the graph lines the paths up by declared latency.

  python tools/rig_sends.py

Test double: an impulse track with a send of -6 dB into bus 1, whose FX is a pure delay of 7 samples.
  honest (declares 7):    the direct path is delayed by the graph to meet the bus path: ONE peak, 0.1 * (1 + 10^(-6/20)) at the impulse
  dishonest (declares 0): the paths do not meet: TWO peaks, 0.1 at the impulse and 0.1 * 10^(-6/20) seven samples later (control)
  return strip -6 dB:     the bus path is 6 dB lower: ONE peak, 0.1 * (1 + 10^(-12/20))
  no send / no bus:       the send alone does nothing (a send into an empty bus is not connected)
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench
from live_vs_rig import read_wav

G = 10 ** (-6 / 20)
AT, AMP = 1000, 0.1
tmp = os.environ.get("TEMP", ".")
bad = 0


def render(tb, name, **kw):
    out = os.path.join(tmp, name)
    r = tb.call("rig_render", source="impulse", impulse_at=AT, impulse_amp=AMP, out=out, rate=48000, block=512, tail=0.2, **kw)
    assert r.get("ok"), r
    tb.wait_idle()
    x, _ = read_wav(out)
    return x[:, 0], tb.call("status").get("rig", {})


def peaks(x, floor=1e-5):
    idx = [i for i in range(len(x)) if abs(x[i]) > floor]
    return [(i, float(x[i])) for i in idx]


def check(label, cond, detail=""):
    global bad
    print(f"  {'ok  ' if cond else 'FAIL'} {label} {detail}")
    bad += 0 if cond else 1


def main():
    tb = TestBench()
    # honest bus
    x, rig = render(tb, "sends_honest.wav", send1_db=-6, bus1_delay_actual=7, bus1_delay_declared=7)
    p = peaks(x)
    check("honest bus: one peak at the impulse", len(p) == 1 and p[0][0] == AT, f"{p}")
    check("honest bus: amplitude 0.1*(1+send)", len(p) == 1 and abs(p[0][1] - AMP * (1 + G)) < 1e-6, f"{p[0][1] if p else None:.6f} vs {AMP * (1 + G):.6f}")
    check("graph latency = declared (7)", rig.get("graph_latency") == 7 and rig.get("chain_latency") == 7, f"graph {rig.get('graph_latency')} chain {rig.get('chain_latency')}")

    # dishonest bus (control): declares 0, delays 7
    x, rig = render(tb, "sends_dishonest.wav", send1_db=-6, bus1_delay_actual=7, bus1_delay_declared=0)
    p = peaks(x)
    check("dishonest bus (control): two peaks", len(p) == 2 and p[1][0] - p[0][0] == 7, f"{p}")

    # return strip
    x, _ = render(tb, "sends_return.wav", send1_db=-6, bus1_delay_actual=7, bus1_delay_declared=7, bus1_gain_db=-6)
    p = peaks(x)
    check("return strip -6 dB", len(p) == 1 and abs(p[0][1] - AMP * (1 + G * G)) < 1e-6, f"{p}")

    # both buses at once, and a bus with nothing sent into it
    x, _ = render(tb, "sends_two.wav", send1_db=-6, send2_db=-12, bus1_delay_actual=7, bus1_delay_declared=7, bus2_delay_actual=3, bus2_delay_declared=3)
    p = peaks(x)
    want = AMP * (1 + G + 10 ** (-12 / 20))
    check("two buses of different delay meet in one sample", len(p) == 1 and p[0][0] == AT and abs(p[0][1] - want) < 1e-6, f"{p} want {want:.6f}")

    # a send without a bus is not connected: the output is the dry impulse
    x, _ = render(tb, "sends_nobus.wav", send1_db=-6)
    p = peaks(x)
    check("send into no bus changes nothing", len(p) == 1 and abs(p[0][1] - AMP) < 1e-6, f"{p}")

    # a bus that nothing is sent to adds nothing
    x, _ = render(tb, "sends_nosend.wav", bus1_delay_actual=7, bus1_delay_declared=7)
    p = peaks(x)
    check("bus without a send changes nothing", len(p) == 1 and abs(p[0][1] - AMP) < 1e-6, f"{p}")

    print("PASS" if bad == 0 else "FAIL")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
