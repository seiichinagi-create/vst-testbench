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

 4. declared vs actual latency   a unit impulse goes through the plug-in with compensation off; the PEAK position is
              compared with the declared latency, at four levels (the position must not depend on the level):
                  measured < declared - 1      FAIL  over-declared: a compensating host trims real signal
                  measured > declared + 28     FAIL  declared far too small (28 = the 2026-10-09 ruling; --late-limit)
                  in between                   ok; the excess is printed (a plug-in's own model delay is expected)
              A cascade is information only (the peak of two nonlinear stages moves with level). Controls first: a pure
              delay with a known true value and a declared value of our choosing; honest and dishonest must be told apart.
 5. state round-trip   a plug-in configured in the bench and saved (save_state) must render exactly like a fresh instance
              given the same parameters by name (<role>_params).
 6. split vs whole     insert + master in one render must equal the two stages rendered one after the other, the first
              one's wav fed to the second (source=file). Control: the wrong state on the second stage must NOT match.
 7. master tail        the time a master effect keeps ringing (60 dB down) against the tail it DECLARES; a longer render
              must not change the samples of a shorter one. Controls: a decay of known length, honest and dishonest.
 8. three tracks       three tracks summed into a master: the graph lines them up by declared latency. Dummy delays (7/0/3)
              meet in one sample; a track that declares less than it delays must break that (control). With real plug-ins
              the sum of the separate renders must equal the three-track render.
 9. instance interference   two instances of one instrument playing at once must sum to their separate renders. A plug-in
              that fails is a NOTE (a finding about the plug-in); the rig is right where Matryoshka Guitar sums at -147 dB.
10. variable block sizes   the host may hand a plug-in any block length from 1 to the prepared maximum, changing from call
              to call. The output must not depend on how the stream was cut: an irregular block pattern must equal the
              fixed-block render. Control: a parameter change applied at the start of the block it falls in (the quantised
              mode) does depend on the cut, so the same comparison must fail there.
11. transport info     tempo, time signature and position as the plug-in receives them (a probe writes them into the audio):
              the bpm sent is the bpm seen, and the position advances by bpm/60/sr quarter notes per sample.
12. in-block automation   a parameter change at sample N must take effect at sample N whatever the block pattern. The
              quantised mode is the control: it moves the change to a block boundary.
14. ARA playback       the plug-in is handed an ARA DOCUMENT (one audio source, one playback region = the clip) instead of
              a stream, is bound to it as a playback renderer and renders it. An ARA plug-in that has been given nothing to
              edit must be transparent: its output equals the plain clip render (same clip_start / clip_offset /
              clip_length) sample for sample, with no extra latency. Control: the same comparison against a clip one sample
              later must fail.
15. ARA under variation   the same ARA render with the stream cut into irregular blocks (1..512 samples), and with the render
              running at 44.1 kHz and 96 kHz on a file of that rate, must equal the plain clip render. A file whose rate is NOT
              the render's (48 kHz source, 96 kHz render) is converted by the plug-in: not compared sample for sample; its
              timing (where a click lands) and level are reported against the ideal.
13. audio clips        a stereo audio clip on a track: where it sits on the timeline (clip_start), where in the file it starts
              (clip_offset), how long it is (clip_length) and its gain. Marks at known places in the file must come out at
              the computed places and levels, and nothing outside the clip may be heard. The clip is also the model of an
              ARA playback region.
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


def decay_seconds(x, at=IMPULSE_AT, db_drop=60.0):
    """Seconds after the impulse until the response (10 ms RMS windows) last stays above peak - db_drop."""
    m = x.reshape(-1, 2).mean(axis=1)
    win = int(0.01 * SR_)
    n = len(m) // win
    env = 10 * np.log10((m[:n * win].reshape(n, win) ** 2).mean(axis=1) + 1e-30)
    above = np.nonzero(env > env.max() - db_drop)[0]
    return (above.max() + 1) * win / SR_ - at / SR_ if above.size else 0.0


SR_ = 48000
TAIL_LEEWAY = 0.1      # seconds: a measured decay this much longer than the declared tail is a cut-off tail


def tail_check(rig, a, bad, notes):
    # controls: a decay of known length (60 dB in 0.8 s) that declares its tail honestly or not
    for name, declared, want in (("honest   (decays in 0.8 s, declares 0.8 s)", 0.8, "ok"),
                                 ("dishonest (decays in 0.8 s, declares 0.1 s)", 0.1, "TAIL-CUT")):
        rig.render("tail_c.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1,
                   master_tail_t60=0.8, master_tail_declared=declared, tail=3.0)
        res = rig.tb.call("status")["rig"]
        t = decay_seconds(rig.read("tail_c.wav"))
        dec = next(s_["tail"] for s_ in res["stages"] if s_["role"] == "master")
        verdict = "TAIL-CUT" if t > dec + TAIL_LEEWAY else "ok"
        good = verdict == want and abs(t - 0.8) < 0.05
        print(f"  control {name}: measured decay {t:.2f} s, declared {dec:.2f} s -> {verdict}  {'ok' if good else 'WRONG'}")
        if not good:
            bad.append(f"tail control '{name}': measured {t:.2f} s, declared {dec:.2f} s, verdict {verdict}, wanted {want}")

    # the render length must not change what came before: 1 s of tail vs 4 s of tail
    rig.render("tail_s.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, master_tail_t60=0.8, tail=1.0)
    short = rig.read("tail_s.wav")
    rig.render("tail_l.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, master_tail_t60=0.8, tail=4.0)
    long_ = rig.read("tail_l.wav")
    same = np.array_equal(short, long_[:len(short)])
    print(f"  a longer render leaves the earlier samples unchanged: {same}")
    if not same:
        bad.append("tail: the samples of a short render changed when the render was made longer")

    for fx in a.master_fx:
        try:
            rig.render("tail_fx.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, master=fx, tail=8.0)
        except RuntimeError as e:
            print(f"  {fx}: skipped ({str(e)[:60]})")
            continue
        res = rig.tb.call("status")["rig"]
        x = rig.read("tail_fx.wav")
        if np.abs(x).max() < 1e-7:
            print(f"  {fx}: no response to the impulse")
            continue
        t = decay_seconds(x)
        dec = next(s_["tail"] for s_ in res["stages"] if s_["role"] == "master")
        if dec is None:           # JSON has no infinity: the plugin declares an unbounded tail, so a host never cuts it
            print(f"  {fx}: response falls 60 dB in {t:.2f} s, declared tail: unbounded (never cut) -> ok")
            continue
        cut = t > dec + TAIL_LEEWAY
        print(f"  {fx}: response falls 60 dB in {t:.2f} s, declared tail {dec:.2f} s -> {'TAIL-CUT' if cut else 'ok'}")
        if cut:
            notes.append(f"{fx}: rings for {t:.2f} s but declares a tail of {dec:.2f} s (a host that stops at the declared tail cuts it)")


def three_tracks(rig, a, bad):
    AT, AMP = IMPULSE_AT, 0.1

    def track(delay_actual, delay_declared, at=AT):
        return dict(source="impulse", impulse_at=at, impulse_amp=AMP, delay_actual=delay_actual, delay_declared=delay_declared)

    # honest delays 7 / 0 / 3 and a master that delays 2: the three impulses must meet in one sample
    honest = [track(7, 7), track(0, 0), track(3, 3)]
    r = rig.render("tt_a.wav", tracks=honest, master_delay_actual=2, master_delay_declared=2, tail=0.2)
    x = rig.read("tt_a.wav").reshape(-1, 2)[:, 0]
    peak_at, peak = int(np.argmax(np.abs(x))), float(np.abs(x).max())
    rest = float(np.abs(np.delete(x, peak_at)).max())
    print(f"  3 tracks, honest delays 7/0/3 + master 2: track latencies {r['track_latencies']}, total {r['chain_latency']}; "
          f"one peak {peak:.4f} at {peak_at} (expected {3 * AMP:.4f} at {AT}), everything else {db(rest):.0f} dB")
    if peak_at != AT or abs(peak - 3 * AMP) > 1e-5 or rest > 1e-6:
        bad.append(f"three tracks: the impulses did not meet (peak {peak:.4f} at {peak_at}, rest {rest:.2e})")
    if r["track_latencies"] != [7, 0, 3]:
        bad.append(f"three tracks: track latencies {r['track_latencies']}, expected [7, 0, 3]")

    # control: the first track delays 7 but declares 0 -> the graph cannot line it up
    liar = [track(7, 0), track(0, 0), track(3, 3)]
    rig.render("tt_b.wav", tracks=liar, master_delay_actual=2, master_delay_declared=2, tail=0.2)
    y = rig.read("tt_b.wav").reshape(-1, 2)[:, 0]
    met = float(np.abs(y).max())
    print(f"    control, track 1 delays 7 but declares 0: strongest sample {met:.4f} (must stay below {3 * AMP:.4f})")
    if met > 3 * AMP - 1e-3:
        bad.append("three tracks: the control (a lying track) still lined up; the test could not tell")

    # real plugins: an instrument through Legacy Distortion, a second instance of it alone, and an impulse; the sum of the
    # separately rendered tracks must equal the three-track render
    ev1 = [{"t": 0.0, "type": "note_on", "ch": 1, "note": 57, "vel": 100, "dur": 0.6}]
    ev2 = [{"t": 0.3, "type": "note_on", "ch": 1, "note": 64, "vel": 90, "dur": 0.6}]
    t1 = dict(inst=a.multi_inst, events=ev1, insert=a.insert)
    t2 = dict(inst=a.multi_inst, events=ev2)
    t3 = dict(source="impulse", impulse_at=AT, impulse_amp=0.2)
    rig.render("tt_all.wav", tracks=[t1, t2, t3], tail=1.0)
    for i, t in enumerate((t1, t2, t3)):
        rig.render(f"tt_{i}.wav", tracks=[t], tail=1.0)
    allx = rig.read("tt_all.wav")
    n = len(allx)

    def padded(name):                      # each track alone is rendered to its own (shorter) length
        x = rig.read(name)[:n]
        return np.concatenate([x, np.zeros(n - len(x))])

    parts = sum(padded(f"tt_{i}.wav") for i in range(3))
    peak = np.abs(allx).max()
    diff = db(np.abs(allx[:n] - parts[:n]).max() / peak)
    # control: the same sum with track 1 shifted by its latency (a host that did not compensate)
    L = 4
    shifted = parts.copy()
    one = padded("tt_0.wav")
    shifted[:n - 2 * L] = shifted[:n - 2 * L] - one[:n - 2 * L] + one[2 * L:n]
    ctl = db(np.abs(allx[:n] - shifted[:n]).max() / peak)
    print(f"  3 tracks with plugins: sum of separate renders vs the three-track render: {diff:.1f} dB re peak; "
          f"control, track 1 shifted by its latency: {ctl:.1f} dB (must stay above -60)")
    if diff > -100:
        bad.append(f"three tracks: the sum of the separate renders differs by {diff:.1f} dB re peak")
    if ctl < -60:
        bad.append(f"three tracks: the control is not discriminating ({ctl:.1f} dB)")


def write_dc(rig, name, value=0.5, seconds=0.5):
    """A constant-level stereo float wav in the bench's data folder: a source whose output shows only what the stage did."""
    n = int(SR_ * seconds)
    data = np.full(n * 2, value, dtype="<f4").tobytes()
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " +
           struct.pack("<IHHIIHH", 16, 3, 2, SR_, SR_ * 8, 8, 32) + b"data" + struct.pack("<I", len(data)))
    path = abs_data(rig, name)
    with open(path, "wb") as f:
        f.write(hdr + data)
    return path


ODD_BLOCKS = [37, 512, 1, 128, 61, 300, 7, 450]


def variable_blocks(rig, a, bad, notes):
    # plug-ins: a fixed 512-sample render against one cut into irregular pieces (1..512 samples)
    ev = [{"t": 0.0, "type": "note_on", "ch": 1, "note": 57, "vel": 100, "dur": 0.4},
          {"t": 0.21, "type": "note_on", "ch": 1, "note": 64, "vel": 90, "dur": 0.3}]
    cases = [("Legacy Distortion (impulse)", dict(source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, insert=a.insert)),
             (f"{a.multi_inst} + {a.insert} (MIDI)", dict(inst=a.multi_inst, events=ev, insert=a.insert))]
    for name, kw in cases:
        rig.render("vb_fixed.wav", block=512, tail=0.5, **kw)
        res = rig.render("vb_odd.wav", block=512, block_pattern=ODD_BLOCKS, tail=0.5, **kw)
        x, y = rig.read("vb_fixed.wav"), rig.read("vb_odd.wav")
        same = np.array_equal(x, y)
        d = db(np.abs(x - y).max() / max(np.abs(x).max(), 1e-12))
        print(f"  {name}: blocks of {res['block_min']}..{res['block_max']} samples ({res['blocks']} calls) vs fixed 512: "
              f"{'identical' if same else f'DIFFERENT, {d:.1f} dB re peak'}")
        if not same:
            bad.append(f"variable blocks: {name} depends on how the stream is cut ({d:.1f} dB re peak)")
        if res["block_min"] != 1:
            bad.append("variable blocks: the 1-sample block of the pattern was never used")

    # plug-ins that are expected to be block-size independent but are not are findings about the plug-in
    for plug in a.interference:
        try:
            kw = dict(inst=plug, events=ev)
            rig.render("vb_f.wav", block=512, tail=0.5, **kw)
            rig.render("vb_o.wav", block=512, block_pattern=ODD_BLOCKS, tail=0.5, **kw)
        except RuntimeError as e:
            print(f"  {plug}: skipped ({str(e)[:50]})")
            continue
        x, y = rig.read("vb_f.wav"), rig.read("vb_o.wav")
        if np.abs(x).max() < 1e-7:
            continue
        same = np.array_equal(x, y)
        d = db(np.abs(x - y).max() / np.abs(x).max())
        print(f"  {plug}: irregular blocks vs fixed 512: {'identical' if same else f'DIFFERENT, {d:.1f} dB re peak'}")
        if not same:
            notes.append(f"{plug}: its output depends on how the stream is cut into blocks ({d:.1f} dB re peak)")

    # control: a change applied at the start of the block it falls in depends on the cut
    dc = write_dc(rig, "vb_dc.wav")
    auto = [dict(track=0, role="insert", param="gain", points=[[0.0123, 0.5]])]
    rig.render("vb_q1.wav", source="file", source_path=dc, probe="gain", automation=auto, automation_quantised=True,
               block=512, tail=0.0)
    rig.render("vb_q2.wav", source="file", source_path=dc, probe="gain", automation=auto, automation_quantised=True,
               block=512, block_pattern=ODD_BLOCKS, tail=0.0)
    differs = not np.array_equal(rig.read("vb_q1.wav"), rig.read("vb_q2.wav"))
    print(f"    control, a parameter change quantised to the block start: depends on the cut: {differs} (must be True)")
    if not differs:
        bad.append("variable blocks: the control did not react to the block pattern, so the comparison proves nothing")


def transport_info(rig, bad):
    SIG = [3, 4]
    r = rig.render("tr_probe.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, probe="playhead",
                   bpm=133.0, time_sig=SIG, tail=0.5, block_pattern=ODD_BLOCKS)
    x = rig.read("tr_probe.wav").reshape(-1, 2)
    bpm = x[:, 0] * 1000.0
    ppq = x[:, 1] * 1000.0
    per = 133.0 / 60.0 / SR_
    slope = (ppq[-1] - ppq[0]) / (len(ppq) - 1)
    jump = np.abs(np.diff(ppq) - per).max()          # no jump at any block boundary
    ok_bpm = np.allclose(bpm, 133.0, atol=1e-3)
    ok_pos = abs(slope - per) / per < 1e-3 and ppq[0] == 0.0 and jump / per < 0.05
    print(f"  transport: bpm seen {bpm.min():.3f}..{bpm.max():.3f} (sent 133); position starts at {ppq[0]:.3f} quarter notes, "
          f"advances {slope:.4e} per sample (expected {per:.4e}), largest step error {jump / per * 100:.2f} % of a sample's worth")
    if not ok_bpm:
        bad.append(f"transport: the plug-in saw bpm {bpm.min():.3f}..{bpm.max():.3f}, 133 was sent")
    if not ok_pos:
        bad.append("transport: the position did not advance smoothly from 0 at the expected rate across irregular blocks")
    # control: with the host saying nothing the probe must say so (-1), or the read-back could not tell
    # (the probe reads the play head; there is always one in this rig, so the control is the changed value)
    r2 = rig.render("tr_probe2.wav", source="impulse", impulse_at=IMPULSE_AT, impulse_amp=0.1, probe="playhead",
                    bpm=90.0, tail=0.1)
    bpm2 = rig.read("tr_probe2.wav").reshape(-1, 2)[:, 0] * 1000.0
    print(f"    control, bpm 90 sent: seen {bpm2.min():.3f}..{bpm2.max():.3f}")
    if not np.allclose(bpm2, 90.0, atol=1e-3):
        bad.append("transport: the probe did not follow a changed tempo")


def block_automation(rig, bad):
    dc = write_dc(rig, "au_dc.wav")
    times = (0.0123, 0.0456, 0.0789)
    auto = [dict(track=0, role="insert", param="gain", points=[[times[0], 0.5], [times[1], 1.0], [times[2], 0.25]])]
    expect = [round(t * SR_) for t in times]

    def changes(**kw):
        rig.render("au_out.wav", source="file", source_path=dc, probe="gain", automation=auto, tail=0.0, **kw)
        x = rig.read("au_out.wav").reshape(-1, 2)[:, 0]
        return [int(i) + 1 for i in np.nonzero(np.abs(np.diff(x)) > 1e-9)[0]], x

    for label, kw in (("block 512", dict(block=512)), ("block 4096", dict(block=4096)),
                      ("irregular blocks", dict(block=512, block_pattern=ODD_BLOCKS))):
        got, x = changes(**kw)
        levels = [round(float(x[i]), 3) for i in (expect[0], expect[1], expect[2])]
        ok = got == expect and levels == [0.25, 0.5, 0.125]
        print(f"  automation, {label}: changes at samples {got} (expected {expect}); levels after each {levels}  {'ok' if ok else 'WRONG'}")
        if not ok:
            bad.append(f"automation ({label}): changes at {got}, expected {expect}")

    got, _ = changes(block=512, automation_quantised=True)
    print(f"    control, quantised to the block start (block 512): changes at {got} (must differ from {expect})")
    if got == expect:
        bad.append("automation: the control (block-quantised) landed on the exact samples, so the test could not tell")


def audio_clip(rig, bad):
    # a file with marks at known places: L 1.0 at 0.12 s, R 0.5 at 0.40 s, both 0.2 at 0.75 s
    x = np.zeros((SR_, 2))
    x[int(0.12 * SR_), 0] = 1.0
    x[int(0.40 * SR_), 1] = 0.5
    x[int(0.75 * SR_), :] = 0.2
    data = x.astype("<f4").tobytes()
    path = abs_data(rig, "clip_marks.wav")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " +
                struct.pack("<IHHIIHH", 16, 3, 2, SR_, SR_ * 8, 8, 32) + b"data" + struct.pack("<I", len(data)) + data)

    def marks(**kw):
        rig.render("clip_out.wav", source="file", source_path=path, tail=0.0, **kw)
        y = rig.read("clip_out.wav").reshape(-1, 2)
        found = [(int(i), round(float(y[i, 0]), 3), round(float(y[i, 1]), 3)) for i in np.nonzero(np.abs(y).max(axis=1) > 1e-6)[0]]
        return found, len(y)

    whole, n_whole = marks()
    want_whole = [(int(0.12 * SR_), 1.0, 0.0), (int(0.40 * SR_), 0.0, 0.5), (int(0.75 * SR_), 0.2, 0.2)]
    # clip: start 0.25 s on the timeline, from 0.10 s into the file, 0.35 s long (covers file time 0.10..0.45), -6.0206 dB
    clip, n_clip = marks(clip_start=0.25, clip_offset=0.10, clip_length=0.35, clip_gain_db=-6.0206)
    t_l = round((0.25 + (0.12 - 0.10)) * SR_)
    t_r = round((0.25 + (0.40 - 0.10)) * SR_)
    want_clip = [(t_l, 0.5, 0.0), (t_r, 0.0, 0.25)]
    print(f"  audio clip: whole file marks at {[m[0] for m in whole]}; clip (start 0.25 s, offset 0.10 s, length 0.35 s, -6 dB) "
          f"marks at {[m[0] for m in clip]} levels {[m[1:] for m in clip]}, length {n_clip / SR_:.4f} s "
          f"(expected {[m[0] for m in want_clip]} and {(0.25 + 0.35):.4f} s)")
    if whole != want_whole or n_whole != SR_:
        bad.append(f"audio clip: the whole file did not come out as it went in ({whole})")
    if clip != want_clip or abs(n_clip - round(0.60 * SR_)) > 1:
        bad.append(f"audio clip: placed marks {clip} (length {n_clip}), expected {want_clip}")


def write_float_wav(path, x, rate):
    data = x.astype("<f4").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " +
                struct.pack("<IHHIIHH", 16, 3, 2, rate, rate * 8, 8, 32) + b"data" + struct.pack("<I", len(data)) + data)


def ara_test_signal(rate, seconds=2.0):
    t = np.arange(int(rate * seconds)) / rate
    x = np.zeros((len(t), 2))
    x[:, 0] = 0.2 * np.sin(2 * np.pi * 440 * t) * (t < 1.0)
    x[:, 1] = 0.2 * np.sin(2 * np.pi * 660 * t) * (t >= 1.0)
    x[int(0.5 * rate), :] += 0.5
    return x


def ara_variation(rig, a, bad, notes):
    if not os.path.exists(a.ara):
        return
    name = a.ara_name
    # 1. irregular blocks
    path = abs_data(rig, "ara_in.wav")
    write_float_wav(path, ara_test_signal(SR_), SR_)
    try:
        rig.render("av_a.wav", source="ara", source_path=path, ara=a.ara, tail=0.5, block=512, block_pattern=ODD_BLOCKS)
    except RuntimeError as e:
        print(f"  ARA variation: skipped ({str(e)[:90]})")
        return
    rig.render("av_r.wav", source="file", source_path=path, tail=0.5, block=512)
    same = np.array_equal(rig.read("av_a.wav"), rig.read("av_r.wav"))
    print(f"  ARA ({name}) with irregular blocks 1..512: {'identical to the plain clip render' if same else 'DIFFERENT'}")
    if not same:
        bad.append(f"ARA ({name}): the output changes when the stream is cut into irregular blocks")

    # 2. other sample rates, on a file of the same rate
    for rate in (44100, 96000):
        p2 = abs_data(rig, f"ara_in_{rate}.wav")
        write_float_wav(p2, ara_test_signal(rate), rate)
        rig.render("av_a.wav", source="ara", source_path=p2, ara=a.ara, tail=0.5, rate=rate)
        rig.render("av_r.wav", source="file", source_path=p2, tail=0.5, rate=rate)
        got, ref = rig.read("av_a.wav"), rig.read("av_r.wav")
        same = np.array_equal(got, ref)
        d = db(np.abs(got[:len(ref)] - ref[:len(got)]).max() / max(np.abs(ref).max(), 1e-12))
        print(f"  ARA ({name}) at {rate} Hz on a {rate} Hz file: {'identical to the plain clip render' if same else f'DIFFERENT, {d:.1f} dB re peak'}")
        if not same:
            bad.append(f"ARA ({name}): not transparent at {rate} Hz ({d:.1f} dB re peak)")

    # 3. a file whose rate is not the render's: the plug-in converts; report where the click lands and how loud it is
    for rate in (96000, 44100):
        rig.render("av_x.wav", source="ara", source_path=path, ara=a.ara, tail=0.5, rate=rate)
        y = rig.read("av_x.wav").reshape(-1, 2)
        click = int(0.5 * rate)
        win = y[click - 64:click + 65]
        peak_at = int(np.argmax(np.abs(win).max(axis=1))) - 64
        # the click is 0.5 on both channels plus the tone; compare the right channel (silent before 1 s: only the click)
        right = np.abs(y[click - 64:click + 65, 1])
        print(f"    48 kHz file at a {rate} Hz render ({name}): length {len(y) / rate:.4f} s (expected 2.5000), the click peaks "
              f"{peak_at:+d} samples from where it should (peak {right.max():.3f}; 0.5 before conversion)")
        if abs(len(y) / rate - 2.5) > 0.01:
            bad.append(f"ARA ({name}): a 48 kHz file at a {rate} Hz render has the wrong length ({len(y) / rate:.4f} s)")
        if abs(peak_at) > 2:
            notes.append(f"{name}: a 48 kHz file rendered at {rate} Hz puts the click {peak_at:+d} samples off")


def ara_playback(rig, a, bad, notes):
    if not os.path.exists(a.ara):
        print(f"  ARA: skipped ({a.ara} is not installed)")
        return
    t = np.arange(2 * SR_) / SR_
    x = np.zeros((2 * SR_, 2))
    x[:, 0] = 0.2 * np.sin(2 * np.pi * 440 * t) * (t < 1.0)
    x[:, 1] = 0.2 * np.sin(2 * np.pi * 660 * t) * (t >= 1.0)
    x[int(0.5 * SR_), :] += 0.5
    data = x.astype("<f4").tobytes()
    path = abs_data(rig, "ara_in.wav")
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + b"fmt " +
                struct.pack("<IHHIIHH", 16, 3, 2, SR_, SR_ * 8, 8, 32) + b"data" + struct.pack("<I", len(data)) + data)

    cases = [("the whole file", {}),
             ("placed at 0.25 s", dict(clip_start=0.25)),
             ("0.5 s taken from 0.3 s into the file, placed at 0.1 s", dict(clip_start=0.1, clip_offset=0.3, clip_length=0.5))]
    for name, clip in cases:
        try:
            res = rig.render("ara_a.wav", source="ara", source_path=path, ara=a.ara, tail=0.5, **clip)
        except RuntimeError as e:
            print(f"  ARA: skipped ({str(e)[:90]})")
            return
        rig.render("ara_r.wav", source="file", source_path=path, tail=0.5, **clip)
        got, ref = rig.read("ara_a.wav"), rig.read("ara_r.wav")
        same = np.array_equal(got, ref)
        lat = res["chain_latency"]
        d = db(np.abs(got[:len(ref)] - ref[:len(got)]).max() / max(np.abs(ref).max(), 1e-12))
        print(f"  ARA ({a.ara_name}), {name}: {'identical to the plain clip render' if same else f'DIFFERENT, {d:.1f} dB re peak'}; "
              f"declared latency {lat}, length {len(got) // 2 / SR_:.4f} s")
        if not same:
            bad.append(f"ARA ({name}): not transparent, {d:.1f} dB re peak away from the plain clip render")
        if lat != 0:
            notes.append(f"{a.ara_name}: declares {lat} samples of latency in ARA playback")

    # control: a clip one sample later is not the same render, so the comparison above could have failed
    rig.render("ara_a.wav", source="ara", source_path=path, ara=a.ara, tail=0.5, clip_start=0.25)
    rig.render("ara_r.wav", source="file", source_path=path, tail=0.5, clip_start=0.25 + 1.0 / SR_)
    got, ref = rig.read("ara_a.wav"), rig.read("ara_r.wav")
    n = min(len(got), len(ref))
    diff = db(np.abs(got[:n] - ref[:n]).max() / np.abs(ref).max())
    print(f"    control, a plain clip one sample later: {diff:.1f} dB re peak away (must stay above -60)")
    if diff < -60:
        bad.append("ARA: the control did not show a one-sample shift, so the comparison could not have told")


def instance_interference(rig, plugs, notes):
    """Two instances of one instrument playing different notes: the two-track render must equal the sum of the
    separate renders (and an idle second instance must not change the first). Findings go to `notes`: a plugin that
    fails is a finding about the plugin, since instruments that do sum (Matryoshka Guitar) show the rig is right."""
    ev1 = [{"t": 0.0, "type": "note_on", "ch": 1, "note": 57, "vel": 100, "dur": 0.6}]
    ev2 = [{"t": 0.3, "type": "note_on", "ch": 1, "note": 64, "vel": 90, "dur": 0.6}]
    idle = [{"t": 0.0, "type": "cc", "ch": 1, "cc": 1, "value": 0}]

    def end_of(evs):
        return max(e["t"] + e.get("dur", 0.0) for e in evs)

    # The rig renders until the last note ends plus `tail`. Renders of different event sets would then differ in
    # length and the shorter one's ring-out would count as silence in the sum (a false -32 dB "interference" for
    # instruments with a long release). Every render therefore gets the same total length.
    total = max(end_of(ev1), end_of(ev2), end_of(idle)) + 1.0

    def run(tracks, tag):
        last = max(end_of(t["events"]) for t in tracks)
        rig.render(f"if_{tag}.wav", tracks=tracks, tail=total - last)
        return rig.read(f"if_{tag}.wav")

    for plug in plugs:
        try:
            t1, t2, ti = dict(inst=plug, events=ev1), dict(inst=plug, events=ev2), dict(inst=plug, events=idle)
            both, a, b = run([t1, t2], "both"), run([t1], "a"), run([t2], "b")
            with_idle = run([t1, ti], "idle")
            again = run([t1, t2], "both2")
        except RuntimeError as e:
            print(f"  {plug}: skipped ({str(e)[:60]})")
            continue
        n = len(both)
        s_ = np.zeros(n)
        for x in (a, b):
            s_[:min(n, len(x))] += x[:n]
        pk = np.abs(both).max()
        if pk < 1e-7:
            print(f"  {plug}: silent")
            continue
        add = db(np.abs(both - s_).max() / pk)
        m = min(len(a), len(with_idle))
        idle_d = db(np.abs(a[:m] - with_idle[:m]).max() / np.abs(a).max())
        repeat = np.array_equal(both, again)
        verdict = "sums" if add < -100 else "DOES NOT SUM"
        print(f"  {plug}: two instances vs the sum of separate renders {add:.1f} dB re peak ({verdict}); "
              f"idle second instance changes the first by {idle_d:.1f} dB; the pair repeats exactly: {repeat}")
        if add >= -100 or not repeat:
            notes.append(f"{plug}: " + ("two playing instances do not sum to the separate renders (" + f"{add:.1f} dB re peak)" if add >= -100 else "")
                         + ("; " if add >= -100 and not repeat else "")
                         + ("the same two-instance render is not repeatable" if not repeat else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inst", default="PAGANIHANDS")
    ap.add_argument("--insert", default="Legacy Distortion")
    ap.add_argument("--master", default="Legacy Distortion")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--fx", action="append", help="plugin(s) for the declared-vs-actual latency test (default: --insert)")
    ap.add_argument("--levels", default="0.001,0.01,0.1,0.5", help="impulse amplitudes (linear)")
    ap.add_argument("--late-limit", type=int, default=LATE_LIMIT)
    ap.add_argument("--multi-inst", default="Matryoshka Guitar", help="instrument for the three-track test (one that sums correctly)")
    ap.add_argument("--interference", action="append", default=None, help="instruments to check for instance interference")
    ap.add_argument("--ara", default=r"C:\Program Files\Common Files\VST3\Celemony\Melodyne\Melodyne.vst3",
                    help="an ARA plug-in (VST3) for the ARA playback test; skipped if it is not there")
    ap.add_argument("--master-fx", action="append", default=None, help="master effects whose tail is measured (default: Spring Reverb, Tape Echoes)")
    a = ap.parse_args()
    a.fx = a.fx or [a.insert]
    a.ara_name = os.path.splitext(os.path.basename(a.ara))[0]
    a.master_fx = a.master_fx if a.master_fx is not None else ["Spring Reverb", "Tape Echoes"]
    a.interference = a.interference if a.interference is not None else ["Matryoshka Guitar", "Retrophie SN", "PAGANIHANDS", "Flesh808", "Bass Cafeteria"]
    notes = []
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

    # 7, 8: master tail, three tracks
    tail_check(rig, a, bad, notes)
    three_tracks(rig, a, bad)

    # 10-12: what the host hands the plug-in on each call
    variable_blocks(rig, a, bad, notes)
    transport_info(rig, bad)
    block_automation(rig, bad)
    audio_clip(rig, bad)
    ara_playback(rig, a, bad, notes)
    ara_variation(rig, a, bad, notes)

    # 9: instance interference (findings about plugins)
    instance_interference(rig, a.interference, notes)

    for n_ in notes:
        print("  NOTE (a finding about a plugin, not a failure of the rig):", n_)
    print("PASS" if not bad else "FAIL: " + "; ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
