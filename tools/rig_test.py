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
IMPULSE_AT = 1000     # samples
EARLY_TOL = 1         # peak is an integer; the true peak may sit between two samples
LATE_LIMIT = 28
NULL_DB = -100.0      # residual limit against the peak (float sums should be exact; this leaves room)
CONTROL_DB = -60.0    # the no-compensation model must be at least this far from null


def db(x):
    return 20 * np.log10(x) if x > 1e-12 else -240.0


class Rig:
    def __init__(self):
        self.tb = TestBench()
        self.dir = os.path.join(os.environ["APPDATA"], "VstTestBench")

    def render(self, out, **kw):
        args = dict(tail=1.0, out=out)
        if kw.get("source") != "impulse":
            args["events"] = EVENTS
        args.update(kw)
        r = self.tb.call("rig_render", **args)
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


def classify(declared, measured, late_limit):
    if measured is None:
        return "NO-RESPONSE", 0
    excess = measured - declared
    if excess < -EARLY_TOL:
        return "OVER-DECLARED", excess
    if excess > late_limit:
        return "LATE", excess
    return "ok", excess


def measure(rig, amp, **slots):
    """(declared, measured peak position) for a unit impulse through the given slots, compensation off."""
    res = rig.render("lat_imp.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=amp,
                     compensate=False, tail=0.1, **slots)
    x = rig.read("lat_imp.wav").reshape(-1, 2)
    e = (x ** 2).sum(axis=1)
    if e.max() < 1e-14:            # nothing came out (a gate, a threshold): there is no peak to locate
        return res["chain_latency"], None
    return res["chain_latency"], int(np.argmax(e)) - IMPULSE_AT


def latency_accuracy(rig, a, bad):
    # controls: through the same path, with a device whose true delay is known
    ctl = [("honest   (actual 7, declared 7)", 7, 7, "ok"),
           ("dishonest (actual 7, declared 12)", 7, 12, "OVER-DECLARED"),
           ("under-declared (actual 7, declared 3)", 7, 3, "ok")]
    for name, actual, declared, want in ctl:
        d, m = measure(rig, 0.1, delay_actual=actual, delay_declared=declared)
        verdict, _ = classify(d, m, a.late_limit)
        good = (m == actual) and (d == declared) and (verdict == want)
        print(f"  control {name}: declared {d} measured {m} -> {verdict}  {'ok' if good else 'WRONG'}")
        if not good:
            bad.append(f"latency control '{name}': declared {d} measured {m} verdict {verdict}, wanted {want}")

    levels = [float(v) for v in a.levels.split(",")]
    targets = [(n, dict(insert=n), True) for n in a.fx]
    if a.master:
        # information only: the peak of a cascade of nonlinear stages is not a delay (it moves with level)
        targets.append((f"{a.insert} + {a.master} (info only)", dict(insert=a.insert, master=a.master), False))
    for name, slots, judged in targets:
        rows = [measure(rig, lv, **slots) for lv in levels]
        declared = rows[0][0]
        ms = [m for _, m in rows]
        seen = [m for m in ms if m is not None]
        verdict, excess = classify(declared, seen[len(seen) // 2] if seen else None, a.late_limit)
        spread = max(seen) - min(seen) if seen else 0
        line = f"  {name}: declared {declared}, measured peak {ms} at {[f'{20*np.log10(l):.0f} dB' for l in levels]}"
        print(f"{line} -> {verdict}, excess {excess:+d}" + (f", LEVEL-DEPENDENT (spread {spread})" if spread > EARLY_TOL else ""))
        if not judged:
            continue
        if verdict != "ok":
            bad.append(f"{name}: {verdict} (declared {declared}, measured {ms})")
        if spread > EARLY_TOL:
            bad.append(f"{name}: measured delay depends on level (spread {spread})")


def abs_data(rig, name):
    return os.path.join(rig.dir, name)


def make_state(rig, plugin, name, **params):
    """Configure `plugin` in the bench (the way a person would) and save its state; returns the file path."""
    rig.tb.call("load_plugin", role="fx", name=plugin)
    while rig.tb.call("status")["busy"]:
        time.sleep(0.15)
    for k, v in params.items():
        r = rig.tb.call("set_param", role="fx", name=k, value=v)
        if not r.get("ok", True):
            raise RuntimeError(f"set_param {k}: {r.get('error')}")
    path = abs_data(rig, name)
    r = rig.tb.call("save_state", role="fx", path=path)
    if not r.get("ok", True):
        raise RuntimeError(f"save_state: {r.get('error')}")
    return path


def state_roundtrip(rig, plugin, bad):
    params = {"Drive": 0.9, "Tone": 0.3}
    state = make_state(rig, plugin, "rt_state.bin", **params)
    rig.render("rt_a.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, insert=plugin,
               insert_state=state, tail=0.5)
    rig.render("rt_b.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, insert=plugin,
               insert_params=params, tail=0.5)
    rig.render("rt_c.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, insert=plugin, tail=0.5)
    a, b, c = (rig.read(f"rt_{k}.wav") for k in "abc")
    same = np.array_equal(a, b)
    moved = not np.array_equal(a, c)
    print(f"  state round-trip ({plugin}, {params}): saved state == same parameters by name: {same}; "
          f"the parameters do change the sound: {moved}")
    if not same:
        bad.append(f"state round-trip: the restored state renders {db(np.abs(a - b).max() / np.abs(a).max()):.1f} dB re peak "
                   f"away from the same parameters set by name")
    if not moved:
        bad.append("state round-trip: the parameters did not change the sound, so the comparison proves nothing")


def split_vs_whole(rig, plugin, bad):
    s1 = make_state(rig, plugin, "sp_1.bin", Model=0.339, Drive=0.9)       # Marshall JCM800
    s2 = make_state(rig, plugin, "sp_2.bin", Model=0.678, Drive=0.5)       # BOSS MT-2
    common = dict(source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1)
    whole = rig.render("sp_whole.wav", insert=plugin, insert_state=s1, master=plugin, master_state=s2, tail=0.5, **common)
    first = rig.render("sp_first.wav", insert=plugin, insert_state=s1, tail=0.5, **common)
    second = rig.render("sp_second.wav", source="file", source_path=abs_data(rig, "sp_first.wav"),
                        insert=plugin, insert_state=s2, tail=0.0)
    w = rig.read("sp_whole.wav").reshape(-1, 2)
    x = rig.read("sp_second.wav").reshape(-1, 2)
    n = min(len(w), len(x))
    diff = db(np.abs(w[:n] - x[:n]).max() / np.abs(w[:n]).max())
    print(f"  split vs whole ({plugin}: JCM800 then MT-2): latencies whole {whole['chain_latency']} = "
          f"{first['chain_latency']} + {second['chain_latency']}, difference {diff:.1f} dB re peak over {n} samples")
    if whole["chain_latency"] != first["chain_latency"] + second["chain_latency"]:
        bad.append("split vs whole: the declared latencies do not add up")
    if diff > -100:
        bad.append(f"split vs whole: differ by {diff:.1f} dB re peak")

    # control: the same split with the WRONG state on the second stage must not match, else this comparison
    # could not tell a faithful split from an unfaithful one
    rig.render("sp_wrong.wav", source="file", source_path=abs_data(rig, "sp_first.wav"),
               insert=plugin, insert_state=s1, tail=0.0)
    y = rig.read("sp_wrong.wav").reshape(-1, 2)
    m = min(len(w), len(y))
    wrong = db(np.abs(w[:m] - y[:m]).max() / np.abs(w[:m]).max())
    print(f"    control, wrong state on the second stage: {wrong:.1f} dB re peak (must stay above -60)")
    if wrong < -60:
        bad.append(f"split vs whole: the control is not discriminating ({wrong:.1f} dB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst", default="PAGANIHANDS")
    ap.add_argument("--insert", default="Legacy Distortion")
    ap.add_argument("--master", default="Legacy Distortion")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--fx", action="append", help="plugin(s) for the declared-vs-actual latency test (default: --insert)")
    ap.add_argument("--levels", default="0.001,0.01,0.1,0.5", help="impulse amplitudes (linear)")
    ap.add_argument("--late-limit", type=int, default=LATE_LIMIT)
    a = ap.parse_args()
    a.fx = a.fx or [a.insert]
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

    # 4: declared vs actual latency
    latency_accuracy(rig, a, bad)

    # 5, 6: state round-trip, split vs whole (they use the bench's own plugin instance to make states)
    state_roundtrip(rig, a.insert, bad)
    split_vs_whole(rig, a.insert, bad)

    print("PASS" if not bad else "FAIL: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
