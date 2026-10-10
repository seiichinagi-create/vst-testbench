#!/usr/bin/env python3
"""Fixed-rig checks (MIDI -> VSTi -> insert -> master, offline) against a running TestBench.

  python tools/rig_test.py [--inst PAGANIHANDS] [--insert "Legacy Distortion"] [--master "Legacy Distortion"] [--repeat 5]

Minimal version: (1) the declared latency of every stage, as the graph reports it,
(2) the same render repeated N times must be bit-identical.
Not yet: PDC null test, instance interference, state round-trip, master tail.
"""
import argparse, hashlib, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst", default="PAGANIHANDS")
    ap.add_argument("--insert", default="Legacy Distortion")
    ap.add_argument("--master", default="Legacy Distortion")
    ap.add_argument("--repeat", type=int, default=5)
    a = ap.parse_args()

    tb = TestBench()
    data = os.path.join(os.environ["APPDATA"], "VstTestBench")
    events = [{"t": 0.0, "type": "note_on", "ch": 1, "note": 57, "vel": 100, "dur": 1.0}]
    hashes, rig = [], None
    for i in range(a.repeat):
        out = f"rig_test_{i}.wav"
        r = tb.call("rig_render", events=events, tail=1.0, out=out,
                    inst=a.inst, insert=a.insert, master=a.master)
        if not r.get("ok"):
            print("FAIL start:", r.get("error")); return 1
        while tb.call("status")["busy"]:
            time.sleep(0.2)
        rig = tb.call("status")["rig"]
        if not rig.get("ok"):
            print("FAIL render:", rig.get("error")); return 1
        hashes.append(hashlib.md5(open(os.path.join(data, out), "rb").read()).hexdigest())

    for s in rig["stages"]:
        print(f"  {s['role']:7s} {s['name']:24s} latency {s['latency']} samples")
    print(f"  chain {rig['chain_latency']}  graph {rig['graph_latency']}  peak {rig['peak_db']:.1f} dB")
    bad = []
    if rig["chain_latency"] != rig["graph_latency"]:
        bad.append("graph latency differs from the sum of the stages")
    if len(set(hashes)) != 1:
        bad.append(f"{len(set(hashes))} different renders out of {a.repeat}")
    print("PASS" if not bad else "FAIL: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
