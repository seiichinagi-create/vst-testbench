#!/usr/bin/env python3
"""Fixed-rig checks (MIDI -> VSTi -> insert -> master, offline) against a running TestBench.

  python tools/rig_test.py [--inst PAGANIHANDS] [--insert "Legacy Distortion"] [--master "Legacy Distortion"] [--repeat 5]

 1. latency   declared latency of every stage; the graph's total must equal their sum (the rig itself
              re-prepares until it does and refuses to render otherwise; see prepare_attempts)
 2. repeat    the same render N times must be bit-identical
 3. pdc null  the VSTi also goes straight to the output (dry_parallel): the host graph must delay that
              short path by the chain's latency. Rendered stems must add up to it:
                  parallel graph  ==  dry stem + wet stem        (null residual)
              Control: the sum WITHOUT delay compensation must NOT null. Otherwise the test could not
              tell a compensating host from a non-compensating one and its pass means nothing.
Not yet: declared-vs-actual latency (impulse), instance interference, state round-trip, master tail.
"""
import argparse, hashlib, os, struct, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench

EVENTS = [{"t": 0.0, "type": "note_on", "ch": 1, "note": 57, "vel": 100, "dur": 1.0}]
NULL_DB = -100.0      # residual limit against the peak (float sums should be exact; this leaves room)
CONTROL_DB = -60.0    # the no-compensation model must be at least this far from null


def db(x):
    return 20 * np.log10(x) if x > 1e-12 else -240.0


class Rig:
    def __init__(self):
        self.tb = TestBench()
        self.dir = os.path.join(os.environ["APPDATA"], "VstTestBench")

    def render(self, out, **kw):
        r = self.tb.call("rig_render", events=EVENTS, tail=1.0, out=out, **kw)
        if not r.get("ok"):
            raise RuntimeError("start: %s" % r.get("error"))
        while self.tb.call("status")["busy"]:
            time.sleep(0.15)
        res = self.tb.call("status")["rig"]
        if not res.get("ok"):
            raise RuntimeError("render: %s" % res.get("error"))
        return res

    def read(self, out):
        b = open(os.path.join(self.dir, out), "rb").read()
        i = b.find(b"data")
        n = struct.unpack("<I", b[i + 4:i + 8])[0]
        return np.frombuffer(b[i + 8:i + 8 + n], dtype="<f4").astype(np.float64)

    def md5(self, out):
        return hashlib.md5(open(os.path.join(self.dir, out), "rb").read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst", default="PAGANIHANDS")
    ap.add_argument("--insert", default="Legacy Distortion")
    ap.add_argument("--master", default="Legacy Distortion")
    ap.add_argument("--repeat", type=int, default=5)
    a = ap.parse_args()
    chain = dict(inst=a.inst, insert=a.insert, master=a.master)
    rig = Rig()
    bad = []

    # 1 + 2: latency, repeatability
    hashes, res, retries = [], None, 0
    for i in range(a.repeat):
        res = rig.render(f"rig_test_{i}.wav", **chain)
        retries += res["prepare_attempts"] - 1
        hashes.append(rig.md5(f"rig_test_{i}.wav"))
    for s in res["stages"]:
        print(f"  {s['role']:7s} {s['name']:24s} latency {s['latency']} samples")
    print(f"  chain {res['chain_latency']}  graph {res['graph_latency']}  peak {res['peak_db']:.1f} dB")
    if res["chain_latency"] != res["graph_latency"]:
        bad.append("graph latency differs from the sum of the stages")
    if len(set(hashes)) != 1:
        bad.append(f"{len(set(hashes))} different renders out of {a.repeat}")
    if retries:
        print(f"  note: the graph needed a second prepare in {retries} of {a.repeat} renders "
              f"(a plugin declared its latency late; the rig re-prepares until the graph agrees)")
    print("  latency/repeat:", "PASS" if not bad else "FAIL")

    # 3: PDC null
    L = res["chain_latency"]
    rig.render("pdc_dry.wav", inst=a.inst)
    rig.render("pdc_wet.wav", **chain)
    par = rig.render("pdc_par.wav", dry_parallel=True, **chain)
    dry, wet, mix = rig.read("pdc_dry.wav"), rig.read("pdc_wet.wav"), rig.read("pdc_par.wav")
    if not (len(dry) == len(wet) == len(mix)):
        bad.append(f"stem lengths differ: dry {len(dry)} wet {len(wet)} parallel {len(mix)}")
    else:
        peak = np.max(np.abs(dry + wet))
        null = db(np.max(np.abs(mix - (dry + wet))) / peak)
        # what a host WITHOUT compensation would give: the dry path arrives L samples early
        early = np.concatenate([dry[2 * L:], np.zeros(2 * L)])     # stereo interleaved: 2 values per sample
        nocomp = db(np.max(np.abs(mix - (early + wet))) / peak)
        print(f"  pdc null: residual {null:.1f} dB (limit {NULL_DB:.0f});  "
              f"control, no compensation: {nocomp:.1f} dB (must stay above {CONTROL_DB:.0f}); chain latency {L}")
        if L == 0:
            bad.append("pdc null: the chain declares no latency, nothing to compensate (pick a latency-declaring insert)")
        if null > NULL_DB:
            bad.append(f"pdc null: residual {null:.1f} dB")
        if L > 0 and nocomp < CONTROL_DB:
            bad.append(f"pdc control is not discriminating ({nocomp:.1f} dB)")

    print("PASS" if not bad else "FAIL: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
