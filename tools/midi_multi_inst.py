#!/usr/bin/env python3
"""MIDI-file bounce with more than one instrument track (docs/TRACKS.md): INST 1 listens to channel 1, INST 2 to channel 2.

  python tools/midi_multi_inst.py

The bench bounces the instrument tracks together through the rig. Checks:
  - the bounce is not silent and holds both instruments (INST 1 alone and INST 2 alone are each part of it);
  - the two instruments alone add up to the bounce of both (INST 1 alone + INST 2 alone == both: the mix is a plain sum).
Control: INST 1 alone and INST 2 alone must differ from each other.
"""
import os, struct, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from tb import TestBench
from live_vs_rig import read_wav


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def make_midi(path):
    """ch1: a held A3 and E4 (a melodic part); ch2: four kicks on note 36. 480 ticks per beat, 120 bpm."""
    ev = []   # (tick, bytes)
    for note, start, dur in ((57, 0, 1800), (64, 0, 1800)):
        ev.append((start, bytes([0x90, note, 100])))
        ev.append((start + dur, bytes([0x80, note, 0])))
    for k in range(4):
        ev.append((k * 480, bytes([0x91, 36, 120])))
        ev.append((k * 480 + 240, bytes([0x81, 36, 0])))
    ev.sort(key=lambda e: (e[0], e[1][0] & 0xF0 == 0x80 and -1 or 0))
    data, last = b"", 0
    for tick, msg in ev:
        data += vlq(tick - last) + msg
        last = tick
    data += vlq(0) + b"\xff\x2f\x00"
    trk = b"MTrk" + struct.pack(">I", len(data)) + data
    open(path, "wb").write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + trk)


def bounce(tb, midi):
    """load the MIDI file (which bounces through the instrument tracks) and return the bounced wav's samples"""
    assert tb.call("load_midi", path=midi).get("ok")
    tb.wait_idle()
    wav = tb.call("status")["file"]["playable_wav"]
    x, _ = read_wav(wav)
    return x, wav


def main():
    tb = TestBench()
    for r in ("fx", "inst", "inst2", "inst3"):
        tb.call("remove_plugin", role=r)
    tb.wait_idle()
    midi = os.path.join(os.environ.get("TEMP", "."), "multi_inst.mid")
    make_midi(midi)
    assert tb.call("load_plugin", role="inst", name="PAGANIHANDS").get("ok"); tb.wait_idle()
    assert tb.call("load_plugin", role="inst2", name="Flesh808").get("ok"); tb.wait_idle()
    tb.call("track_set", track="inst1", midi_channels=[1], mute=False)
    tb.call("track_set", track="inst2", midi_channels=[2], mute=False)

    both, wboth = bounce(tb, midi)
    print("both instruments:", wboth.split("\\")[-1], f"{len(both)} samples, peak {abs(both).max():.3f}")
    tb.call("track_set", track="inst2", mute=True)
    one, _ = bounce(tb, midi)
    tb.call("track_set", track="inst2", mute=False)
    tb.call("track_set", track="inst1", mute=True)
    two, _ = bounce(tb, midi)
    tb.call("track_set", track="inst1", mute=False)
    print(f"INST 1 alone peak {abs(one).max():.3f}, INST 2 alone peak {abs(two).max():.3f}")

    n = min(len(both), len(one), len(two))
    summ = one[:n] + two[:n]
    err = np.sqrt(np.mean((both[:n] - summ) ** 2))
    ref = np.sqrt(np.mean(both[:n] ** 2))
    res = 20 * np.log10(max(err, 1e-12) / max(ref, 1e-12))
    diff = 20 * np.log10(max(np.sqrt(np.mean((one[:n] - two[:n]) ** 2)), 1e-12) / max(ref, 1e-12))
    print(f"both vs INST 1 + INST 2 alone: residual {res:.1f} dB re the mix (limit -100); control INST 1 vs INST 2: {diff:.1f} dB (must stay above -30)")
    ok = res <= -100 and diff > -30 and abs(both).max() > 0.01 and abs(one).max() > 0.01 and abs(two).max() > 0.01
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
