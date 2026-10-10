#!/usr/bin/env python3
"""Live bench against the offline rig (docs/TRACKS.md, P3): the same file through the same master FX, once played and
recorded by the live graph, once rendered by `rig_render`. Both are built by MixGraph, so the audio must be the same
up to a shift in time (the live recording starts wherever the device was); the residual after aligning is the test.

  python tools/live_vs_rig.py [--fx "Legacy Distortion"] [--param Drive=0.7] [--seconds 3]

Control (must FAIL): the same comparison against a render whose FX parameter differs; otherwise the test could not
tell a wrong graph from a right one and its pass would mean nothing.
"""
import argparse
import os
import struct
import sys
import tempfile
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench  # noqa: E402


def write_wav(path, x, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())


def read_wav(path):
    """16/24/32-bit PCM or 32-bit float WAV -> float array (n, 2)"""
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF"
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(pcm, "<f4").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(pcm, "<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        b = np.frombuffer(pcm, np.uint8).reshape(-1, 3)
        v = b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v / 8388608.0
    else:
        raise SystemExit(f"unsupported wav format {tag}/{bits}")
    return x.reshape(-1, ch), rate


def test_signal(rate, seconds):
    n = int(rate * seconds)
    t = np.arange(n) / rate
    rng = np.random.default_rng(7)
    x = np.zeros((n, 2))
    for f, a in ((110, 0.20), (440, 0.18), (1760, 0.10), (5000, 0.05)):
        x[:, 0] += a * np.sin(2 * np.pi * f * t)
        x[:, 1] += a * np.sin(2 * np.pi * f * 1.01 * t + 0.4)
    x += 0.02 * rng.standard_normal((n, 2))
    for k in range(1, 6):                       # clicks: the FX sees transients and decays
        i = int(k * n / 6)
        x[i:i + 40] += 0.5 * np.hanning(40)[:, None]
    return np.clip(x, -0.95, 0.95)


def align_residual_db(live, rig, search=20000):
    """Shift `rig` against `live` by the integer lag that matches best, compare the common steady stretch.
    Returns (lag, residual dB re the live signal's RMS)."""
    a = live[:, 0]
    b = rig[:, 0]
    seg = a[len(a) // 4: len(a) // 4 + 20000]
    best, lag = -1.0, 0
    for s in range(-search, search):
        lo = len(a) // 4 + s
        if lo < 0 or lo + len(seg) > len(b):
            continue
        c = float(np.dot(seg, b[lo:lo + len(seg)]))
        if c > best:
            best, lag = c, s
    # live[i] == rig[i + lag]
    n = min(len(a) - max(0, -lag), len(b) - max(0, lag)) - 1
    i0 = max(0, -lag)
    la = live[i0 + 5000: i0 + n - 5000]
    rb = rig[i0 + lag + 5000: i0 + lag + n - 5000]
    k = min(len(la), len(rb))
    la, rb = la[:k], rb[:k]
    rms = np.sqrt(np.mean(la ** 2))
    res = np.sqrt(np.mean((la - rb) ** 2))
    return lag, 20 * np.log10(max(res, 1e-12) / rms)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fx", default="Legacy Distortion")
    ap.add_argument("--param", default="Drive=0.7")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--limit", type=float, default=-100.0)
    args = ap.parse_args()
    pname, pval = args.param.split("=")
    pval = float(pval)

    tb = TestBench()
    tmp = tempfile.gettempdir()
    for r in ("fx", "inst", "inst2", "inst3"):
        tb.call("remove_plugin", role=r)
    tb.wait_idle()
    tb.call("set_source", mode="file")
    tb.wait_idle()
    rate = int(tb.call("status")["audio"]["sample_rate"])
    block = int(tb.call("status")["audio"]["buffer"])
    print(f"device: {rate} Hz, buffer {block}")

    src = os.path.join(tmp, "live_vs_rig_src.wav")
    write_wav(src, test_signal(rate, args.seconds), rate)

    # --- live ---
    assert tb.call("load_plugin", role="fx", name=args.fx).get("ok")
    tb.wait_idle()
    assert tb.call("set_param", role="fx", name=pname, value=pval).get("ok")
    assert tb.call("load_audio", path=src).get("ok")
    tb.wait_idle()
    tb.call("set_source", mode="file")
    tb.wait_idle()
    rec = os.path.join(tmp, "live_vs_rig_live.wav")
    tb.call("record_start", path=rec)
    time.sleep(0.4)
    tb.call("play", **{"from": 0.0})
    time.sleep(args.seconds + 1.0)
    tb.call("stop")
    time.sleep(0.3)
    tb.call("record_stop")
    live, lrate = read_wav(rec)
    print(f"live recording: {len(live) / lrate:.2f} s")

    # --- rig ---
    def rig(out, value):
        r = tb.call("rig_render", source="file", source_path=src, master=args.fx, master_params={pname: value},
                    out=out, rate=rate, block=block, tail=1.0, compensate=False)
        assert r.get("ok"), r
        tb.wait_idle()

    ok_path = os.path.join(tmp, "live_vs_rig_rig.wav")
    bad_path = os.path.join(tmp, "live_vs_rig_wrong.wav")
    rig(ok_path, pval)
    rig(bad_path, max(0.0, pval - 0.25))
    rigx, _ = read_wav(ok_path)
    badx, _ = read_wav(bad_path)

    lag, res = align_residual_db(live, rigx)
    lag_b, res_b = align_residual_db(live, badx)
    print(f"live vs rig:          lag {lag} samples, residual {res:.1f} dB re signal (limit {args.limit})")
    print(f"control, FX {pname} off by 0.25: residual {res_b:.1f} dB (must stay above -60)")
    good = res <= args.limit and res_b > -60.0
    print("PASS" if good else "FAIL")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
